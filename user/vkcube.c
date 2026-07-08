/*
 * user/vkcube.c  --  VKCUBE.ELF: echte Vulkan-App auf rpi_rtos.
 *
 * Der komplette kanonische Vulkan-Weg (Hommage an das klassische vkcube):
 *   vkCreateInstance (VK_KHR_surface + VK_RTOS_surface) -> vkCreateRtosSurfaceRTOS ->
 *   Surface-Caps/-Formate -> vkCreateDevice (VK_KHR_swapchain) -> vkCreateSwapchainKHR
 *   (Image = GUI-Backbuffer) -> Depth-Image (D32) -> RenderPass/Framebuffer ->
 *   Shader-Module aus .spv-DATEIEN (hdd1:VERT.SPV/FRAG.SPV, SYS_READ_FILE) ->
 *   Pipeline -> je Frame: vkAcquireNextImageKHR -> CommandBuffer (Clear + 36 Vertices,
 *   MVP via Push-Constants) -> vkQueueSubmit(Fence) -> vkWaitForFences ->
 *   vkQueuePresentKHR (SYS_GUI_FLUSH).
 * Nach VKCUBE_PROBE_FRAMES Frames: Selbsttest-Marker (Backbuffer-Probe) -- danach
 * laeuft die Animation weiter (visueller Modus), bis der Prozess beendet wird.
 * MIT FP kompiliert; braucht USER_CAP_GUI.
 */
#include "abi.h"
#include "ulib.h"
#include "gui.h"
#include "r3d.h"             /* nur Matrix-Helfer (Column-Major) + sin/cos */
#include "vk/vk_rtos.h"

void *memset(void *dst, int c, unsigned long n);

#define VKCUBE_PROBE_FRAMES 12

/* --- Ausgabe-Helfer --- */
static char lbuf[200];
static int  lpos;
static void lflush(void) { if (lpos > 0) { sys3(SYS_WRITE, 1, (long)lbuf, lpos); lpos = 0; } }
static void uwrite(const char *s)
{
    for (; *s; ++s) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = *s; } if (*s == '\n') { lflush(); } }
}
static void fmt_u(char *buf, unsigned long v)
{
    char tmp[24]; int i = 0, p = 0;
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i > 0) { buf[p++] = tmp[--i]; }
    buf[p] = 0;
}

/* --- Device-Heap (Plattform-Bootstrap): Depth 640x480 D32 = 1,17 MiB + Buffer --- */
static unsigned char g_vkheap[1344 * 1024] __attribute__((aligned(64)));

/* --- Shader von hdd1 (echte .spv-Dateien, 4-Byte-aligned Puffer) --- */
static unsigned g_vert_spv[512];
static unsigned g_frag_spv[512];

/* --- Wuerfel: 36 Vertices (pos vec3 + farbe vec3), CCW von aussen --- */
static const float C[8][3] = {
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
    {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1}
};
static const int   F[6][4] = { {4,5,6,7}, {1,0,3,2}, {5,1,2,6}, {0,4,7,3}, {7,6,2,3}, {0,1,5,4} };
static const float FC[6][3] = {
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
            g_verts[n][0] = p[0] * 0.8f;
            g_verts[n][1] = p[1] * 0.8f;
            g_verts[n][2] = p[2] * 0.8f;
            g_verts[n][3] = FC[f][0];
            g_verts[n][4] = FC[f][1];
            g_verts[n][5] = FC[f][2];
            n++;
        }
    }
}

/* Mehrpunkt-Hash der zentralen Bildregion (5x5-Gitter, 20px-Abstand). */
static unsigned vkc_region_hash(const gui_t *g, int cx, int cy)
{
    unsigned h = 2166136261u;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            unsigned p = gui_get(g, cx + dx * 20, cy + dy * 20);
            h = (h ^ p) * 16777619u;
        }
    }
    return h;
}

