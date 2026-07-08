/*
 * user/vkgui.c  --  VKGUI.ELF: eine kleine Vulkan-Demo IN einer WinForms-GUI-App.
 *
 * Verbindet die beiden Subsysteme dieses Projekts:
 *   - WinForms (winforms.h) + libgui (gui.h) fuer das Fenster-Chrome (Titelleiste, gerahmter
 *     Viewport, Buttons, Statuszeile) und die Eingabe (Maus ueber die Kernel-Event-Queue).
 *   - Die Software-Vulkan-Lib (vk/vk_rtos.h) rendert OFFSCREEN einen rotierenden, beleuchteten
 *     Wuerfel (Push-Constant-MVP, D32-Tiefentest, Cull-Back) in ein eigenes VkImage.
 *
 * Je Frame: Vulkan zeichnet in das Offscreen-Farbbild -> die Pixel (B8G8R8A8 = 0xAARRGGBB) werden
 * mit `& 0xFFFFFF` (0xRRGGBB) direkt in den GUI-Backbuffer an der Viewport-Position geblittet ->
 * gui_flush schiebt nur die Viewport-Zeilen auf den Schirm. Die Buttons steuern die Demo zur
 * Laufzeit (Rotation an/aus, Tint, Beenden). Statt der blockierenden wf_run-Loop laeuft eine
 * eigene Render-Schleife, die wf_pump (nicht-blockierend) fuer die GUI-Events nutzt.
 *
 * MIT FP kompiliert (GUI_FP); braucht USER_CAP_GUI. Shader (.spv) werden zur Laufzeit von hdd1
 * geladen (dieselben wie vkcube).
 */
#include "abi.h"
#include "ulib.h"
#include "gui.h"
#include "winforms.h"
#include "r3d.h"                 /* Matrix-Helfer (Column-Major) + sin/cos */
#include "vk/vk_rtos.h"

void *memset(void *dst, int c, unsigned long n);

/* --- Serial-Ausgabe-Helfer --- */
static char lbuf[200];
static int  lpos;
static void lflush(void) { if (lpos > 0) { sys3(SYS_WRITE, 1, (long)lbuf, lpos); lpos = 0; } }
static void uwrite(const char *s)
{
    for (; *s; ++s) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = *s; } if (*s == '\n') { lflush(); } }
}
static void fmt_int(char *buf, int v)
{
    char tmp[12]; int i = 0, p = 0, neg = (v < 0);
    if (neg) { v = -v; }
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) { buf[p++] = '-'; }
    while (i > 0) { buf[p++] = tmp[--i]; }
    buf[p] = 0;
}

/* --- Viewport-Geometrie (in das 640x480-Formular eingepasst) --- */
#define VW 400              /* Vulkan-Render-Breite  */
#define VH 300              /* Vulkan-Render-Hoehe   */
#define VX 20               /* Blit-Position links   */
#define VY 52               /* Blit-Position oben    */

/* --- Device-Heap: Farbbild (VWxVH B8G8R8A8) + Tiefe (VWxVH D32) + Vertex-Buffer + Reserve --- */
static unsigned char g_vkheap[1600 * 1024] __attribute__((aligned(64)));

/* --- Shader von hdd1 (echte .spv, 4-Byte-aligned) --- */
static unsigned g_vert_spv[512];
static unsigned g_frag_spv[512];

/* --- Wuerfel: 36 Vertices (pos vec3 + farbe vec3), CCW von aussen (wie vkcube) --- */
static const float C[8][3] = {
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
    {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1}
};
static const int   F[6][4] = { {4,5,6,7}, {1,0,3,2}, {5,1,2,6}, {0,4,7,3}, {7,6,2,3}, {0,1,5,4} };
static float FC[6][3] = {
    {0.86f,0.30f,0.30f}, {0.30f,0.62f,0.86f}, {0.32f,0.78f,0.38f},
    {0.88f,0.76f,0.30f}, {0.72f,0.40f,0.82f}, {0.90f,0.56f,0.28f}
};
static float g_verts[36][6];

static void build_cube(void)
{
    int n = 0;
    for (int f = 0; f < 6; f++) {
        int idx[6] = { F[f][0], F[f][1], F[f][2], F[f][0], F[f][2], F[f][3] };
        for (int k = 0; k < 6; k++) {
            const float *p = C[idx[k]];
            g_verts[n][0] = p[0] * 0.9f;
            g_verts[n][1] = p[1] * 0.9f;
            g_verts[n][2] = p[2] * 0.9f;
            g_verts[n][3] = FC[f][0];
            g_verts[n][4] = FC[f][1];
            g_verts[n][5] = FC[f][2];
            n++;
        }
    }
}

static int load_spv(const char *path, unsigned *buf, unsigned cap_words, unsigned *out_words)
{
    long n = sys3(SYS_READ_FILE, (long)path, (long)buf, (long)(cap_words * 4));
    if (n <= 0 || (n % 4) != 0 || (unsigned)n > cap_words * 4) { return -1; }
    if (buf[0] != 0x07230203u) { return -2; }
    *out_words = (unsigned)n / 4;
    return 0;
}

/* --- Farben (klassischer WinForms-Look) --- */
#define BG_FORM   0xD4D0C8u
#define TITLE_BAR 0x0A246Au
#define COL_TITLE 0xFFFFFFu
#define COL_SECT  0x000080u
#define COL_TEXT  0x000000u

/* --- Sitzungszustand (Buttons greifen darauf zu) --- */
static wf_form_t    *g_form;
static wf_control_t *g_lblStatus;
static int           g_rotate = 1;         /* Rotation an/aus */
static int           g_running = 1;        /* Render-Schleife laeuft */

static void set_status(const char *s) { if (g_lblStatus) { g_lblStatus->text = s; } if (g_form) { g_form->dirty = 1; } }

static void on_toggle(wf_control_t *c) { (void)c; g_rotate = !g_rotate; set_status(g_rotate ? "Rotation: an." : "Rotation: PAUSE."); }
static void on_exit(wf_control_t *c)   { (void)c; g_running = 0; if (g_form) { wf_close(g_form); } }
/* Die drei Farb-Buttons tauschen die Wuerfel-Grundpalette (Tint der Flaechen). */
static void tint(float r, float gg, float b, const char *msg)
{
    for (int f = 0; f < 6; f++) {
        float k = 0.55f + 0.09f * (float)f;       /* je Flaeche leicht andere Helligkeit */
        FC[f][0] = r * k; FC[f][1] = gg * k; FC[f][2] = b * k;
    }
    build_cube();
    set_status(msg);
}
static void on_warm(wf_control_t *c)  { (void)c; tint(1.0f, 0.55f, 0.25f, "Palette: warm."); }
static void on_cool(wf_control_t *c)  { (void)c; tint(0.30f, 0.65f, 1.0f, "Palette: kuehl."); }
static void on_multi(wf_control_t *c)
{
    (void)c;
    static const float base[6][3] = {
        {0.86f,0.30f,0.30f}, {0.30f,0.62f,0.86f}, {0.32f,0.78f,0.38f},
        {0.88f,0.76f,0.30f}, {0.72f,0.40f,0.82f}, {0.90f,0.56f,0.28f}
    };
    for (int f = 0; f < 6; f++) { FC[f][0] = base[f][0]; FC[f][1] = base[f][1]; FC[f][2] = base[f][2]; }
    build_cube();
    set_status("Palette: bunt.");
}