static int load_spv(const char *path, unsigned *buf, unsigned cap_words, unsigned *out_words)
{
    long n = sys3(SYS_READ_FILE, (long)path, (long)buf, (long)(cap_words * 4));
    if (n <= 0 || (n % 4) != 0 || (unsigned)n > cap_words * 4) { return -1; }
    if (buf[0] != 0x07230203u) { return -2; }
    *out_words = (unsigned)n / 4;
    return 0;
}

void _start(void)
{
    char nb[24];
    build_cube();
    vk_rtos_set_heap(g_vkheap, sizeof(g_vkheap));

    /* --- Shader-Dateien laden (EL0-Policy: hdd1) --- */
    unsigned nv = 0, nf = 0;
    if (load_spv("hdd1:VERT.SPV", g_vert_spv, 512, &nv) != 0 ||
        load_spv("hdd1:FRAG.SPV", g_frag_spv, 512, &nf) != 0) {
        uwrite("[vkcube] FEHLER: .spv-Dateien nicht ladbar (hdd1:VERT.SPV/FRAG.SPV)\n");
        sys3(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    uwrite("[vkcube] shader geladen: hdd1:VERT.SPV + hdd1:FRAG.SPV (SPIR-V-Magic ok)\n");

    /* --- Instance mit Surface-Extensions --- */
    const char *iext[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_RTOS_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    VkInstance inst = VK_NULL_HANDLE;
    int ok = (vkCreateInstance(&ici, 0, &inst) == VK_SUCCESS);

    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkRtosSurfaceCreateInfoRTOS sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_RTOS_SURFACE_CREATE_INFO_RTOS;
    ok = ok && (vkCreateRtosSurfaceRTOS(inst, &sci, 0, &surf) == VK_SUCCESS);

    uint32_t nd = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    ok = ok && (vkEnumeratePhysicalDevices(inst, &nd, &pd) == VK_SUCCESS);

    VkBool32 sup = VK_FALSE;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR sfmt;
    uint32_t one = 1;
    ok = ok && (vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &sup) == VK_SUCCESS) && sup;
    ok = ok && (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps) == VK_SUCCESS);
    ok = ok && (vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &one, &sfmt) == VK_SUCCESS);
    unsigned W = ok ? caps.currentExtent.width : 0;
    unsigned H = ok ? caps.currentExtent.height : 0;

    /* --- Device mit VK_KHR_swapchain --- */
    const char *dext[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VkDevice dev = VK_NULL_HANDLE;
    ok = ok && (vkCreateDevice(pd, &dci, 0, &dev) == VK_SUCCESS);
    VkQueue q = VK_NULL_HANDLE;
    if (ok) { vkGetDeviceQueue(dev, 0, 0, &q); }

    /* --- Swapchain (1 Image = GUI-Backbuffer) + View --- */
    VkSwapchainCreateInfoKHR swi;
    memset(&swi, 0, sizeof(swi));
    swi.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swi.surface = surf;
    swi.minImageCount = 1;
    swi.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swi.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swi.imageExtent = caps.currentExtent;
    swi.imageArrayLayers = 1;
    swi.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swi.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swi.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swi.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swi.clipped = VK_TRUE;
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    ok = ok && (vkCreateSwapchainKHR(dev, &swi, 0, &swap) == VK_SUCCESS);
    uint32_t nimg = 1;
    VkImage scimg = VK_NULL_HANDLE;
    ok = ok && (vkGetSwapchainImagesKHR(dev, swap, &nimg, &scimg) == VK_SUCCESS) && (nimg == 1);

    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vci.image = scimg;
    vci.format = VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageView scview = VK_NULL_HANDLE;
    ok = ok && (vkCreateImageView(dev, &vci, 0, &scview) == VK_SUCCESS);
    uwrite("[vkcube] swapchain=");
    uwrite(ok ? "ok" : "FEHLER");
    uwrite(" (VK_KHR_surface/VK_RTOS_surface/VK_KHR_swapchain, 1 image = backbuffer)\n");
    if (!ok) { sys3(SYS_EXIT, 1, 0, 0); for (;;) { } }

    /* --- Depth-Image (D32) aus dem Device-Heap --- */
    VkImageCreateInfo ic;
    memset(&ic, 0, sizeof(ic));
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.extent.width = W; ic.extent.height = H; ic.extent.depth = 1;
    ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.format = VK_FORMAT_D32_SFLOAT;
    ic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage dimg = VK_NULL_HANDLE;
    ok = (vkCreateImage(dev, &ic, 0, &dimg) == VK_SUCCESS);
    VkMemoryRequirements dr;
    vkGetImageMemoryRequirements(dev, dimg, &dr);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = dr.size;
    VkDeviceMemory dmem = VK_NULL_HANDLE;
    ok = ok && (vkAllocateMemory(dev, &mai, 0, &dmem) == VK_SUCCESS) &&
               (vkBindImageMemory(dev, dimg, dmem, 0) == VK_SUCCESS);
    vci.image = dimg;
    vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageView dview = VK_NULL_HANDLE;
    ok = ok && (vkCreateImageView(dev, &vci, 0, &dview) == VK_SUCCESS);

    /* --- RenderPass + Framebuffer --- */
    VkAttachmentDescription att[2];
    memset(att, 0, sizeof(att));
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref, dref;
    memset(&cref, 0, sizeof(cref)); memset(&dref, 0, sizeof(dref));
    cref.attachment = 0; cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dref.attachment = 1; dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpi;
    memset(&rpi, 0, sizeof(rpi));
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 2; rpi.pAttachments = att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    ok = ok && (vkCreateRenderPass(dev, &rpi, 0, &rp) == VK_SUCCESS);

    VkImageView fbat[2] = { scview, dview };
    VkFramebufferCreateInfo fbi;
    memset(&fbi, 0, sizeof(fbi));
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = rp;
    fbi.attachmentCount = 2; fbi.pAttachments = fbat;
    fbi.width = W; fbi.height = H; fbi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    ok = ok && (vkCreateFramebuffer(dev, &fbi, 0, &fb) == VK_SUCCESS);

    /* --- Shader/Layout/Pipeline --- */
    VkShaderModuleCreateInfo smi;
    memset(&smi, 0, sizeof(smi));
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = nv * 4; smi.pCode = g_vert_spv;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    ok = ok && (vkCreateShaderModule(dev, &smi, 0, &vs) == VK_SUCCESS);
    smi.codeSize = nf * 4; smi.pCode = g_frag_spv;
    ok = ok && (vkCreateShaderModule(dev, &smi, 0, &fs) == VK_SUCCESS);

    VkPushConstantRange pcr;
    memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.size = 64;
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof(pli));
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    ok = ok && (vkCreatePipelineLayout(dev, &pli, 0, &layout) == VK_SUCCESS);

    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs; stages[1].pName = "main";

    VkVertexInputBindingDescription bind;
    memset(&bind, 0, sizeof(bind));
    bind.stride = 24;
    VkVertexInputAttributeDescription attrs[2];
    memset(attrs, 0, sizeof(attrs));
    attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
    attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = 12;
    VkPipelineVertexInputStateCreateInfo vis;
    memset(&vis, 0, sizeof(vis));
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount = 1; vis.pVertexBindingDescriptions = &bind;
    vis.vertexAttributeDescriptionCount = 2; vis.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ias;
    memset(&ias, 0, sizeof(ias));
    ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp; VkRect2D sc;
    memset(&vp, 0, sizeof(vp)); memset(&sc, 0, sizeof(sc));
    vp.width = (float)W; vp.height = (float)H; vp.maxDepth = 1.0f;
    sc.extent.width = W; sc.extent.height = H;
    VkPipelineViewportStateCreateInfo vps;
    memset(&vps, 0, sizeof(vps));
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp;
    vps.scissorCount = 1; vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo ras;
    memset(&ras, 0, sizeof(ras));
    ras.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ras.cullMode = VK_CULL_MODE_BACK_BIT;
    ras.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    ras.lineWidth = 1.0f;

    VkPipelineDepthStencilStateCreateInfo dss;
    memset(&dss, 0, sizeof(dss));
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS;

    VkGraphicsPipelineCreateInfo gpi;
    memset(&gpi, 0, sizeof(gpi));
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vis;
    gpi.pInputAssemblyState = &ias;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &ras;
    gpi.pDepthStencilState = &dss;
    gpi.layout = layout;
    gpi.renderPass = rp;
    VkPipeline pipe = VK_NULL_HANDLE;
    ok = ok && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, 0, &pipe) == VK_SUCCESS);

    /* --- Vertex-Buffer --- */
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(g_verts);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vbuf = VK_NULL_HANDLE;
    ok = ok && (vkCreateBuffer(dev, &bci, 0, &vbuf) == VK_SUCCESS);
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(dev, vbuf, &br);
    mai.allocationSize = br.size;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    ok = ok && (vkAllocateMemory(dev, &mai, 0, &vmem) == VK_SUCCESS) &&
               (vkBindBufferMemory(dev, vbuf, vmem, 0) == VK_SUCCESS);
    void *map = 0;
    ok = ok && (vkMapMemory(dev, vmem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS);
    if (ok) {
        const unsigned char *s = (const unsigned char *)g_verts;
        unsigned char *d = (unsigned char *)map;
        for (unsigned i = 0; i < sizeof(g_verts); i++) { d[i] = s[i]; }
        vkUnmapMemory(dev, vmem);
    }

    /* --- CommandPool/-Buffer + Fence --- */
    VkCommandPoolCreateInfo cpi;
    memset(&cpi, 0, sizeof(cpi));
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    VkCommandPool cpool = VK_NULL_HANDLE;
    ok = ok && (vkCreateCommandPool(dev, &cpi, 0, &cpool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cbi;
    memset(&cbi, 0, sizeof(cbi));
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = cpool;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ok = ok && (vkAllocateCommandBuffers(dev, &cbi, &cmd) == VK_SUCCESS);
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    ok = ok && (vkCreateFence(dev, &fci, 0, &fence) == VK_SUCCESS);
    uwrite("[vkcube] pipeline=");
    uwrite(ok ? "ok" : "FEHLER");
    uwrite(" (renderpass+depth D32, pushconst-mvp, cull-back, .spv-module)\n");
    if (!ok) { sys3(SYS_EXIT, 1, 0, 0); for (;;) { } }

    /* --- Projektions-/View-Matrix (r3d-Helfer, Vulkan-Clip-Konventionen) --- */
    r3d_mat4 proj, view, vpm;
    r3d_mat4_perspective(&proj, 1.0471976f, (float)W / (float)H, 0.1f, 10.0f);
    r3d_mat4_translate(&view, 0.0f, 0.0f, -3.4f);
    r3d_mat4_mul(&vpm, &proj, &view);

    /* Acquire-Semaphore (Review #20: VUID-vkAcquireNextImageKHR-semaphore-01780 verlangt,
     * dass semaphore ODER fence != NULL ist). */
    VkSemaphoreCreateInfo sem_ci;
    memset(&sem_ci, 0, sizeof(sem_ci));
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore acq = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &sem_ci, 0, &acq);

    /* --- Render-Schleife: Acquire -> Record -> Submit -> Wait -> Present --- */
    float ang = 0.0f;
    unsigned long frames = 0;
    int probed = 0;
    unsigned early_hash = 0;             /* Mehrpunkt-Hash eines fruehen Frames (Animations-Nachweis) */
    const gui_t *bb = 0;                 /* fuer die Selbsttest-Probe (Backbuffer lesen) */
    static gui_t probe_gui;
    if (gui_init(&probe_gui) == 0) { bb = &probe_gui; }

    /* Hash ueber ein 5x5-Gitter der zentralen Wuerfelregion -> aendert sich zuverlaessig,
     * sobald der Wuerfel rotiert (robuster als ein einzelner Mittelpixel, der auf derselben
     * Flaeche liegen bleiben kann). */
    #define VKC_HASH(g) vkc_region_hash((g), (int)(W / 2), (int)(H / 2))

    for (;;) {
        uint32_t idx = 0;
        if (vkAcquireNextImageKHR(dev, swap, ~0ull, acq, VK_NULL_HANDLE, &idx) != VK_SUCCESS) { break; }

        r3d_mat4 rx, ry, mm, mvp;
        r3d_mat4_rot_y(&ry, ang);
        r3d_mat4_rot_x(&rx, ang * 0.63f);
        r3d_mat4_mul(&mm, &ry, &rx);
        r3d_mat4_mul(&mvp, &vpm, &mm);
        ang += 0.06f;

        VkCommandBufferBeginInfo cbb;
        memset(&cbb, 0, sizeof(cbb));
        cbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &cbb);
        VkClearValue clears[2];
        memset(clears, 0, sizeof(clears));
        clears[0].color.float32[0] = 0.06f;
        clears[0].color.float32[1] = 0.09f;
        clears[0].color.float32[2] = 0.16f;
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil.depth = 1.0f;
        VkRenderPassBeginInfo rbi;
        memset(&rbi, 0, sizeof(rbi));
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = rp;
        rbi.framebuffer = fb;
        rbi.renderArea.extent.width = W;
        rbi.renderArea.extent.height = H;
        rbi.clearValueCount = 2;
        rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize zoff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zoff);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp.m);
        vkCmdDraw(cmd, 36, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        vkResetFences(dev, 1, &fence);

        VkPresentInfoKHR pi;
        memset(&pi, 0, sizeof(pi));
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap;
        pi.pImageIndices = &idx;
        vkQueuePresentKHR(q, &pi);

        frames++;
        /* Region-Hash eines fruehen Frames merken (fuer den Animationsvergleich in der Probe). */
        if (bb && frames == 3) { early_hash = VKC_HASH(bb); }
        if (!probed && frames == VKCUBE_PROBE_FRAMES) {
            probed = 1;
            /* GESCHAERFTE Probe (Review #29): die Mitte muss ein PLAUSIBLES beleuchtetes Wuerfel-
             * Pixel sein -- NICHT Clear, NICHT schwarz, NICHT das FS-Fail-Pink (0x00FF00FF);
             * eine Ecke muss die Clear-Farbe tragen; und der Mitte-Pixel muss sich seit Frame 3
             * GEAENDERT haben (Rotation -> beweist echte Animation, nicht ein Standbild/Muell). */
            unsigned clear_word = 0xFF0F1729u;     /* (0.06,0.09,0.16, a=1) -> V1.1: mit Alpha */
            unsigned center = bb ? gui_get(bb, (int)(W / 2), (int)(H / 2)) : 0;
            unsigned corner = bb ? gui_get(bb, 4, 4) : 1;
            int ok_mitte = bb && center != clear_word && center != 0x00000000u &&
                           center != 0x00FF00FFu;
            int ok_ecke  = bb && corner == clear_word;
            int ok_anim  = bb && (VKC_HASH(bb) != early_hash);   /* Region-Hash aenderte sich */
            int ok_all   = ok_mitte && ok_ecke && ok_anim;
            uwrite("[vkcube] probe nach ");
            fmt_u(nb, frames); uwrite(nb);
            uwrite(" frames: mitte=");
            uwrite(ok_mitte ? "wuerfel(beleuchtet)" : "FEHLER");
            uwrite(" ecke=");
            uwrite(ok_ecke ? "clearfarbe" : "FEHLER");
            uwrite(" animation=");
            uwrite(ok_anim ? "ja" : "FEHLER");
            /* Marker-Hygiene (Review #31): das Erfolgs-Suffix NUR bei bestandener Probe. */
            if (ok_all) {
                uwrite(" -> vulkan-praesentation ok\n");
                uwrite("[vkcube] laeuft weiter (visueller Modus)\n");
            } else {
                uwrite(" -> PROBE FEHLGESCHLAGEN\n");
            }
        }
        sys1(SYS_SLEEP_MS, 10);
    }
    sys3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