void _start(void)
{
    gui_t g;
    if (gui_init(&g) != 0) {
        uwrite("[vkgui] gui_init fehlgeschlagen (keine Bruecke/GUI-Cap)\n");
        sys3(SYS_EXIT, 1, 0, 0); for (;;) { }
    }
    build_cube();
    vk_rtos_set_heap(g_vkheap, sizeof(g_vkheap));

    unsigned nv = 0, nf = 0;
    if (load_spv("hdd1:VERT.SPV", g_vert_spv, 512, &nv) != 0 ||
        load_spv("hdd1:FRAG.SPV", g_frag_spv, 512, &nf) != 0) {
        uwrite("[vkgui] FEHLER: .spv-Dateien nicht ladbar (hdd1:VERT.SPV/FRAG.SPV)\n");
        sys3(SYS_EXIT, 1, 0, 0); for (;;) { }
    }
    uwrite("[vkgui] shader geladen: hdd1:VERT.SPV + hdd1:FRAG.SPV\n");

    /* ================= Vulkan-Setup (offscreen, kein Swapchain) ================= */
    VkInstanceCreateInfo ici; memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkInstance inst = VK_NULL_HANDLE;
    int ok = (vkCreateInstance(&ici, 0, &inst) == VK_SUCCESS);

    uint32_t nd = 1; VkPhysicalDevice pd = VK_NULL_HANDLE;
    ok = ok && (vkEnumeratePhysicalDevices(inst, &nd, &pd) == VK_SUCCESS);

    VkDeviceQueueCreateInfo qci; memset(&qci, 0, sizeof(qci));
    float prio = 1.0f;
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci; memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    ok = ok && (vkCreateDevice(pd, &dci, 0, &dev) == VK_SUCCESS);
    VkQueue queue = VK_NULL_HANDLE;
    if (ok) { vkGetDeviceQueue(dev, 0, 0, &queue); }

    VkMemoryAllocateInfo mai; memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;

    /* --- Offscreen-Farbbild (B8G8R8A8, LINEAR -> mappbar) --- */
    VkImageCreateInfo cic; memset(&cic, 0, sizeof(cic));
    cic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; cic.imageType = VK_IMAGE_TYPE_2D;
    cic.format = VK_FORMAT_B8G8R8A8_UNORM;
    cic.extent.width = VW; cic.extent.height = VH; cic.extent.depth = 1;
    cic.mipLevels = 1; cic.arrayLayers = 1; cic.samples = VK_SAMPLE_COUNT_1_BIT;
    cic.tiling = VK_IMAGE_TILING_LINEAR;
    cic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage cimg = VK_NULL_HANDLE;
    ok = ok && (vkCreateImage(dev, &cic, 0, &cimg) == VK_SUCCESS);
    VkMemoryRequirements cmr; vkGetImageMemoryRequirements(dev, cimg, &cmr);
    mai.allocationSize = cmr.size;
    VkDeviceMemory cmem = VK_NULL_HANDLE;
    ok = ok && (vkAllocateMemory(dev, &mai, 0, &cmem) == VK_SUCCESS) &&
               (vkBindImageMemory(dev, cimg, cmem, 0) == VK_SUCCESS);
    VkImageViewCreateInfo vci; memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    vci.image = cimg; vci.format = VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageView cview = VK_NULL_HANDLE;
    ok = ok && (vkCreateImageView(dev, &vci, 0, &cview) == VK_SUCCESS);

    /* --- Tiefenbild (D32) --- */
    VkImageCreateInfo dic = cic;
    dic.format = VK_FORMAT_D32_SFLOAT; dic.tiling = VK_IMAGE_TILING_OPTIMAL;
    dic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage dimg = VK_NULL_HANDLE;
    ok = ok && (vkCreateImage(dev, &dic, 0, &dimg) == VK_SUCCESS);
    VkMemoryRequirements dmr; vkGetImageMemoryRequirements(dev, dimg, &dmr);
    mai.allocationSize = dmr.size;
    VkDeviceMemory dmem = VK_NULL_HANDLE;
    ok = ok && (vkAllocateMemory(dev, &mai, 0, &dmem) == VK_SUCCESS) &&
               (vkBindImageMemory(dev, dimg, dmem, 0) == VK_SUCCESS);
    vci.image = dimg; vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageView dview = VK_NULL_HANDLE;
    ok = ok && (vkCreateImageView(dev, &vci, 0, &dview) == VK_SUCCESS);

    /* --- RenderPass (Farbe CLEAR/STORE + Tiefe CLEAR) + Framebuffer --- */
    VkAttachmentDescription att[2]; memset(att, 0, sizeof(att));
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    att[1].format = VK_FORMAT_D32_SFLOAT; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref, dref; memset(&cref, 0, sizeof(cref)); memset(&dref, 0, sizeof(dref));
    cref.attachment = 0; cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dref.attachment = 1; dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub; memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref; sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpi; memset(&rpi, 0, sizeof(rpi));
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 2; rpi.pAttachments = att; rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    ok = ok && (vkCreateRenderPass(dev, &rpi, 0, &rp) == VK_SUCCESS);

    VkImageView fbat[2] = { cview, dview };
    VkFramebufferCreateInfo fbi; memset(&fbi, 0, sizeof(fbi));
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = rp; fbi.attachmentCount = 2; fbi.pAttachments = fbat;
    fbi.width = VW; fbi.height = VH; fbi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    ok = ok && (vkCreateFramebuffer(dev, &fbi, 0, &fb) == VK_SUCCESS);

    /* --- Shader/Layout/Pipeline (Push-Constant-MVP, Cull-Back, Depth-Less) --- */
    VkShaderModuleCreateInfo smi; memset(&smi, 0, sizeof(smi));
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = nv * 4; smi.pCode = g_vert_spv;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    ok = ok && (vkCreateShaderModule(dev, &smi, 0, &vs) == VK_SUCCESS);
    smi.codeSize = nf * 4; smi.pCode = g_frag_spv;
    ok = ok && (vkCreateShaderModule(dev, &smi, 0, &fs) == VK_SUCCESS);

    VkPushConstantRange pcr; memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.size = 64;
    VkPipelineLayoutCreateInfo pli; memset(&pli, 0, sizeof(pli));
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    ok = ok && (vkCreatePipelineLayout(dev, &pli, 0, &layout) == VK_SUCCESS);

    VkPipelineShaderStageCreateInfo stages[2]; memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription bind; memset(&bind, 0, sizeof(bind)); bind.stride = 24;
    VkVertexInputAttributeDescription attrs[2]; memset(attrs, 0, sizeof(attrs));
    attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = 12;
    VkPipelineVertexInputStateCreateInfo vis; memset(&vis, 0, sizeof(vis));
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount = 1; vis.pVertexBindingDescriptions = &bind;
    vis.vertexAttributeDescriptionCount = 2; vis.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ias; memset(&ias, 0, sizeof(ias));
    ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp; VkRect2D scs; memset(&vp, 0, sizeof(vp)); memset(&scs, 0, sizeof(scs));
    vp.width = (float)VW; vp.height = (float)VH; vp.maxDepth = 1.0f;
    scs.extent.width = VW; scs.extent.height = VH;
    VkPipelineViewportStateCreateInfo vps; memset(&vps, 0, sizeof(vps));
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scs;
    VkPipelineRasterizationStateCreateInfo ras; memset(&ras, 0, sizeof(ras));
    ras.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ras.cullMode = VK_CULL_MODE_BACK_BIT; ras.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; ras.lineWidth = 1.0f;
    VkPipelineDepthStencilStateCreateInfo dss; memset(&dss, 0, sizeof(dss));
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_LESS;
    VkGraphicsPipelineCreateInfo gpi; memset(&gpi, 0, sizeof(gpi));
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vis; gpi.pInputAssemblyState = &ias; gpi.pViewportState = &vps;
    gpi.pRasterizationState = &ras; gpi.pDepthStencilState = &dss;
    gpi.layout = layout; gpi.renderPass = rp;
    VkPipeline pipe = VK_NULL_HANDLE;
    ok = ok && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &pipe) == VK_SUCCESS);

    /* --- Vertex-Buffer (persistent gemappt, damit der Tint-Wechsel sofort greift) --- */
    VkBufferCreateInfo bci; memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bci.size = sizeof(g_verts);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vbuf = VK_NULL_HANDLE;
    ok = ok && (vkCreateBuffer(dev, &bci, 0, &vbuf) == VK_SUCCESS);
    VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev, vbuf, &br);
    mai.allocationSize = br.size;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    ok = ok && (vkAllocateMemory(dev, &mai, 0, &vmem) == VK_SUCCESS) &&
               (vkBindBufferMemory(dev, vbuf, vmem, 0) == VK_SUCCESS);
    void *vmap = 0;
    ok = ok && (vkMapMemory(dev, vmem, 0, VK_WHOLE_SIZE, 0, &vmap) == VK_SUCCESS);

    /* --- CommandPool/-Buffer + Fence + Zugriff auf die Farbbild-Pixel --- */
    VkCommandPoolCreateInfo cpi; memset(&cpi, 0, sizeof(cpi));
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    VkCommandPool cpool = VK_NULL_HANDLE;
    ok = ok && (vkCreateCommandPool(dev, &cpi, 0, &cpool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cbi; memset(&cbi, 0, sizeof(cbi));
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = cpool; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ok = ok && (vkAllocateCommandBuffers(dev, &cbi, &cmd) == VK_SUCCESS);
    VkFenceCreateInfo fci; memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    ok = ok && (vkCreateFence(dev, &fci, 0, &fence) == VK_SUCCESS);
    void *cpixels = 0;
    ok = ok && (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cpixels) == VK_SUCCESS);

    uwrite(ok ? "[vkgui] vulkan-setup ok (offscreen farbbild + tiefe + pipeline aus .spv)\n"
              : "[vkgui] FEHLER: vulkan-setup\n");
    if (!ok) { sys3(SYS_EXIT, 1, 0, 0); for (;;) { } }

    /* --- Projektion/View (r3d, Vulkan-Clip) --- */
    r3d_mat4 proj, view, vpm;
    r3d_mat4_perspective(&proj, 1.0471976f, (float)VW / (float)VH, 0.1f, 10.0f);
    r3d_mat4_translate(&view, 0.0f, 0.0f, -3.6f);
    r3d_mat4_mul(&vpm, &proj, &view);

    /* ================= WinForms-Chrome ================= */
    wf_form_t form; g_form = &form;
    wf_form_init(&form, &g, BG_FORM, 0);
    wf_add_panel(&form, 0, 0, 640, 34, TITLE_BAR);
    wf_control_t *tl = wf_add_label(&form, 12, 9, 480, 16, "rpi_rtos  -  Vulkan-Demo in WinForms", COL_TITLE);
    if (tl) { tl->back = TITLE_BAR; }

    /* Gerahmter Viewport (versenkt) -- der Vulkan-Wuerfel wird in sein Inneres geblittet. */
    wf_control_t *vppanel = wf_add_panel(&form, VX - 4, VY - 4, VW + 8, VH + 8, 0x101018u);
    if (vppanel) { vppanel->style = WF_STYLE_SUNKEN; }

    /* Rechte Steuer-Spalte. */
    wf_add_label(&form, 444, 48, 180, 12, "Software-Vulkan rendert", COL_SECT);
    wf_add_label(&form, 444, 62, 180, 12, "einen rotierenden Wuerfel", COL_TEXT);
    wf_add_label(&form, 444, 76, 180, 12, "offscreen -> ins Panel.", COL_TEXT);

    wf_add_button(&form, 444, 108, 180, 30, "Rotation an/aus", on_toggle);
    wf_add_label(&form, 444, 150, 180, 12, "Palette:", COL_SECT);
    wf_add_button(&form, 444, 168, 56, 28, "bunt",  on_multi);
    wf_add_button(&form, 506, 168, 56, 28, "warm",  on_warm);
    wf_add_button(&form, 568, 168, 56, 28, "kuehl", on_cool);
    wf_add_button(&form, 444, 342, 180, 32, "Beenden", on_exit);

    g_lblStatus = wf_add_label(&form, 16, 402, 470, 12, "Bereit -- Maus im Fenster. 'Beenden' schliesst.", COL_TEXT);
    wf_add_label(&form, 16, 420, 470, 12, "Live-FPS:", COL_TEXT);

    wf_paint(&form);
    uwrite("[vkgui] chrome gezeichnet, starte render-schleife\n");

    /* ================= Render-Schleife ================= */
    float ang = 0.0f;
    unsigned long frames = 0;
    char infobuf[64];
    unsigned gwpr = g.wpr;                      /* Backbuffer-Woerter je Zeile */
    unsigned ipix = VW;                         /* Farbbild-Woerter je Zeile (LINEAR, pitch = VW*4) */
    int probed = 0;

    while (g_running) {
        /* GUI-Events (Buttons) verarbeiten; nach Chrome-Aenderung neu zeichnen. */
        while (wf_pump(&form)) { }
        if (form.dirty) { wf_paint(&form); form.dirty = 0; }
        if (!g_running) { break; }

        /* Vertexdaten (evtl. neu getintet) hochladen. */
        {
            const unsigned char *s = (const unsigned char *)g_verts;
            unsigned char *d = (unsigned char *)vmap;
            for (unsigned i = 0; i < sizeof(g_verts); i++) { d[i] = s[i]; }
        }

        /* MVP mit Rotation. */
        r3d_mat4 rx, ry, mm, mvp;
        r3d_mat4_rot_y(&ry, ang);
        r3d_mat4_rot_x(&rx, ang * 0.61f);
        r3d_mat4_mul(&mm, &ry, &rx);
        r3d_mat4_mul(&mvp, &vpm, &mm);
        if (g_rotate) { ang += 0.05f; }

        /* Offscreen zeichnen. */
        VkCommandBufferBeginInfo cbb; memset(&cbb, 0, sizeof(cbb));
        cbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &cbb);
        VkClearValue clears[2]; memset(clears, 0, sizeof(clears));
        clears[0].color.float32[0] = 0.05f; clears[0].color.float32[1] = 0.07f;
        clears[0].color.float32[2] = 0.13f; clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil.depth = 1.0f;
        VkRenderPassBeginInfo rbi; memset(&rbi, 0, sizeof(rbi));
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = rp; rbi.framebuffer = fb;
        rbi.renderArea.extent.width = VW; rbi.renderArea.extent.height = VH;
        rbi.clearValueCount = 2; rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize zoff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zoff);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp.m);
        vkCmdDraw(cmd, 36, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si; memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        vkResetFences(dev, 1, &fence);

        /* Farbbild -> GUI-Backbuffer an (VX,VY): B8G8R8A8 (0xAARRGGBB) -> 0xRRGGBB per & 0xFFFFFF. */
        {
            const unsigned *src = (const unsigned *)cpixels;
            volatile unsigned *bb = g.bb;
            for (unsigned y = 0; y < VH; y++) {
                const unsigned *sr = src + (unsigned)y * ipix;
                volatile unsigned *dr = bb + (unsigned)(VY + (int)y) * gwpr + VX;
                for (unsigned x = 0; x < VW; x++) { dr[x] = sr[x] & 0x00FFFFFFu; }
            }
        }
        gui_flush(&g, VY, VH);

        frames++;

        /* Alle 30 Frames die Info-Zeile direkt zeichnen (ohne die ganze Form neu zu malen). */
        if ((frames % 30u) == 0u) {
            int p = 0; char nb[12];
            for (const char *s = "Frames: "; *s; ++s) { infobuf[p++] = *s; }
            fmt_int(nb, (int)frames);
            for (char *s = nb; *s; ++s) { infobuf[p++] = *s; }
            for (const char *s = (g_rotate ? "    Rotation: an " : "    Rotation: PAUSE"); *s; ++s) { infobuf[p++] = *s; }
            infobuf[p] = 0;
            gui_fill_rect(&g, 16, 418, 470, 14, BG_FORM);
            gui_text(&g, 16, 420, infobuf, COL_TEXT, GUI_TRANSPARENT, 1);
            gui_flush(&g, 418, 14);
        }

        if (!probed && frames == 5) {
            /* Einmaliger Selbsttest-Marker: die Viewport-Mitte traegt ein Wuerfel-Pixel
             * (nicht die Clear-Farbe), d.h. Vulkan-Inhalt liegt im GUI-Backbuffer. */
            unsigned clearw = 0x000D1221u;   /* (0.05,0.07,0.13) -> 0xRRGGBB */
            unsigned mid = gui_get(&g, VX + VW / 2, VY + VH / 2);
            uwrite((mid != clearw && mid != 0u)
                   ? "[vkgui] probe: wuerfel im GUI-viewport sichtbar -> vulkan-in-gui ok\n"
                   : "[vkgui] probe: FEHLER (viewport-mitte leer)\n");
            probed = 1;
        }

        sys1(SYS_SLEEP_MS, 8);
    }

    uwrite("[vkgui] Sitzung beendet\n");
    sys3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
