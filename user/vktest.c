/*
 * user/vktest.c  --  VKTEST.ELF: Selbsttests fuer 3D/Vulkan
 *
 * Deterministische Frame-0-Proben gegen den GUI-Backbuffer (gui_get liest zurueck):
 *   flat     : konstantes Dreieck -> EXAKTE Farbe innen, Hintergrund aussen
 *   gouraud  : RGB-Dreieck, Zentroid faellt EXAKT auf ein Pixelzentrum -> 0x555555
 *   tiefe    : nahes gruenes Dreieck zuerst, fernes rotes danach -> fern wird VERWORFEN
 *              (beweist Depth-TEST und Depth-WRITE zusammen)
 *   cull     : Back-Face (clockwise am Schirm bei front=CCW) wird verworfen; dieselbe
 *              Geometrie mit Front-Winding erscheint (beweist beide Richtungen)
 *   fillrule : Rechteck aus 2 Dreiecken mit GETEILTER Diagonale; ein Zaehl-Fragment-
 *              Shader zaehlt je Pixel -> jedes Rechteck-Pixel GENAU 1x (wasserdicht,
 *              keine Doppel-Fragmente auf der Kante = Top-Left-Regel korrekt)
 *   nearclip : Dreieck mit einer Ecke VOR der Near-Plane (z_clip < 0) -> sichtbarer
 *              Teil gerendert, der geclippte Bereich bleibt Hintergrund
 * Danach: animierte Demo (2 interpenetrierende, beleuchtete Wuerfel, Z-Buffer live).
 *
 * MIT FP kompiliert; laeuft nur in GUI_FP-Builds (-Vk). Braucht USER_CAP_GUI.
 */
#include "abi.h"
#include "ulib.h"
#include "gui.h"
#include "r3d.h"
#include "vk/vk_rtos.h"      /* echte Khronos-Vulkan-API (offizielle Header) */
#include "vk/v3d_qpu.h"      /* V5: V3D-QPU-Encoder-Fundament (Compiler-Ausgabestufe) */
#include "vk/vk_spirv.h"     /* SPIR-V-Interpreter */
#include "vk_shaders.h"      /* generiert von tools/gen_spirv.py (Shader + Referenzwerte) */

void *memset(void *dst, int c, unsigned long n);   /* user/lib/ustring.c */

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

#define BG 0x101828u

static float g_depth[640 * 480];      /* 1,17 MiB .bss (zaehlt nicht gegen das ELF-Dateilimit) */
/* Device-Heap der Vulkan-Implementierung (Plattform-Bootstrap, s. vk_rtos.h).
 * Klein gehalten -- die Offscreen-API-Tests brauchen nur Buffer + 64x64-Images. */
static unsigned char g_vkheap[192 * 1024] __attribute__((aligned(64)));

/* Vertex direkt aus FENSTER-Koordinaten (w=1): Inverse der Viewport-Transformation. */
static r3d_vtx_t sv(float sx, float sy, float z, float r, float g, float b)
{
    r3d_vtx_t v;
    v.pos.x = sx / 320.0f - 1.0f;
    v.pos.y = sy / 240.0f - 1.0f;
    v.pos.z = z;
    v.pos.w = 1.0f;
    v.attr[0] = r; v.attr[1] = g; v.attr[2] = b;
    for (int i = 3; i < R3D_MAX_ATTR; i++) { v.attr[i] = 0.0f; }
    return v;
}

/* Vertex mit explizitem w: Fensterposition (sx,sy) und NDC-Tiefe zndc bleiben w-invariant
 * (Clip-Koordinaten = NDC*w) -- fuer die Probe der perspektivischen Attribut-Korrektur. */
static r3d_vtx_t svw(float sx, float sy, float zndc, float w, float r, float g, float b)
{
    r3d_vtx_t v;
    v.pos.x = (sx / 320.0f - 1.0f) * w;
    v.pos.y = (sy / 240.0f - 1.0f) * w;
    v.pos.z = zndc * w;
    v.pos.w = w;
    v.attr[0] = r; v.attr[1] = g; v.attr[2] = b;
    for (int i = 3; i < R3D_MAX_ATTR; i++) { v.attr[i] = 0.0f; }
    return v;
}

/* Fill-Rule-Zaehler: 64x64-Abdeckungskarte; Basis (x0,y0) kommt via user-Zeiger. */
static unsigned char g_cov[64 * 64];
static unsigned fs_count(void *user, int px, int py, float z, const float *attr)
{
    (void)z; (void)attr;
    const int *base = (const int *)user;
    int cx = px - base[0], cy = py - base[1];
    if (cx >= 0 && cx < 64 && cy >= 0 && cy < 64) { g_cov[cy * 64 + cx]++; }
    return 0xC0C0C0u;
}

/* Abdeckungskarte auswerten: jedes der 64x64 Rechteck-Pixel GENAU 1x. */
static void cov_eval(int *doppel, int *luecken)
{
    *doppel = 0; *luecken = 0;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            unsigned char n = g_cov[y * 64 + x];
            if (n > 1) { (*doppel)++; }
            if (n == 0) { (*luecken)++; }
        }
    }
}

/* ---- Demo-Geometrie: Einheitswuerfel (12 Dreiecke, CCW von aussen gesehen) ---- */
static const float cube_pos[8][3] = {
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
    {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1}
};
/* Je Flaeche: 4 Eckindizes (CCW von aussen) + Normale + Grundfarbe. */
static const int   cube_face[6][4] = {
    {4,5,6,7}, {1,0,3,2}, {5,1,2,6}, {0,4,7,3}, {7,6,2,3}, {0,1,5,4}
};
static const float cube_nrm[6][3] = {
    {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}
};
static const float cube_col[6][3] = {
    {0.85f,0.25f,0.25f}, {0.25f,0.65f,0.85f}, {0.25f,0.80f,0.35f},
    {0.85f,0.75f,0.25f}, {0.75f,0.35f,0.80f}, {0.90f,0.55f,0.25f}
};

static float fmaxf0(float v) { return v > 0.0f ? v : 0.0f; }

/* ================= Vulkan-Kern-Selbsttests (echte Khronos-API) ================= */
static int str_begins(const char *s, const char *pre)
{
    while (*pre) { if (*s++ != *pre++) { return 0; } }
    return 1;
}

static int run_vk_core_tests(void)
{
    char nb[24];
    int all = 1;
    vk_rtos_set_heap(g_vkheap, sizeof(g_vkheap));

    /* --- (1) Instance: Version, Fehlerpfad (unbekannte Extension), Enumerations-Protokoll --- */
    uint32_t ver = 0;
    vkEnumerateInstanceVersion(&ver);
    int ok_ver = (VK_API_VERSION_MAJOR(ver) == 1);

    VkInstance inst = VK_NULL_HANDLE;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    const char *bogus_ext[] = { "VK_KHR_gibt_es_nicht" };
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = bogus_ext;
    int ok_extf = (vkCreateInstance(&ici, 0, &inst) == VK_ERROR_EXTENSION_NOT_PRESENT);

    ici.enabledExtensionCount = 0;
    ici.ppEnabledExtensionNames = 0;
    int ok_inst = (vkCreateInstance(&ici, 0, &inst) == VK_SUCCESS) && (inst != VK_NULL_HANDLE);

    uint32_t ndev = 0;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    int ok_cnt = (vkEnumeratePhysicalDevices(inst, &ndev, 0) == VK_SUCCESS) && (ndev == 1);
    uint32_t zero = 0;
    VkPhysicalDevice dummy[1];
    int ok_inc = (vkEnumeratePhysicalDevices(inst, &zero, dummy) == VK_INCOMPLETE);
    ndev = 1;
    int ok_get = (vkEnumeratePhysicalDevices(inst, &ndev, &pd) == VK_SUCCESS) && pd;
    int t_inst = ok_ver && ok_extf && ok_inst && ok_cnt && ok_inc && ok_get;
    all = all && t_inst;
    uwrite("[vktest] vk: instance=");     uwrite(ok_inst && ok_ver ? "ok" : "FEHLER");
    uwrite(" geraete=1:");                uwrite(ok_cnt && ok_get ? "ok" : "FEHLER");
    uwrite(" protokoll(INCOMPLETE)=");    uwrite(ok_inc ? "ok" : "FEHLER");
    uwrite(" ext-neg=");                  uwrite(ok_extf ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- (2) Eigenschaften: CPU-Geraet, Queue-Familie, Speicher-Heap, Formate --- */
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    int ok_type = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) &&
                  str_begins(props.deviceName, "rpi_rtos");
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, 0);
    VkQueueFamilyProperties qfp;
    uint32_t one = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &one, &qfp);
    int ok_qf = (nqf == 1) && (qfp.queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qfp.queueCount == 1);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    int ok_mem = (mp.memoryHeapCount == 1) && (mp.memoryHeaps[0].size == sizeof(g_vkheap)) &&
                 (mp.memoryTypeCount == 1) &&
                 (mp.memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                 (mp.memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_B8G8R8A8_UNORM, &fp);
    int ok_fmt = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    VkImageFormatProperties ifp;
    int ok_fmtneg = (vkGetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
                        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_SAMPLED_BIT, 0, &ifp) == VK_ERROR_FORMAT_NOT_SUPPORTED);
    /* --- V1.10: erweiterte Format-Features + gehobene Limits (ehrlich zu dem, was die
     * Runtime jetzt kann: Sampling V1.4, Blending V1.1, MRT=8 V1.5, Push=256). --- */
    int ok_fmt2 = ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0) &&
                  ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0);
    VkFormatProperties fvp;
    vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32G32B32_SFLOAT, &fvp);
    int ok_vfmt = ((fvp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0) &&
                  (fvp.optimalTilingFeatures == 0);   /* reines Vertex-Format, kein Bild */
    int ok_limits = (props.limits.maxColorAttachments == 8) &&
                    (props.limits.maxPushConstantsSize == 256);
    int t_props = ok_type && ok_qf && ok_mem && ok_fmt && ok_fmtneg &&
                  ok_fmt2 && ok_vfmt && ok_limits;
    all = all && t_props;
    uwrite("[vktest] vk: eigenschaften=");
    uwrite(t_props ? "ok" : "FEHLER");
    uwrite(" (cpu-geraet, 1 queue-familie graphics, heap=");
    fmt_u(nb, (unsigned long)mp.memoryHeaps[0].size); uwrite(nb);
    uwrite(", format-neg=ok, mrt=8, push=256, vtx-fmt=ok)\n");

    /* --- V3: Core-1.1 "2"-Varianten -- muessen die Basis-Struktur konsistent zur 1.0-Abfrage fuellen,
     * und die PhysicalDevice-"2"-Abfragen sind Instance-Level (vkGetDeviceProcAddr -> NULL). */
    VkPhysicalDeviceProperties2 props2; memset(&props2, 0, sizeof(props2));
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties2(pd, &props2);
    VkPhysicalDeviceFeatures2 feat2; memset(&feat2, 0, sizeof(feat2));
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vkGetPhysicalDeviceFeatures2(pd, &feat2);
    VkPhysicalDeviceMemoryProperties2 mem2; memset(&mem2, 0, sizeof(mem2));
    mem2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    vkGetPhysicalDeviceMemoryProperties2(pd, &mem2);
    uint32_t nqf2 = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &nqf2, 0);
    VkQueueFamilyProperties2 qfp2; memset(&qfp2, 0, sizeof(qfp2));
    qfp2.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    uint32_t one2 = 1;
    vkGetPhysicalDeviceQueueFamilyProperties2(pd, &one2, &qfp2);
    int ok_pa2 = (vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2") != 0) &&
                 (vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2") == 0);
    int t_khr2 = (props2.properties.limits.maxColorAttachments == props.limits.maxColorAttachments) &&
                 (props2.properties.limits.maxPushConstantsSize == 256) &&
                 (props2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) &&
                 (feat2.features.robustBufferAccess == VK_TRUE) &&
                 (mem2.memoryProperties.memoryHeaps[0].size == sizeof(g_vkheap)) &&
                 (nqf2 == 1) && (qfp2.queueFamilyProperties.queueCount == 1) &&
                 ((qfp2.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) && ok_pa2;
    all = all && t_khr2;
    uwrite("[vktest] vk V3: core-1.1 '2'-varianten=");
    uwrite(t_khr2 ? "ok" : "FEHLER");
    uwrite(" (Properties2/Features2/MemoryProperties2/QueueFamilyProperties2 konsistent + ProcAddr-Level)\n");

    /* --- V3: Core-1.3 objektlose Mem-Requirements -- muessen mit dem objektbasierten Pfad
     * uebereinstimmen (alle Funktionen ignorieren den Device-Param -> VK_NULL_HANDLE genuegt). */
    VkBufferCreateInfo bmci; memset(&bmci, 0, sizeof(bmci));
    bmci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bmci.size = 1000; bmci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer tmpbuf = VK_NULL_HANDLE;
    int ok_mr = (vkCreateBuffer(VK_NULL_HANDLE, &bmci, 0, &tmpbuf) == VK_SUCCESS);
    VkBufferMemoryRequirementsInfo2 bri; memset(&bri, 0, sizeof(bri));
    bri.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2; bri.buffer = tmpbuf;
    VkMemoryRequirements2 mr_obj; memset(&mr_obj, 0, sizeof(mr_obj));
    mr_obj.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetBufferMemoryRequirements2(VK_NULL_HANDLE, &bri, &mr_obj);
    VkDeviceBufferMemoryRequirements dbr; memset(&dbr, 0, sizeof(dbr));
    dbr.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS; dbr.pCreateInfo = &bmci;
    VkMemoryRequirements2 mr_less; memset(&mr_less, 0, sizeof(mr_less));
    mr_less.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetDeviceBufferMemoryRequirements(VK_NULL_HANDLE, &dbr, &mr_less);
    ok_mr = ok_mr && (mr_obj.memoryRequirements.size == mr_less.memoryRequirements.size) &&
            (mr_less.memoryRequirements.size >= 1000) && (mr_less.memoryRequirements.memoryTypeBits == 1u);
    vkDestroyBuffer(VK_NULL_HANDLE, tmpbuf, 0);
    VkImageCreateInfo imci; memset(&imci, 0, sizeof(imci));
    imci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; imci.imageType = VK_IMAGE_TYPE_2D;
    imci.format = VK_FORMAT_B8G8R8A8_UNORM;
    imci.extent.width = 16; imci.extent.height = 8; imci.extent.depth = 1;
    imci.mipLevels = 1; imci.arrayLayers = 1; imci.samples = VK_SAMPLE_COUNT_1_BIT;
    imci.tiling = VK_IMAGE_TILING_LINEAR; imci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage tmpimg = VK_NULL_HANDLE;
    int ok_mri = (vkCreateImage(VK_NULL_HANDLE, &imci, 0, &tmpimg) == VK_SUCCESS);
    VkImageMemoryRequirementsInfo2 iri; memset(&iri, 0, sizeof(iri));
    iri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2; iri.image = tmpimg;
    VkMemoryRequirements2 imr_obj; memset(&imr_obj, 0, sizeof(imr_obj));
    imr_obj.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetImageMemoryRequirements2(VK_NULL_HANDLE, &iri, &imr_obj);
    VkDeviceImageMemoryRequirements dir; memset(&dir, 0, sizeof(dir));
    dir.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS; dir.pCreateInfo = &imci;
    VkMemoryRequirements2 imr_less; memset(&imr_less, 0, sizeof(imr_less));
    imr_less.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetDeviceImageMemoryRequirements(VK_NULL_HANDLE, &dir, &imr_less);
    ok_mri = ok_mri && (imr_obj.memoryRequirements.size == imr_less.memoryRequirements.size) &&
             (imr_less.memoryRequirements.size >= 16u * 8u * 4u);
    vkDestroyImage(VK_NULL_HANDLE, tmpimg, 0);
    int t_memreq = ok_mr && ok_mri;
    all = all && t_memreq;
    uwrite("[vktest] vk V3: core-1.3 objektlose mem-req=");
    uwrite(t_memreq ? "ok" : "FEHLER");
    uwrite(" (buffer/image DeviceMemoryRequirements == objektbasiert)\n");

    /* --- V3: Core-1.1 Device-Groups + External-Properties + Trim. */
    uint32_t ngrp = 0;
    int ok_grpcnt = (vkEnumeratePhysicalDeviceGroups(inst, &ngrp, 0) == VK_SUCCESS) && (ngrp == 1);
    VkPhysicalDeviceGroupProperties grp; memset(&grp, 0, sizeof(grp));
    grp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
    uint32_t g1 = 1;
    vkEnumeratePhysicalDeviceGroups(inst, &g1, &grp);
    int ok_grp = ok_grpcnt && (g1 == 1) && (grp.physicalDeviceCount == 1) && (grp.physicalDevices[0] == pd);
    VkPeerMemoryFeatureFlags peer = 0;
    vkGetDeviceGroupPeerMemoryFeatures(VK_NULL_HANDLE, 0, 0, 0, &peer);
    int ok_peer = ((peer & VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT) != 0) &&
                  ((peer & VK_PEER_MEMORY_FEATURE_COPY_DST_BIT) != 0);
    VkPhysicalDeviceExternalBufferInfo ebi; memset(&ebi, 0, sizeof(ebi));
    ebi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    ebi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties ebp; memset(&ebp, 0, sizeof(ebp));
    ebp.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(pd, &ebi, &ebp);
    int ok_ext = (ebp.externalMemoryProperties.externalMemoryFeatures == 0);   /* ehrlich: kein External */
    int t_grp = ok_grp && ok_peer && ok_ext;
    all = all && t_grp;
    uwrite("[vktest] vk V3: core-1.1 device-group/external=");
    uwrite(t_grp ? "ok" : "FEHLER");
    uwrite(" (1 gruppe/1 device, peer-mem-features, kein external-handle)\n");

    /* --- V3: Core-1.1 vkGetDescriptorSetLayoutSupport -- gueltiges Layout -> supported, Over-Limit -> nicht. */
    VkDescriptorSetLayoutBinding lsb; memset(&lsb, 0, sizeof(lsb));
    lsb.binding = 0; lsb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; lsb.descriptorCount = 1;
    lsb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lsci; memset(&lsci, 0, sizeof(lsci));
    lsci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lsci.bindingCount = 1; lsci.pBindings = &lsb;
    VkDescriptorSetLayoutSupport sup; memset(&sup, 0, sizeof(sup));
    sup.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
    vkGetDescriptorSetLayoutSupport(VK_NULL_HANDLE, &lsci, &sup);
    int ok_dsls = (sup.supported == VK_TRUE);
    lsci.bindingCount = 99;                             /* Over-Limit -> nicht unterstuetzt (pBindings ungelesen) */
    VkDescriptorSetLayoutSupport sup2; memset(&sup2, 0, sizeof(sup2));
    sup2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
    vkGetDescriptorSetLayoutSupport(VK_NULL_HANDLE, &lsci, &sup2);
    ok_dsls = ok_dsls && (sup2.supported == VK_FALSE);
    all = all && ok_dsls;
    uwrite("[vktest] vk V3: core-1.1 dsl-support (1 binding -> supported, 99 -> nicht)=");
    uwrite(ok_dsls ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 vkGetPhysicalDeviceToolProperties -- keine aktiven Tools -> count 0. */
    uint32_t ntool = 123;
    int ok_tool = (vkGetPhysicalDeviceToolProperties(pd, &ntool, 0) == VK_SUCCESS) && (ntool == 0);
    all = all && ok_tool;
    uwrite("[vktest] vk V3: core-1.3 tool-properties (keine aktiven Tools -> 0)=");
    uwrite(ok_tool ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 Private-Data-Slots -- Set/Get pro Objekt-Handle, Ueberschreiben, ungesetzt -> 0. */
    int ok_pd = 1;
    {
        VkPrivateDataSlotCreateInfo pdci; memset(&pdci, 0, sizeof(pdci));
        pdci.sType = VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO;
        VkPrivateDataSlot slot = VK_NULL_HANDLE;
        ok_pd = (vkCreatePrivateDataSlot(VK_NULL_HANDLE, &pdci, 0, &slot) == VK_SUCCESS);
        ok_pd = ok_pd && (vkSetPrivateData(VK_NULL_HANDLE, VK_OBJECT_TYPE_BUFFER, 0x1111u, slot, 0xDEADBEEFull) == VK_SUCCESS);
        ok_pd = ok_pd && (vkSetPrivateData(VK_NULL_HANDLE, VK_OBJECT_TYPE_BUFFER, 0x2222u, slot, 0xCAFEull) == VK_SUCCESS);
        uint64_t d1 = 0, d2 = 0, d3 = 7;
        vkGetPrivateData(VK_NULL_HANDLE, VK_OBJECT_TYPE_BUFFER, 0x1111u, slot, &d1);
        vkGetPrivateData(VK_NULL_HANDLE, VK_OBJECT_TYPE_BUFFER, 0x2222u, slot, &d2);
        vkGetPrivateData(VK_NULL_HANDLE, VK_OBJECT_TYPE_BUFFER, 0x9999u, slot, &d3);   /* ungesetzt -> 0 */
        ok_pd = ok_pd && (d1 == 0xDEADBEEFull) && (d2 == 0xCAFEull) && (d3 == 0);
        vkSetPrivateData(VK_NULL_HANDLE, VK_OBJECT_TYPE_BUFFER, 0x1111u, slot, 0x99ull);   /* ueberschreiben */
        uint64_t d4 = 0; vkGetPrivateData(VK_NULL_HANDLE, VK_OBJECT_TYPE_BUFFER, 0x1111u, slot, &d4);
        ok_pd = ok_pd && (d4 == 0x99ull);
        vkDestroyPrivateDataSlot(VK_NULL_HANDLE, slot, 0);
    }
    all = all && ok_pd;
    uwrite("[vktest] vk V3: private-data (set/get je handle, ueberschreiben, ungesetzt->0)=");
    uwrite(ok_pd ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V4: ICD-Interface -- Negotiate klemmt auf 5, GetInstanceProcAddr konsistent, PhysDevProcAddr filtert. */
    uint32_t icdVer = 7;
    int ok_icd = (vk_icdNegotiateLoaderICDInterfaceVersion(&icdVer) == VK_SUCCESS) && (icdVer == 5);
    uint32_t icdVer2 = 3;
    ok_icd = ok_icd && (vk_icdNegotiateLoaderICDInterfaceVersion(&icdVer2) == VK_SUCCESS) && (icdVer2 == 3);
    ok_icd = ok_icd &&
        (vk_icdGetInstanceProcAddr(inst, "vkCreateDevice") == vkGetInstanceProcAddr(inst, "vkCreateDevice")) &&
        (vk_icdGetInstanceProcAddr(inst, "vkCreateDevice") != 0) &&
        (vk_icdGetPhysicalDeviceProcAddr(inst, "vkGetPhysicalDeviceProperties") != 0) &&
        (vk_icdGetPhysicalDeviceProcAddr(inst, "vkCreateDevice") == 0);   /* keine PhysDev-Funktion */
    all = all && ok_icd;
    uwrite("[vktest] vk V4: icd-interface (Negotiate->5, GetInstanceProcAddr konsistent, PhysDevProcAddr gefiltert)=");
    uwrite(ok_icd ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: promovierte Versions-Features EHRLICH gemeldet (Features2 pNext: aggregierte Vulkan1x-Structs). */
    VkPhysicalDeviceVulkan13Features v13; memset(&v13, 0, sizeof(v13));
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features v12; memset(&v12, 0, sizeof(v12));
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; v12.pNext = &v13;
    VkPhysicalDeviceFeatures2 vf2; memset(&vf2, 0, sizeof(vf2));
    vf2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; vf2.pNext = &v12;
    vkGetPhysicalDeviceFeatures2(pd, &vf2);
    int ok_vf = (v12.timelineSemaphore == VK_TRUE) && (v12.drawIndirectCount == VK_TRUE) &&
                (v13.synchronization2 == VK_TRUE) && (v12.bufferDeviceAddress == VK_FALSE);
    all = all && ok_vf;
    uwrite("[vktest] vk V3: versions-features (Timeline+Sync2+DrawIndirectCount gemeldet, BDA ehrlich FALSE)=");
    uwrite(ok_vf ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V6: Treiber-/Konformitaets-Properties -- driverName identifiziert uns; conformanceVersion
     * EHRLICH {0,0,0,0} (Khronos-VK-GL-CTS nicht durchlaufen -> keine offizielle Konformitaet behauptet). */
    VkPhysicalDeviceDriverProperties dprops; memset(&dprops, 0, sizeof(dprops));
    dprops.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 pp2; memset(&pp2, 0, sizeof(pp2));
    pp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; pp2.pNext = &dprops;
    vkGetPhysicalDeviceProperties2(pd, &pp2);
    int ok_drv = str_begins(dprops.driverName, "rpi_rtos") && (dprops.conformanceVersion.major == 0) &&
                 (dprops.conformanceVersion.minor == 0);
    all = all && ok_drv;
    uwrite("[vktest] vk V6: driver-properties (driverName=rpi_rtos, conformanceVersion ehrlich 0.0.0.0)=");
    uwrite(ok_drv ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V5: V3D-Backend-Fundament (Compiler-Ausgabestufe + Control-Lists) -- QPU-Instruktions-
     * Encoder (Round-Trip + Feld-Isolation + Maskierung) UND Control-List-Builder (byte-exakte
     * Record-Emission + Overflow-Schutz). Die exakten Feld-/Opcode-Layouts gegen die reale
     * V3D-4.2-ISA sind per HW-RE zu validieren (QEMU emuliert die V3D nicht). */
    int ok_qpu = (v3d_qpu_selftest() == 0);
    all = all && ok_qpu;
    uwrite("[vktest] vk V5: v3d-backend-fundament (QPU-encoder round-trip + control-list byte-exakt, HW-RE-zu-validieren)=");
    uwrite(ok_qpu ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- (3) Device: Feature-Fehlerpfad, dann echtes Device + Queue --- */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures want;
    memset(&want, 0, sizeof(want));
    want.geometryShader = VK_TRUE;                 /* nicht vorhanden -> Spez-Fehlercode */
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &want;
    VkDevice dev = VK_NULL_HANDLE;
    int ok_featf = (vkCreateDevice(pd, &dci, 0, &dev) == VK_ERROR_FEATURE_NOT_PRESENT);

    memset(&want, 0, sizeof(want));
    want.robustBufferAccess = VK_TRUE;             /* vorhanden -> muss klappen */
    int ok_dev = (vkCreateDevice(pd, &dci, 0, &dev) == VK_SUCCESS) && dev;
    VkQueue q = VK_NULL_HANDLE;
    if (ok_dev) { vkGetDeviceQueue(dev, 0, 0, &q); }
    int ok_q = (q != VK_NULL_HANDLE) && (vkQueueWaitIdle(q) == VK_SUCCESS);
    int t_dev = ok_featf && ok_dev && ok_q;
    all = all && t_dev;
    uwrite("[vktest] vk: device=");     uwrite(ok_dev ? "ok" : "FEHLER");
    uwrite(" queue=");                  uwrite(ok_q ? "ok" : "FEHLER");
    uwrite(" feature-neg=");            uwrite(ok_featf ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- (4) Speicher: Buffer/Image anlegen, Requirements, Alloc/Map/Write/Read, OOM --- */
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 4096;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    int ok_buf = (vkCreateBuffer(dev, &bci, 0, &buf) == VK_SUCCESS);
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(dev, buf, &br);
    ok_buf = ok_buf && (br.size >= 4096) && (br.memoryTypeBits == 1u);

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = br.size;
    mai.memoryTypeIndex = 0;
    VkDeviceMemory dm = VK_NULL_HANDLE;
    ok_buf = ok_buf && (vkAllocateMemory(dev, &mai, 0, &dm) == VK_SUCCESS);
    void *map = 0;
    ok_buf = ok_buf && (vkMapMemory(dev, dm, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) && map;
    int ok_rw = 0;
    if (ok_buf) {
        unsigned char *p = (unsigned char *)map;
        for (int i = 0; i < 4096; i++) { p[i] = (unsigned char)(i * 13 + 7); }
        ok_rw = 1;
        for (int i = 0; i < 4096; i++) { if (p[i] != (unsigned char)(i * 13 + 7)) { ok_rw = 0; break; } }
        vkUnmapMemory(dev, dm);
    }
    ok_buf = ok_buf && ok_rw && (vkBindBufferMemory(dev, buf, dm, 0) == VK_SUCCESS);

    VkImageCreateInfo ic;
    memset(&ic, 0, sizeof(ic));
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = VK_FORMAT_B8G8R8A8_UNORM;
    ic.extent.width = 64; ic.extent.height = 64; ic.extent.depth = 1;
    ic.mipLevels = 1; ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    /* LINEAR-Tiling: vkGetImageSubresourceLayout ist laut Spez (VUID-...-image-00996) NUR fuer
     * LINEAR-getilte Images definiert -- der rowPitch-Assert unten braucht daher LINEAR. */
    ic.tiling = VK_IMAGE_TILING_LINEAR;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    int ok_img = (vkCreateImage(dev, &ic, 0, &img) == VK_SUCCESS);
    VkMemoryRequirements ir;
    vkGetImageMemoryRequirements(dev, img, &ir);
    ok_img = ok_img && (ir.size >= 64 * 64 * 4);
    mai.allocationSize = ir.size;
    VkDeviceMemory dmi = VK_NULL_HANDLE;
    ok_img = ok_img && (vkAllocateMemory(dev, &mai, 0, &dmi) == VK_SUCCESS) &&
             (vkBindImageMemory(dev, img, dmi, 0) == VK_SUCCESS);
    VkImageSubresource sub; memset(&sub, 0, sizeof(sub));
    sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout lay;
    vkGetImageSubresourceLayout(dev, img, &sub, &lay);
    ok_img = ok_img && (lay.rowPitch == 256);
    /* V3.4: Core-1.4 vkGetImageSubresourceLayout2 -- muss dasselbe rowPitch liefern wie die 1.0-Variante. */
    VkImageSubresource2 sub2; memset(&sub2, 0, sizeof(sub2));
    sub2.sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2; sub2.imageSubresource = sub;
    VkSubresourceLayout2 lay2; memset(&lay2, 0, sizeof(lay2));
    lay2.sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2;
    vkGetImageSubresourceLayout2(dev, img, &sub2, &lay2);
    ok_img = ok_img && (lay2.subresourceLayout.rowPitch == lay.rowPitch) && (lay2.subresourceLayout.rowPitch == 256);
    /* V3.4: Core-1.4 vkGetDeviceImageSubresourceLayout (objektlos, aus dem Create-Info) + Granularitaet. */
    VkDeviceImageSubresourceInfo disi; memset(&disi, 0, sizeof(disi));
    disi.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO;
    disi.pCreateInfo = &ic; disi.pSubresource = &sub2;
    VkSubresourceLayout2 dlay2; memset(&dlay2, 0, sizeof(dlay2));
    dlay2.sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2;
    vkGetDeviceImageSubresourceLayout(dev, &disi, &dlay2);
    ok_img = ok_img && (dlay2.subresourceLayout.rowPitch == 256);
    VkRenderingAreaInfo rai; memset(&rai, 0, sizeof(rai));
    rai.sType = VK_STRUCTURE_TYPE_RENDERING_AREA_INFO;
    VkExtent2D gran; memset(&gran, 0, sizeof(gran));
    vkGetRenderingAreaGranularity(dev, &rai, &gran);
    ok_img = ok_img && (gran.width == 1) && (gran.height == 1);

    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    ok_img = ok_img && (vkCreateImageView(dev, &vci, 0, &view) == VK_SUCCESS);

    mai.allocationSize = sizeof(g_vkheap);         /* mehr als frei -> ehrlicher OOM */
    VkDeviceMemory dmo = VK_NULL_HANDLE;
    int ok_oom = (vkAllocateMemory(dev, &mai, 0, &dmo) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
    int t_mem = ok_buf && ok_img && ok_oom;
    all = all && t_mem;
    uwrite("[vktest] vk: speicher=");   uwrite(ok_buf ? "ok" : "FEHLER");
    uwrite(" (map/write/read+bind) image=");  uwrite(ok_img ? "ok" : "FEHLER");
    uwrite(" (rowpitch=256+view) oom=");
    uwrite(ok_oom ? "VK_ERROR_OUT_OF_DEVICE_MEMORY" : "FEHLER");
    uwrite("\n");

    /* --- (5) ProcAddr: implementierte Entries identisch, unbekannte NULL --- */
    PFN_vkVoidFunction f1 = vkGetInstanceProcAddr(inst, "vkCreateDevice");
    PFN_vkVoidFunction f2 = vkGetInstanceProcAddr(inst, "vkDingsBums");
    int t_pa = (f1 == (PFN_vkVoidFunction)vkCreateDevice) && (f2 == 0);
    all = all && t_pa;
    uwrite("[vktest] vk: procaddr=");   uwrite(t_pa ? "ok" : "FEHLER");
    uwrite("\n");

    /* Aufraeumen (exerziert die Destroy-Pfade). */
    vkDestroyImageView(dev, view, 0);
    vkDestroyImage(dev, img, 0);
    vkFreeMemory(dev, dmi, 0);
    vkDestroyBuffer(dev, buf, 0);
    vkFreeMemory(dev, dm, 0);
    vkDestroyDevice(dev, 0);
    vkDestroyInstance(inst, 0);
    return all;
}

/* Einen Wuerfel (Modellmatrix 'mm', Skalierung s) beleuchtet in den Clip-Space bringen
 * und zeichnen. Flat-Licht je Flaeche: I = 0.3 + 0.7*max(0, n.l). */
static void draw_cube(const r3d_target_t *t, const r3d_mat4 *vp, const r3d_mat4 *mm, float s)
{
    /* Lichtrichtung (Weltraum), normalisiert. */
    float lx = 0.37f, ly = 0.66f, lz = 0.65f;
    for (int f = 0; f < 6; f++) {
        /* Normale mit der Modellmatrix rotieren (mm ist Rotation+Translation -> obere
         * 3x3 ist orthonormal, w=0 laesst die Translation aussen vor). */
        r3d_vec4 n = { cube_nrm[f][0], cube_nrm[f][1], cube_nrm[f][2], 0.0f };
        r3d_mat4_mul_vec4(&n, mm, &n);
        float it = 0.3f + 0.7f * fmaxf0(n.x * lx + n.y * ly + n.z * lz);
        float r = cube_col[f][0] * it, g = cube_col[f][1] * it, b = cube_col[f][2] * it;

        r3d_vtx_t q[4];
        for (int k = 0; k < 4; k++) {
            const float *p = cube_pos[cube_face[f][k]];
            r3d_vec4 wpos = { p[0] * s, p[1] * s, p[2] * s, 1.0f };
            r3d_mat4_mul_vec4(&wpos, mm, &wpos);
            r3d_mat4_mul_vec4(&wpos, vp, &wpos);
            q[k].pos = wpos;
            q[k].attr[0] = r; q[k].attr[1] = g; q[k].attr[2] = b;
            for (int i = 3; i < R3D_MAX_ATTR; i++) { q[k].attr[i] = 0.0f; }
        }
        r3d_draw_tri(t, &q[0], &q[1], &q[2], 3, 0, 0);
        r3d_draw_tri(t, &q[0], &q[2], &q[3], 3, 0, 0);
    }
}

/* ================= SPIR-V-Interpreter-Selbsttests ================= */
static float fabsf_(float v) { return v < 0.0f ? -v : v; }
static int close4(const float *a, const float *b)
{
    for (int i = 0; i < 4; i++) { if (fabsf_(a[i] - b[i]) > 1e-4f) { return 0; } }
    return 1;
}

static spv_mod_t g_spv_vert, g_spv_frag, g_spv_test;

static int run_spirv_tests(void)
{
    int all = 1;

    /* (1) Parsen + Entry-Points aller drei Module (vert/frag/test). */
    int p1 = spv_parse(&g_spv_vert, spv_vert_words, sizeof(spv_vert_words) / 4);
    int p2 = spv_parse(&g_spv_frag, spv_frag_words, sizeof(spv_frag_words) / 4);
    int p3 = spv_parse(&g_spv_test, spv_test_words, sizeof(spv_test_words) / 4);
    unsigned fv = 0, ff = 0, ft = 0;
    int e1 = spv_find_entry(&g_spv_vert, SPV_MODEL_VERTEX, &fv);
    int e2 = spv_find_entry(&g_spv_frag, SPV_MODEL_FRAGMENT, &ff);
    int e3 = spv_find_entry(&g_spv_test, SPV_MODEL_VERTEX, &ft);
    int t_parse = (p1 == 0 && p2 == 0 && p3 == 0 && e1 == 0 && e2 == 0 && e3 == 0);
    all = all && t_parse;
    uwrite("[vktest] spirv: parse+entry (vert/frag/test)=");
    uwrite(t_parse ? "ok" : "FEHLER");
    uwrite("\n");

    /* (2) Vertex-Shader: gl_Position = pushconst.mvp * vec4(pos,1); vColor = inColor.
     * Referenzwerte von gen_spirv.py (Python) mit Toleranz 1e-4. */
    static spv_io_t io;
    memset(&io, 0, sizeof(io));
    for (int i = 0; i < 3; i++) { io.in_loc[0][i] = spv_tv_pos[i]; io.in_loc[1][i] = spv_tv_col[i]; }
    io.push = spv_tv_mvp_cols;
    io.push_bytes = sizeof(spv_tv_mvp_cols);
    int r = spv_exec(&g_spv_vert, fv, &io);
    int t_vert = (r == 0) && close4(io.builtin_pos, spv_tv_exp_pos) &&
                 fabsf_(io.out_loc[0][0] - spv_tv_col[0]) < 1e-6f &&
                 fabsf_(io.out_loc[0][1] - spv_tv_col[1]) < 1e-6f &&
                 fabsf_(io.out_loc[0][2] - spv_tv_col[2]) < 1e-6f;
    all = all && t_vert;
    uwrite("[vktest] spirv: vertex mvp*pos+color (pushconst, referenz +-1e-4)=");
    uwrite(t_vert ? "ok" : "FEHLER");
    uwrite("\n");

    /* (3) Fragment-Shader: outColor = vec4(vColor, 1). */
    memset(&io, 0, sizeof(io));
    io.in_loc[0][0] = 0.25f; io.in_loc[0][1] = 0.5f; io.in_loc[0][2] = 0.75f;
    r = spv_exec(&g_spv_frag, ff, &io);
    int t_frag = (r == 0) &&
                 fabsf_(io.out_loc[0][0] - 0.25f) < 1e-6f &&
                 fabsf_(io.out_loc[0][1] - 0.5f)  < 1e-6f &&
                 fabsf_(io.out_loc[0][2] - 0.75f) < 1e-6f &&
                 fabsf_(io.out_loc[0][3] - 1.0f)  < 1e-6f;
    all = all && t_frag;
    uwrite("[vktest] spirv: fragment vec4(vColor,1)=");
    uwrite(t_frag ? "ok" : "FEHLER");
    uwrite("\n");

    /* (4) Kontrollfluss-Shader: BEIDE Branch-Pfade (Phi!) + Shuffle + Normalize/Dot + Select. */
    memset(&io, 0, sizeof(io));
    for (int i = 0; i < 4; i++) { io.in_loc[0][i] = spv_tv_t1[i]; }
    r = spv_exec(&g_spv_test, ft, &io);
    int t_cf = (r == 0) && close4(io.out_loc[0], spv_tv_e1);
    memset(&io, 0, sizeof(io));
    for (int i = 0; i < 4; i++) { io.in_loc[0][i] = spv_tv_t2[i]; }
    r = spv_exec(&g_spv_test, ft, &io);
    t_cf = t_cf && (r == 0) && close4(io.out_loc[0], spv_tv_e2);
    all = all && t_cf;
    uwrite("[vktest] spirv: branch/phi/shuffle/normalize/dot/select (beide pfade)=");
    uwrite(t_cf ? "ok" : "FEHLER");
    uwrite("\n");

    /* (5) Fail-loud: verfaelschtes Magic + unbekannte Op muessen ABGELEHNT werden. */
    static unsigned bad[600];
    unsigned fw = sizeof(spv_frag_words) / 4;
    for (unsigned i = 0; i < fw && i < 600; i++) { bad[i] = spv_frag_words[i]; }
    bad[0] = 0x12345678u;
    spv_mod_t bm;
    int t_neg = (spv_parse(&bm, bad, fw) < 0);
    all = all && t_neg;
    uwrite("[vktest] spirv: magic-neg=");
    uwrite(t_neg ? "ok" : "FEHLER");
    uwrite("\n");

   
    static unsigned atk[600];
    unsigned vw = sizeof(spv_vert_words) / 4;
    int t_hard = 1;
    if (vw <= 600) {
        /* (a) Ergebnis-<id> ausserhalb bound: erstes OpLoad (Opcode 61) finden, seine
         * Result-<id> (Wort +2) auf 0x40000000 setzen -> muss abgelehnt werden statt val[]-OOB. */
        for (unsigned i = 0; i < vw; i++) { atk[i] = spv_vert_words[i]; }
        unsigned pc = 5, patched = 0;
        while (pc < vw) {
            unsigned wc = atk[pc] >> 16, opc = atk[pc] & 0xFFFFu;
            if (wc == 0) { break; }
            if (opc == 61 /*OpLoad*/ && wc >= 4) { atk[pc + 2] = 0x40000000u; patched = 1; break; }
            pc += wc;
        }
        spv_mod_t am;
        unsigned afn = 0;
        int parsed = (spv_parse(&am, atk, vw) == 0) && (spv_find_entry(&am, SPV_MODEL_VERTEX, &afn) == 0);
        static spv_io_t aio;
        memset(&aio, 0, sizeof(aio));
        aio.push = spv_tv_mvp_cols; aio.push_bytes = sizeof(spv_tv_mvp_cols);
        int rc = parsed ? spv_exec(&am, afn, &aio) : 0;
        /* patched: spv_exec MUSS < 0 liefern (Abbruch), NICHT 0. Ohne Patch waere es 0. */
        t_hard = patched && parsed && (rc < 0);
    }
    all = all && t_hard;
    uwrite("[vktest] spirv: haertung (untrusted <id> >= bound -> fail-loud statt OOB)=");
    uwrite(t_hard ? "ok" : "FEHLER");
    uwrite("\n");

    return all;
}

/* ================= Vulkan-Draw-Pfad (Offscreen, komplette API-Kette) ================= */
static int run_vkdraw_tests(void)
{
    int all = 1;
    vk_rtos_set_heap(g_vkheap, sizeof(g_vkheap));

    VkInstance inst = VK_NULL_HANDLE;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if (vkCreateInstance(&ici, 0, &inst) != VK_SUCCESS) { return 0; }
    uint32_t nd = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &nd, &pd);
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
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(pd, &dci, 0, &dev) != VK_SUCCESS) { vkDestroyInstance(inst, 0); return 0; }
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    /* --- Attachments: 64x64 Farbe (B8G8R8A8) + 64x64 Tiefe (D32) --- */
    VkImageCreateInfo ic;
    memset(&ic, 0, sizeof(ic));
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.extent.width = 64; ic.extent.height = 64; ic.extent.depth = 1;
    ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ic.format = VK_FORMAT_B8G8R8A8_UNORM;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage cimg = VK_NULL_HANDLE;
    int ok = (vkCreateImage(dev, &ic, 0, &cimg) == VK_SUCCESS);
    ic.format = VK_FORMAT_D32_SFLOAT;
    ic.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage dimg = VK_NULL_HANDLE;
    ok = ok && (vkCreateImage(dev, &ic, 0, &dimg) == VK_SUCCESS);

    VkMemoryRequirements cr, dr;
    vkGetImageMemoryRequirements(dev, cimg, &cr);
    vkGetImageMemoryRequirements(dev, dimg, &dr);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    VkDeviceMemory cmem = VK_NULL_HANDLE, dmem = VK_NULL_HANDLE;
    mai.allocationSize = cr.size;
    ok = ok && (vkAllocateMemory(dev, &mai, 0, &cmem) == VK_SUCCESS) &&
               (vkBindImageMemory(dev, cimg, cmem, 0) == VK_SUCCESS);
    mai.allocationSize = dr.size;
    ok = ok && (vkAllocateMemory(dev, &mai, 0, &dmem) == VK_SUCCESS) &&
               (vkBindImageMemory(dev, dimg, dmem, 0) == VK_SUCCESS);

    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VkImageView cview = VK_NULL_HANDLE, dview = VK_NULL_HANDLE;
    vci.image = cimg; vci.format = VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ok = ok && (vkCreateImageView(dev, &vci, 0, &cview) == VK_SUCCESS);
    vci.image = dimg; vci.format = VK_FORMAT_D32_SFLOAT;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ok = ok && (vkCreateImageView(dev, &vci, 0, &dview) == VK_SUCCESS);

    /* --- RenderPass (Farbe CLEAR + Tiefe CLEAR) + Framebuffer --- */
    VkAttachmentDescription att[2];
    memset(att, 0, sizeof(att));
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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

    VkImageView fbat[2] = { cview, dview };
    VkFramebufferCreateInfo fbi;
    memset(&fbi, 0, sizeof(fbi));
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = rp;
    fbi.attachmentCount = 2; fbi.pAttachments = fbat;
    fbi.width = 64; fbi.height = 64; fbi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    ok = ok && (vkCreateFramebuffer(dev, &fbi, 0, &fb) == VK_SUCCESS);

    /* --- Shader-Module (ECHTES vkCreateShaderModule mit den .spv-Woertern) --- */
    VkShaderModuleCreateInfo smi;
    memset(&smi, 0, sizeof(smi));
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(spv_vert_words); smi.pCode = spv_vert_words;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    ok = ok && (vkCreateShaderModule(dev, &smi, 0, &vs) == VK_SUCCESS);
    smi.codeSize = sizeof(spv_frag_words); smi.pCode = spv_frag_words;
    ok = ok && (vkCreateShaderModule(dev, &smi, 0, &fs) == VK_SUCCESS);

    /* --- Layout (64B Push-Constants: mat4) + Pipeline --- */
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
    bind.stride = 24;                                  /* vec3 pos + vec3 farbe */
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
    vp.width = 64.0f; vp.height = 64.0f; vp.maxDepth = 1.0f;
    sc.extent.width = 64; sc.extent.height = 64;
    VkPipelineViewportStateCreateInfo vps;
    memset(&vps, 0, sizeof(vps));
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp;
    vps.scissorCount = 1; vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo ras;
    memset(&ras, 0, sizeof(ras));
    ras.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ras.cullMode = VK_CULL_MODE_NONE;
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

    /* --- Vertex-Buffer: 2 Dreiecke, gleicher Footprint -- gruen NAH (z=0.25) zuerst,
     * rot FERN (z=0.75) danach -> Tiefentest muss rot VERWERFEN. --- */
    static const float verts[6][6] = {
        { -0.9f, -0.9f, 0.25f,  0, 1, 0 },
        {  0.9f, -0.9f, 0.25f,  0, 1, 0 },
        {  0.0f,  0.9f, 0.25f,  0, 1, 0 },
        { -0.9f, -0.9f, 0.75f,  1, 0, 0 },
        {  0.9f, -0.9f, 0.75f,  1, 0, 0 },
        {  0.0f,  0.9f, 0.75f,  1, 0, 0 },
    };
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(verts);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vbuf = VK_NULL_HANDLE;
    ok = ok && (vkCreateBuffer(dev, &bci, 0, &vbuf) == VK_SUCCESS);
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(dev, vbuf, &br);
    mai.allocationSize = br.size;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    ok = ok && (vkAllocateMemory(dev, &mai, 0, &vmem) == VK_SUCCESS) &&
               (vkBindBufferMemory(dev, vbuf, vmem, 0) == VK_SUCCESS);
    void *vp_map = 0;
    ok = ok && (vkMapMemory(dev, vmem, 0, VK_WHOLE_SIZE, 0, &vp_map) == VK_SUCCESS);
    if (ok) {
        const unsigned char *s = (const unsigned char *)verts;
        unsigned char *d = (unsigned char *)vp_map;
        for (unsigned i = 0; i < sizeof(verts); i++) { d[i] = s[i]; }
        vkUnmapMemory(dev, vmem);
    }

    /* --- CommandBuffer aufzeichnen --- */
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
    uwrite("[vktest] vkdraw: objekte=");
    uwrite(ok ? "ok" : "FEHLER");
    uwrite(" (module/layout/renderpass/framebuffer/pipeline/cmdbuffer)\n");
    if (!ok) { return 0; }

    /* V3: Indirect-Draw-Buffer {vertexCount=6, instanceCount=1, firstVertex=0, firstInstance=0}. */
    VkBuffer indbuf = VK_NULL_HANDLE; VkDeviceMemory indmem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo ibci; memset(&ibci, 0, sizeof(ibci));
        ibci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; ibci.size = 16;
        ibci.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        vkCreateBuffer(dev, &ibci, 0, &indbuf);
        VkMemoryRequirements imrq; vkGetBufferMemoryRequirements(dev, indbuf, &imrq);
        VkMemoryAllocateInfo imai; memset(&imai, 0, sizeof(imai));
        imai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; imai.allocationSize = imrq.size;
        vkAllocateMemory(dev, &imai, 0, &indmem);
        vkBindBufferMemory(dev, indbuf, indmem, 0);
        void *im = 0;
        if (vkMapMemory(dev, indmem, 0, VK_WHOLE_SIZE, 0, &im) == VK_SUCCESS) {
            unsigned *ip = (unsigned *)im; ip[0]=6; ip[1]=1; ip[2]=0; ip[3]=0; vkUnmapMemory(dev, indmem);
        }
    }

    static const float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    VkCommandBufferBeginInfo cbb;
    memset(&cbb, 0, sizeof(cbb));
    cbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &cbb);
    VkClearValue clears[2];
    memset(clears, 0, sizeof(clears));
    clears[0].color.float32[0] = 0.1f; clears[0].color.float32[1] = 0.2f;
    clears[0].color.float32[2] = 0.8f; clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rbi;
    memset(&rbi, 0, sizeof(rbi));
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea.extent.width = 64; rbi.renderArea.extent.height = 64;
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDeviceSize zero_off = 0;
    vkCmdBindVertexBuffers2(cmd, 0, 1, &vbuf, &zero_off, 0, 0);   /* V3: Extended-Dynamic-State (Haupt-Draw) */
    VkPushConstantsInfo pci2; memset(&pci2, 0, sizeof(pci2));   /* V3.4: Core-1.4 Push2 */
    pci2.sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO; pci2.layout = layout;
    pci2.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pci2.offset = 0; pci2.size = 64; pci2.pValues = ident;
    vkCmdPushConstants2(cmd, &pci2);
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);      /* V3: Extended-Dynamic-State (greift real -> fernes Rot verworfen) */
    vkCmdDrawIndirect(cmd, indbuf, 0, 1, 0);   /* V3: Draw-Parameter {6,1,0,0} aus dem Buffer */
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    /* --- Submit + Fence --- */
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    ok = (vkCreateFence(dev, &fci, 0, &fence) == VK_SUCCESS);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    ok = ok && (vkQueueSubmit(q, 1, &si, fence) == VK_SUCCESS);
    ok = ok && (vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull) == VK_SUCCESS);
    uwrite("[vktest] vkdraw: submit+fence=");
    uwrite(ok ? "ok" : "FEHLER");
    uwrite("\n");
    all = all && ok;

    /* --- Readback via vkMapMemory: Farbe exakt + Tiefe --- */
    void *cmap = 0, *dmap = 0;
    int rb = (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cmap) == VK_SUCCESS) &&
             (vkMapMemory(dev, dmem, 0, VK_WHOLE_SIZE, 0, &dmap) == VK_SUCCESS);
    int ok_center = 0, ok_corner = 0, ok_depth = 0, ok_red = 0;
    if (rb) {
        const unsigned *cw = (const unsigned *)cmap;
        const float *dw = (const float *)dmap;
        unsigned center = cw[32 * 64 + 32];
        unsigned corner = cw[2 * 64 + 2];
        float dcen = dw[32 * 64 + 32];
        ok_center = (center == 0xFF00FF00u);          /* gruen opak (V1.1: Alpha=0xFF): nahes Dreieck */
        ok_red    = (center != 0xFFFF0000u);          /* fernes rot wurde VERWORFEN  */
        ok_corner = (corner == 0xFF1A33CCu);          /* Clear-Farbe (0.1,0.2,0.8, a=1) */
        ok_depth  = (dcen > 0.2499f && dcen < 0.2501f);
        vkUnmapMemory(dev, cmem);
        vkUnmapMemory(dev, dmem);
    }
    int t_rb = rb && ok_center && ok_red && ok_corner && ok_depth;
    all = all && t_rb;
    uwrite("[vktest] vkdraw: readback center=");
    uwrite(ok_center ? "gruen(nah)" : "FEHLER");
    uwrite(" ecke=");
    uwrite(ok_corner ? "clearfarbe" : "FEHLER");
    uwrite(" tiefe=");
    uwrite(ok_depth ? "0.25" : "FEHLER");
    uwrite(" ferngetestet=");
    uwrite(ok_red ? "verworfen" : "FEHLER");
    uwrite("\n");

    /* --- Push-Constants PRO DRAW (Review #7): DERSELBE CommandBuffer zeichnet das Dreieck
     * ZWEIMAL -- Draw 1 mit Identitaets-MVP (mittig sichtbar), Draw 2 mit einer MVP, die es
     * WEIT aus dem 64x64-Viewport schiebt (unsichtbar). Korrekt (Push je Draw eingefroren):
     * die Mitte ist gruen (Draw 1). Beim alten Bug (letzter Push gilt fuer ALLE Draws) wuerden
     * BEIDE Draws die Offscreen-MVP nutzen -> Mitte bleibt Clear-Farbe. Diskriminierend. */
    static const float mvp_off[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 3.0f,0,0,1 }; /* +3 in x -> offscreen */
    vkBeginCommandBuffer(cmd, &cbb);
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);   /* Draw 1: sichtbar */
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp_off); /* Draw 2: offscreen */
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    vkResetFences(dev, 1, &fence);
    vkQueueSubmit(q, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
    int ok_perdraw = 0;
    void *cmap2 = 0;
    if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cmap2) == VK_SUCCESS) {
        const unsigned *cw = (const unsigned *)cmap2;
        ok_perdraw = (cw[32 * 64 + 32] == 0xFF00FF00u);   /* gruen opak -> Draw 1 nutzte SEINEN Push */
        vkUnmapMemory(dev, cmem);
    }
    all = all && ok_perdraw;
    uwrite("[vktest] vkdraw: push-pro-draw (2 draws, Push je Draw eingefroren)=");
    uwrite(ok_perdraw ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3b: Extended-Dynamic-State REST (EDS/EDS2/EDS3) -- rasterizerDiscard, dynamisches
     * Scissor und EDS3-colorWriteMask greifen REAL in denselben Referenz-Draw ein. Bezug:
     * Mitte-Gruen 0xFF00FF00, Clear-Farbe 0xFF1A33CC (0.1,0.2,0.8). Jeder Effekt ist gegen den
     * unveraenderten Draw diskriminierend (bricht man den Override, wird die Mitte wieder gruen). */
    unsigned eds_clear = 0xFF1A33CCu;
    int ok_eds_discard = 0, ok_eds_scissor = 0, ok_eds_wmask = 0;
    /* A) rasterizerDiscardEnable=TRUE -> keine Fragmente -> Mitte bleibt Clear-Farbe. */
    vkBeginCommandBuffer(cmd, &cbb);
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
    vkCmdSetRasterizerDiscardEnable(cmd, VK_TRUE);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd); vkEndCommandBuffer(cmd);
    vkResetFences(dev, 1, &fence); vkQueueSubmit(q, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
    { void *m = 0; if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &m) == VK_SUCCESS) {
        ok_eds_discard = (((unsigned *)m)[32 * 64 + 32] == eds_clear); vkUnmapMemory(dev, cmem); } }
    /* B) dynamisches Scissor auf die 4x4-Ecke oben-links -> Mitte (32,32) ausserhalb -> Clear. */
    VkRect2D eds_scr; eds_scr.offset.x = 0; eds_scr.offset.y = 0;
    eds_scr.extent.width = 4; eds_scr.extent.height = 4;
    vkBeginCommandBuffer(cmd, &cbb);
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
    vkCmdSetScissor(cmd, 0, 1, &eds_scr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd); vkEndCommandBuffer(cmd);
    vkResetFences(dev, 1, &fence); vkQueueSubmit(q, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
    { void *m = 0; if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &m) == VK_SUCCESS) {
        ok_eds_scissor = (((unsigned *)m)[32 * 64 + 32] == eds_clear); vkUnmapMemory(dev, cmem); } }
    /* C) EDS3 colorWriteMask = nur R -> gruenes Fragment schreibt nur R(=0); G/B behalten Clear
     *    (0x33,0xCC) -> Mitte = 0xFF0033CC (weder Clear noch Voll-Gruen). */
    VkColorComponentFlags eds_wm = VK_COLOR_COMPONENT_R_BIT;
    vkBeginCommandBuffer(cmd, &cbb);
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
    vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &eds_wm);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd); vkEndCommandBuffer(cmd);
    vkResetFences(dev, 1, &fence); vkQueueSubmit(q, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
    { void *m = 0; if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &m) == VK_SUCCESS) {
        ok_eds_wmask = (((unsigned *)m)[32 * 64 + 32] == 0xFF0033CCu); vkUnmapMemory(dev, cmem); } }
    int t_eds = ok_eds_discard && ok_eds_scissor && ok_eds_wmask;
    all = all && t_eds;
    uwrite("[vktest] vk V3b: eds-rest discard=");
    uwrite(ok_eds_discard ? "leer" : "FEHLER");
    uwrite(" scissor=");
    uwrite(ok_eds_scissor ? "geklippt" : "FEHLER");
    uwrite(" writemask=");
    uwrite(ok_eds_wmask ? "nurR" : "FEHLER");
    uwrite("\n");

    /* --- V3b: dynamischer depthCompareOp -- NEVER verwirft ALLE Fragmente trotz bestandener
     * Geometrie. Bezug: normaler Draw macht die Mitte gruen (Tiefe 0.25 < Clear 1.0, LESS). Mit
     * NEVER bleibt die Mitte Clear-Farbe. Bricht man den Override, gilt Pipeline-LESS -> gruen. */
    int ok_dcmp = 0;
    vkBeginCommandBuffer(cmd, &cbb);
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_NEVER);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd); vkEndCommandBuffer(cmd);
    vkResetFences(dev, 1, &fence); vkQueueSubmit(q, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
    { void *m = 0; if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &m) == VK_SUCCESS) {
        ok_dcmp = (((unsigned *)m)[32 * 64 + 32] == 0xFF1A33CCu); vkUnmapMemory(dev, cmem); } }
    all = all && ok_dcmp;
    uwrite("[vktest] vk V3b: depth-compare-op NEVER (verwirft alle Fragmente -> Mitte Clear)=");
    uwrite(ok_dcmp ? "verworfen" : "FEHLER");
    uwrite("\n");

    /* --- V3b: Host-Image-Copy (Core 1.4) -- Memory->Image->Memory Round-Trip OHNE Command-Buffer.
     * 4x4 B8G8R8A8 (LINEAR, mappbar). vkCopyMemoryToImage schreibt 16 bekannte Pixel; ein Direkt-Map
     * prueft die erste Zeile (Offset 0..3, pitch-unabhaengig); vkCopyImageToMemory liest zurueck.
     * Memory->Image kaputt -> Map- UND Round-Trip-Check fallen; Image->Memory kaputt -> nur der
     * Round-Trip -> die beiden Checks isolieren beide Richtungen. */
    int ok_hic_w = 0, ok_hic_rt = 0;
    {
        unsigned hsrc[16], hback[16];
        for (int i = 0; i < 16; i++) { hsrc[i] = 0xFF000000u | (unsigned)(i * 0x010101); hback[i] = 0; }
        VkImageCreateInfo hci; memset(&hci, 0, sizeof(hci));
        hci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; hci.imageType = VK_IMAGE_TYPE_2D;
        hci.format = VK_FORMAT_B8G8R8A8_UNORM; hci.extent.width = 4; hci.extent.height = 4; hci.extent.depth = 1;
        hci.mipLevels = 1; hci.arrayLayers = 1; hci.samples = VK_SAMPLE_COUNT_1_BIT;
        hci.tiling = VK_IMAGE_TILING_LINEAR;
        hci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage himg = VK_NULL_HANDLE;
        int ok = (vkCreateImage(dev, &hci, 0, &himg) == VK_SUCCESS);
        VkMemoryRequirements hmr; vkGetImageMemoryRequirements(dev, himg, &hmr);
        VkMemoryAllocateInfo hmai; memset(&hmai, 0, sizeof(hmai));
        hmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; hmai.allocationSize = hmr.size;
        VkDeviceMemory hmem = VK_NULL_HANDLE;
        ok = ok && (vkAllocateMemory(dev, &hmai, 0, &hmem) == VK_SUCCESS) &&
             (vkBindImageMemory(dev, himg, hmem, 0) == VK_SUCCESS);
        if (ok) {
            VkImageSubresourceLayers sub; memset(&sub, 0, sizeof(sub));
            sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; sub.layerCount = 1;
            VkMemoryToImageCopy m2i; memset(&m2i, 0, sizeof(m2i));
            m2i.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY; m2i.pHostPointer = hsrc;
            m2i.imageSubresource = sub;
            m2i.imageExtent.width = 4; m2i.imageExtent.height = 4; m2i.imageExtent.depth = 1;
            VkCopyMemoryToImageInfo ci; memset(&ci, 0, sizeof(ci));
            ci.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO; ci.dstImage = himg;
            ci.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL; ci.regionCount = 1; ci.pRegions = &m2i;
            vkCopyMemoryToImage(dev, &ci);
            void *hm = 0;
            if (vkMapMemory(dev, hmem, 0, VK_WHOLE_SIZE, 0, &hm) == VK_SUCCESS) {
                const unsigned *ip = (const unsigned *)hm;
                ok_hic_w = (ip[0] == hsrc[0] && ip[1] == hsrc[1] && ip[2] == hsrc[2] && ip[3] == hsrc[3]);
                vkUnmapMemory(dev, hmem);
            }
            VkImageToMemoryCopy i2m; memset(&i2m, 0, sizeof(i2m));
            i2m.sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY; i2m.pHostPointer = hback;
            i2m.imageSubresource = sub;
            i2m.imageExtent.width = 4; i2m.imageExtent.height = 4; i2m.imageExtent.depth = 1;
            VkCopyImageToMemoryInfo co; memset(&co, 0, sizeof(co));
            co.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO; co.srcImage = himg;
            co.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL; co.regionCount = 1; co.pRegions = &i2m;
            vkCopyImageToMemory(dev, &co);
            ok_hic_rt = 1;
            for (int i = 0; i < 16; i++) { if (hback[i] != hsrc[i]) { ok_hic_rt = 0; } }
        }
        vkDestroyImage(dev, himg, 0); vkFreeMemory(dev, hmem, 0);
    }
    all = all && ok_hic_w && ok_hic_rt;
    uwrite("[vktest] vk V3b: host-image-copy (Mem->Img Map=");
    uwrite(ok_hic_w ? "ok" : "FEHLER");
    uwrite(", Img->Mem Round-Trip=");
    uwrite(ok_hic_rt ? "ok" : "FEHLER");
    uwrite(")\n");

    /* --- V1.1 BLENDING: zweite Pipeline (Depth AUS, 50/50-Blend via CONSTANT_ALPHA=0.5).
     * Draw 1 (opake Pipeline): gruen (0,1,0). Draw 2 (Blend-Pipeline): rot (1,0,0) darueber ->
     * res = 0.5*rot + 0.5*gruen = (0.5,0.5,0), Alpha = 0.5*1+0.5*1 = 1 -> exakt 0xFF808000. */
    VkPipelineColorBlendAttachmentState batt;
    memset(&batt, 0, sizeof(batt));
    batt.blendEnable = VK_TRUE;
    batt.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    batt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    batt.colorBlendOp = VK_BLEND_OP_ADD;
    batt.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    batt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    batt.alphaBlendOp = VK_BLEND_OP_ADD;
    batt.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo bcb;
    memset(&bcb, 0, sizeof(bcb));
    bcb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    bcb.attachmentCount = 1; bcb.pAttachments = &batt;
    bcb.blendConstants[3] = 0.5f;
    VkPipelineDepthStencilStateCreateInfo bds;
    memset(&bds, 0, sizeof(bds));
    bds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    bds.depthTestEnable = VK_FALSE; bds.depthWriteEnable = VK_FALSE;
    VkGraphicsPipelineCreateInfo bgpi = gpi;    /* Basis kopieren, Blend/Depth ueberschreiben */
    bgpi.pColorBlendState = &bcb;
    bgpi.pDepthStencilState = &bds;
    VkPipeline pipe_blend = VK_NULL_HANDLE;
    int ok_blend = (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &bgpi, 0, &pipe_blend) == VK_SUCCESS);
    if (ok_blend) {
        vkBeginCommandBuffer(cmd, &cbb);
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);        /* opak: gruen */
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_blend);  /* blend: rot 50% */
        vkCmdDraw(cmd, 3, 1, 3, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        vkResetFences(dev, 1, &fence);
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        void *cmap3 = 0;
        int got = 0;
        if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cmap3) == VK_SUCCESS) {
            got = (((const unsigned *)cmap3)[32 * 64 + 32] == 0xFF808000u);   /* 50/50 gruen+rot */
            vkUnmapMemory(dev, cmem);
        }
        ok_blend = got;
        vkDestroyPipeline(dev, pipe_blend, 0);
    }
    all = all && ok_blend;
    uwrite("[vktest] vkdraw: blending (50/50 CONSTANT_ALPHA gruen+rot -> 0xFF808000)=");
    uwrite(ok_blend ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V1.2 INDEX-DRAW: Index-Puffer [3,4,5] (uint16) -> vkCmdDrawIndexed(3) rendert die
     * verts[3..5] (ROTES Dreieck, z=0.75). Ohne Index-Fetch wuerde linear verts[0..2] (GRUEN)
     * gerendert -> center gruen. Mit Index-Fetch -> center ROT (0xFFFF0000). Diskriminierend. */
    static const unsigned short idxbuf[3] = { 3, 4, 5 };
    VkBufferCreateInfo ibci;
    memset(&ibci, 0, sizeof(ibci));
    ibci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ibci.size = sizeof(idxbuf);
    ibci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkBuffer ibuf = VK_NULL_HANDLE;
    int ok_index = (vkCreateBuffer(dev, &ibci, 0, &ibuf) == VK_SUCCESS);
    VkMemoryRequirements ibr;
    vkGetBufferMemoryRequirements(dev, ibuf, &ibr);
    mai.allocationSize = ibr.size;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    ok_index = ok_index && (vkAllocateMemory(dev, &mai, 0, &imem) == VK_SUCCESS) &&
               (vkBindBufferMemory(dev, ibuf, imem, 0) == VK_SUCCESS);
    void *imap = 0;
    if (ok_index && vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, &imap) == VK_SUCCESS) {
        for (unsigned i = 0; i < 3; i++) { ((unsigned short *)imap)[i] = idxbuf[i]; }
        vkUnmapMemory(dev, imem);
    } else { ok_index = 0; }
    if (ok_index) {
        vkBeginCommandBuffer(cmd, &cbb);
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
        vkCmdBindIndexBuffer2(cmd, ibuf, 0, VK_WHOLE_SIZE, VK_INDEX_TYPE_UINT16);   /* V3.4: Core-1.4 */
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        vkResetFences(dev, 1, &fence);
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        void *cm = 0; int got = 0;
        if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cm) == VK_SUCCESS) {
            got = (((const unsigned *)cm)[32 * 64 + 32] == 0xFFFF0000u);   /* rot -> Index [3,4,5] gefolgt */
            vkUnmapMemory(dev, cmem);
        }
        ok_index = got;
    }
    all = all && ok_index;
    uwrite("[vktest] vkdraw: index-draw (uint16 [3,4,5] -> rotes Dreieck via vkCmdDrawIndexed)=");
    uwrite(ok_index ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V1.2 INSTANCING: instanced-VS (gl_InstanceIndex), kleines Dreieck 2x gezeichnet ->
     * Instanz 0 nach LINKS (-0.25), Instanz 1 nach RECHTS (+0.25). Probe: links UND rechts gruen,
     * Mitte leer. Ohne Instancing-Schleife fehlt eine Kopie -> FEHLER. */
    static const float itri[3][6] = {
        { -0.15f, -0.3f, 0.5f,  0, 1, 0 },
        {  0.15f, -0.3f, 0.5f,  0, 1, 0 },
        {  0.0f,   0.3f, 0.5f,  0, 1, 0 },
    };
    VkShaderModule ivs = VK_NULL_HANDLE;
    smi.codeSize = sizeof(spv_inst_words); smi.pCode = spv_inst_words;
    int ok_inst = (vkCreateShaderModule(dev, &smi, 0, &ivs) == VK_SUCCESS);
    /* eigener Vertex-Puffer fuer das kleine Dreieck */
    VkBuffer itbuf = VK_NULL_HANDLE;
    bci.size = sizeof(itri);
    ok_inst = ok_inst && (vkCreateBuffer(dev, &bci, 0, &itbuf) == VK_SUCCESS);
    VkMemoryRequirements itbr;
    vkGetBufferMemoryRequirements(dev, itbuf, &itbr);
    mai.allocationSize = itbr.size;
    VkDeviceMemory itmem = VK_NULL_HANDLE;
    ok_inst = ok_inst && (vkAllocateMemory(dev, &mai, 0, &itmem) == VK_SUCCESS) &&
              (vkBindBufferMemory(dev, itbuf, itmem, 0) == VK_SUCCESS);
    void *itmap = 0;
    if (ok_inst && vkMapMemory(dev, itmem, 0, VK_WHOLE_SIZE, 0, &itmap) == VK_SUCCESS) {
        const unsigned char *sp = (const unsigned char *)itri; unsigned char *dp = itmap;
        for (unsigned i = 0; i < sizeof(itri); i++) { dp[i] = sp[i]; }
        vkUnmapMemory(dev, itmem);
    } else { ok_inst = 0; }
    /* Pipeline mit dem instanced-VS (Depth aus, kein Blend). */
    VkPipelineShaderStageCreateInfo istages[2] = { stages[0], stages[1] };
    istages[0].module = ivs;
    VkGraphicsPipelineCreateInfo igpi = gpi;
    igpi.pStages = istages;
    igpi.pDepthStencilState = &bds;      /* Depth aus (wie Blend-Test) */
    VkPipeline ipipe = VK_NULL_HANDLE;
    ok_inst = ok_inst && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &igpi, 0, &ipipe) == VK_SUCCESS);
    if (ok_inst) {
        vkBeginCommandBuffer(cmd, &cbb);
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ipipe);
        vkCmdBindVertexBuffers(cmd, 0, 1, &itbuf, &zero_off);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdDraw(cmd, 3, 2, 0, 0);       /* 3 Vertices, 2 INSTANZEN */
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        vkResetFences(dev, 1, &fence);
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        void *cm = 0; int got = 0;
        if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cm) == VK_SUCCESS) {
            const unsigned *cw = (const unsigned *)cm;
            int left  = (cw[32 * 64 + 24] == 0xFF00FF00u);   /* Instanz 0: links gruen */
            int right = (cw[32 * 64 + 40] == 0xFF00FF00u);   /* Instanz 1: rechts gruen */
            got = left && right;
            vkUnmapMemory(dev, cmem);
        }
        ok_inst = got;
        vkDestroyPipeline(dev, ipipe, 0);
    }
    all = all && ok_inst;
    uwrite("[vktest] vkdraw: instancing (gl_InstanceIndex, 2 instanzen links+rechts)=");
    uwrite(ok_inst ? "ok" : "FEHLER");
    uwrite("\n");
    if (ivs) { vkDestroyShaderModule(dev, ivs, 0); }
    vkDestroyBuffer(dev, ibuf, 0); vkFreeMemory(dev, imem, 0);
    vkDestroyBuffer(dev, itbuf, 0); vkFreeMemory(dev, itmem, 0);

    /* --- V1.3 DESCRIPTOR-SETS / UBO: Uniform-Buffer {vec4 color=(0.25,0.5,0.75,1)} an
     * set0/binding0; Fragment-Shader gibt color aus -> Dreieck = 0xFF4080BF. Ohne Descriptor-
     * Aufloesung liest der Shader Muell/0 -> andere Farbe. Diskriminierend. */
    VkDescriptorSetLayoutBinding dlb;
    memset(&dlb, 0, sizeof(dlb));
    dlb.binding = 0; dlb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dlb.descriptorCount = 1; dlb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlci;
    memset(&dlci, 0, sizeof(dlci));
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1; dlci.pBindings = &dlb;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    int ok_ubo = (vkCreateDescriptorSetLayout(dev, &dlci, 0, &dsl) == VK_SUCCESS);

    VkDescriptorPoolSize dps;
    memset(&dps, 0, sizeof(dps));
    dps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; dps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &dps;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    ok_ubo = ok_ubo && (vkCreateDescriptorPool(dev, &dpci, 0, &dpool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof(dsai));
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    ok_ubo = ok_ubo && (vkAllocateDescriptorSets(dev, &dsai, &dset) == VK_SUCCESS);

    static const float ubo_color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    VkBuffer ubuf = VK_NULL_HANDLE;
    bci.size = sizeof(ubo_color);
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    ok_ubo = ok_ubo && (vkCreateBuffer(dev, &bci, 0, &ubuf) == VK_SUCCESS);
    VkMemoryRequirements ubr;
    vkGetBufferMemoryRequirements(dev, ubuf, &ubr);
    mai.allocationSize = ubr.size;
    VkDeviceMemory umem = VK_NULL_HANDLE;
    ok_ubo = ok_ubo && (vkAllocateMemory(dev, &mai, 0, &umem) == VK_SUCCESS) &&
             (vkBindBufferMemory(dev, ubuf, umem, 0) == VK_SUCCESS);
    void *umap = 0;
    if (ok_ubo && vkMapMemory(dev, umem, 0, VK_WHOLE_SIZE, 0, &umap) == VK_SUCCESS) {
        for (int i = 0; i < 4; i++) { ((float *)umap)[i] = ubo_color[i]; }
        vkUnmapMemory(dev, umem);
    } else { ok_ubo = 0; }
    VkDescriptorBufferInfo dbi;
    memset(&dbi, 0, sizeof(dbi));
    dbi.buffer = ubuf; dbi.offset = 0; dbi.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet wds;
    memset(&wds, 0, sizeof(wds));
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = dset; wds.dstBinding = 0; wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wds.pBufferInfo = &dbi;
    if (ok_ubo) { vkUpdateDescriptorSets(dev, 1, &wds, 0, 0); }

    /* Pipeline-Layout mit Push (64) + Descriptor-Set-Layout; FS = fragubo. */
    VkPipelineLayout ulayout = VK_NULL_HANDLE;
    pli.setLayoutCount = 1; pli.pSetLayouts = &dsl;
    ok_ubo = ok_ubo && (vkCreatePipelineLayout(dev, &pli, 0, &ulayout) == VK_SUCCESS);
    VkShaderModule ufs = VK_NULL_HANDLE;
    smi.codeSize = sizeof(spv_fragubo_words); smi.pCode = spv_fragubo_words;
    ok_ubo = ok_ubo && (vkCreateShaderModule(dev, &smi, 0, &ufs) == VK_SUCCESS);
    VkPipelineShaderStageCreateInfo ustages[2] = { stages[0], stages[1] };
    ustages[1].module = ufs;
    VkGraphicsPipelineCreateInfo ugpi = gpi;
    ugpi.pStages = ustages; ugpi.layout = ulayout;
    VkPipeline upipe = VK_NULL_HANDLE;
    ok_ubo = ok_ubo && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ugpi, 0, &upipe) == VK_SUCCESS);
    int ok_pushd = 0;
    if (ok_ubo) {
        vkBeginCommandBuffer(cmd, &cbb);
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upipe);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ulayout, 0, 1, &dset, 0, 0);
        vkCmdPushConstants(cmd, ulayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        vkResetFences(dev, 1, &fence);
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        void *cm = 0; int got = 0;
        if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cm) == VK_SUCCESS) {
            got = (((const unsigned *)cm)[32 * 64 + 32] == 0xFF4080BFu);   /* UBO-Farbe */
            vkUnmapMemory(dev, cmem);
        }
        ok_ubo = got;

        /* V3b: DIESELBE UBO via PUSH-DESCRIPTOR -- KEIN Set-Objekt gebunden. Der FS muss dieselbe
         * Farbe (0xFF4080BF) ausgeben. Ohne funktionierenden Push-Pfad bleibt der Deskriptor leer
         * -> FS liest 0 -> andere Farbe. Diskriminierend (frischer CB, nur Push, kein Bind). */
        VkWriteDescriptorSet pwds; memset(&pwds, 0, sizeof(pwds));
        pwds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        pwds.dstBinding = 0; pwds.descriptorCount = 1;
        pwds.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; pwds.pBufferInfo = &dbi;
        vkBeginCommandBuffer(cmd, &cbb);
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, upipe);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
        vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ulayout, 0, 1, &pwds);
        vkCmdPushConstants(cmd, ulayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        vkResetFences(dev, 1, &fence);
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        void *pcm = 0;
        if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &pcm) == VK_SUCCESS) {
            ok_pushd = (((const unsigned *)pcm)[32 * 64 + 32] == 0xFF4080BFu);
            vkUnmapMemory(dev, cmem);
        }

        vkDestroyPipeline(dev, upipe, 0);
        vkDestroyShaderModule(dev, ufs, 0);
        vkDestroyPipelineLayout(dev, ulayout, 0);
    }
    all = all && ok_ubo;
    uwrite("[vktest] vkdraw: descriptor-ubo (set0/binding0 vec4 -> FS gibt 0xFF4080BF aus)=");
    uwrite(ok_ubo ? "ok" : "FEHLER");
    uwrite("\n");
    all = all && ok_pushd;
    uwrite("[vktest] vk V3b: push-descriptor (UBO via vkCmdPushDescriptorSet ohne Set-Objekt -> 0xFF4080BF)=");
    uwrite(ok_pushd ? "ok" : "FEHLER");
    uwrite("\n");
    vkDestroyBuffer(dev, ubuf, 0); vkFreeMemory(dev, umem, 0);

    /* --- V1.4 TEXTUR: 2x2-Textur (rot/gruen/blau/gelb) + Sampler; FS sampelt konstante Koord
     * (0.25,0.75) -> nearest -> Texel(0,1) = BLAU (0xFF0000FF). Ohne Sampling -> andere Farbe. */
    VkImageCreateInfo tic;
    memset(&tic, 0, sizeof(tic));
    tic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    tic.imageType = VK_IMAGE_TYPE_2D;
    tic.format = VK_FORMAT_B8G8R8A8_UNORM;
    tic.extent.width = 2; tic.extent.height = 2; tic.extent.depth = 1;
    tic.mipLevels = 1; tic.arrayLayers = 1; tic.samples = VK_SAMPLE_COUNT_1_BIT;
    tic.tiling = VK_IMAGE_TILING_LINEAR;
    tic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    tic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage timg = VK_NULL_HANDLE;
    int ok_tex = (vkCreateImage(dev, &tic, 0, &timg) == VK_SUCCESS);
    VkMemoryRequirements tmr;
    vkGetImageMemoryRequirements(dev, timg, &tmr);
    mai.allocationSize = tmr.size;
    VkDeviceMemory tmem = VK_NULL_HANDLE;
    ok_tex = ok_tex && (vkAllocateMemory(dev, &mai, 0, &tmem) == VK_SUCCESS) &&
             (vkBindImageMemory(dev, timg, tmem, 0) == VK_SUCCESS);
    void *tmap = 0;
    if (ok_tex && vkMapMemory(dev, tmem, 0, VK_WHOLE_SIZE, 0, &tmap) == VK_SUCCESS) {
        unsigned *tp = (unsigned *)tmap;           /* pitch = 2 px */
        tp[0] = 0xFFFF0000u; tp[1] = 0xFF00FF00u;  /* Zeile 0: rot, gruen */
        tp[2] = 0xFF0000FFu; tp[3] = 0xFFFFFF00u;  /* Zeile 1: BLAU, gelb */
        vkUnmapMemory(dev, tmem);
    } else { ok_tex = 0; }
    VkImageView tview = VK_NULL_HANDLE;
    vci.image = timg; vci.format = VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ok_tex = ok_tex && (vkCreateImageView(dev, &vci, 0, &tview) == VK_SUCCESS);
    VkSamplerCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSampler samp = VK_NULL_HANDLE;
    ok_tex = ok_tex && (vkCreateSampler(dev, &sci, 0, &samp) == VK_SUCCESS);

    /* Descriptor: COMBINED_IMAGE_SAMPLER an set0/binding0. */
    VkDescriptorSetLayoutBinding tlb;
    memset(&tlb, 0, sizeof(tlb));
    tlb.binding = 0; tlb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tlb.descriptorCount = 1; tlb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dlci.pBindings = &tlb;
    VkDescriptorSetLayout tdsl = VK_NULL_HANDLE;
    ok_tex = ok_tex && (vkCreateDescriptorSetLayout(dev, &dlci, 0, &tdsl) == VK_SUCCESS);
    dps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VkDescriptorPool tdpool = VK_NULL_HANDLE;
    ok_tex = ok_tex && (vkCreateDescriptorPool(dev, &dpci, 0, &tdpool) == VK_SUCCESS);
    dsai.descriptorPool = tdpool; dsai.pSetLayouts = &tdsl;
    VkDescriptorSet tdset = VK_NULL_HANDLE;
    ok_tex = ok_tex && (vkAllocateDescriptorSets(dev, &dsai, &tdset) == VK_SUCCESS);
    VkDescriptorImageInfo tii;
    memset(&tii, 0, sizeof(tii));
    tii.sampler = samp; tii.imageView = tview;
    tii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet twds;
    memset(&twds, 0, sizeof(twds));
    twds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    twds.dstSet = tdset; twds.dstBinding = 0; twds.descriptorCount = 1;
    twds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; twds.pImageInfo = &tii;
    if (ok_tex) { vkUpdateDescriptorSets(dev, 1, &twds, 0, 0); }

    VkPipelineLayout tlayout = VK_NULL_HANDLE;
    pli.pSetLayouts = &tdsl;
    ok_tex = ok_tex && (vkCreatePipelineLayout(dev, &pli, 0, &tlayout) == VK_SUCCESS);
    VkShaderModule tfs = VK_NULL_HANDLE;
    smi.codeSize = sizeof(spv_fragtex_words); smi.pCode = spv_fragtex_words;
    ok_tex = ok_tex && (vkCreateShaderModule(dev, &smi, 0, &tfs) == VK_SUCCESS);
    VkPipelineShaderStageCreateInfo tstages[2] = { stages[0], stages[1] };
    tstages[1].module = tfs;
    VkGraphicsPipelineCreateInfo tgpi = gpi;
    tgpi.pStages = tstages; tgpi.layout = tlayout;
    VkPipeline tpipe = VK_NULL_HANDLE;
    ok_tex = ok_tex && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &tgpi, 0, &tpipe) == VK_SUCCESS);
    if (ok_tex) {
        vkBeginCommandBuffer(cmd, &cbb);
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tpipe);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
        VkBindDescriptorSetsInfo bdsi2; memset(&bdsi2, 0, sizeof(bdsi2));   /* V3.4: Core-1.4 BindDescriptorSets2 */
        bdsi2.sType = VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO;
        bdsi2.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; bdsi2.layout = tlayout;
        bdsi2.firstSet = 0; bdsi2.descriptorSetCount = 1; bdsi2.pDescriptorSets = &tdset;
        vkCmdBindDescriptorSets2(cmd, &bdsi2);
        vkCmdPushConstants(cmd, tlayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        vkResetFences(dev, 1, &fence);
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        void *cm = 0; int got = 0;
        if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cm) == VK_SUCCESS) {
            got = (((const unsigned *)cm)[32 * 64 + 32] == 0xFF0000FFu);   /* blauer Texel */
            vkUnmapMemory(dev, cmem);
        }
        ok_tex = got;
        vkDestroyPipeline(dev, tpipe, 0);
        vkDestroyShaderModule(dev, tfs, 0);
        vkDestroyPipelineLayout(dev, tlayout, 0);
    }
    all = all && ok_tex;
    uwrite("[vktest] vkdraw: textur (2x2 sampler2D nearest, texel(0,1) -> 0xFF0000FF)=");
    uwrite(ok_tex ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V2.6 IMAGE-OPS: textureSize + texelFetch. Reuse Textur/Sampler/Descriptor der V1.4-Probe
     * (timg/tview/samp/tdsl/tdset noch am Leben). FS: ivec2 sz=textureSize(tex,0);
     * outColor=texelFetch(tex, ivec2(sz.x-1,1), 0) -> Texel(1,1)=gelb 0xFFFFFF00. QuerySizeLod (sz.x)
     * + ImageFetch aneinandergebunden -> falsche Groesse ODER falscher Fetch faellt auf. */
    int ok_fetch = ok_tex;
    {
        VkPipelineLayout flayout = VK_NULL_HANDLE;
        pli.pSetLayouts = &tdsl;
        ok_fetch = ok_fetch && (vkCreatePipelineLayout(dev, &pli, 0, &flayout) == VK_SUCCESS);
        VkShaderModule ffs = VK_NULL_HANDLE;
        smi.codeSize = sizeof(spv_fragfetch_words); smi.pCode = spv_fragfetch_words;
        ok_fetch = ok_fetch && (vkCreateShaderModule(dev, &smi, 0, &ffs) == VK_SUCCESS);
        VkPipelineShaderStageCreateInfo fstages[2] = { stages[0], stages[1] };
        fstages[1].module = ffs;
        VkGraphicsPipelineCreateInfo fgpi = gpi;
        fgpi.pStages = fstages; fgpi.layout = flayout;
        VkPipeline fpipe = VK_NULL_HANDLE;
        ok_fetch = ok_fetch && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &fgpi, 0, &fpipe) == VK_SUCCESS);
        if (ok_fetch) {
            vkBeginCommandBuffer(cmd, &cbb);
            vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fpipe);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flayout, 0, 1, &tdset, 0, 0);
            vkCmdPushConstants(cmd, flayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            vkEndCommandBuffer(cmd);
            vkResetFences(dev, 1, &fence);
            vkQueueSubmit(q, 1, &si, fence);
            vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
            void *cm = 0; int got = 0;
            if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cm) == VK_SUCCESS) {
                got = (((const unsigned *)cm)[32 * 64 + 32] == 0xFFFFFF00u);   /* gelber Texel(1,1) */
                vkUnmapMemory(dev, cmem);
            }
            ok_fetch = got;
            vkDestroyPipeline(dev, fpipe, 0);
            vkDestroyShaderModule(dev, ffs, 0);
            vkDestroyPipelineLayout(dev, flayout, 0);
        }
    }
    all = all && ok_fetch;
    uwrite("[vktest] vkdraw V2.6: image-ops (textureSize+texelFetch, texel(1,1) -> 0xFFFFFF00)=");
    uwrite(ok_fetch ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V2.6 GATHER: textureGather(tex, vec2(0.5,0.5), 0) sammelt R der 4 Zentrums-Texel in
     * Spez-Reihenfolge (blau,gelb,gruen,rot).R = (0,1,0,1) -> outColor -> 0xFF00FF00 (gruen).
     * Reuse Textur/Sampler/Descriptor. Falsche Gather-Reihenfolge -> andere Farbe. */
    int ok_gather = ok_tex;
    {
        VkPipelineLayout glayout = VK_NULL_HANDLE;
        pli.pSetLayouts = &tdsl;
        ok_gather = ok_gather && (vkCreatePipelineLayout(dev, &pli, 0, &glayout) == VK_SUCCESS);
        VkShaderModule gfs = VK_NULL_HANDLE;
        smi.codeSize = sizeof(spv_fraggather_words); smi.pCode = spv_fraggather_words;
        ok_gather = ok_gather && (vkCreateShaderModule(dev, &smi, 0, &gfs) == VK_SUCCESS);
        VkPipelineShaderStageCreateInfo gstages[2] = { stages[0], stages[1] };
        gstages[1].module = gfs;
        VkGraphicsPipelineCreateInfo ggpi = gpi;
        ggpi.pStages = gstages; ggpi.layout = glayout;
        VkPipeline gpipe = VK_NULL_HANDLE;
        ok_gather = ok_gather && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ggpi, 0, &gpipe) == VK_SUCCESS);
        if (ok_gather) {
            vkBeginCommandBuffer(cmd, &cbb);
            vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gpipe);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glayout, 0, 1, &tdset, 0, 0);
            vkCmdPushConstants(cmd, glayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            vkEndCommandBuffer(cmd);
            vkResetFences(dev, 1, &fence);
            vkQueueSubmit(q, 1, &si, fence);
            vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
            void *cm = 0; int got = 0;
            if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &cm) == VK_SUCCESS) {
                got = (((const unsigned *)cm)[32 * 64 + 32] == 0xFF00FF00u);   /* gather R = (0,1,0,1) -> gruen */
                vkUnmapMemory(dev, cmem);
            }
            ok_gather = got;
            vkDestroyPipeline(dev, gpipe, 0);
            vkDestroyShaderModule(dev, gfs, 0);
            vkDestroyPipelineLayout(dev, glayout, 0);
        }
    }
    all = all && ok_gather;
    uwrite("[vktest] vkdraw V2.6: textureGather (R der 4 Zentrums-Texel -> 0xFF00FF00)=");
    uwrite(ok_gather ? "ok" : "FEHLER");
    uwrite("\n");

    vkDestroySampler(dev, samp, 0);
    vkDestroyImageView(dev, tview, 0);
    vkDestroyImage(dev, timg, 0); vkFreeMemory(dev, tmem, 0);
    vkDestroyDescriptorPool(dev, tdpool, 0); vkDestroyDescriptorPool(dev, dpool, 0);
    vkDestroyDescriptorSetLayout(dev, tdsl, 0); vkDestroyDescriptorSetLayout(dev, dsl, 0);

    /* --- V1.5 MRT: 2 Farb-Attachments; FS schreibt o0=rot(loc0), o1=gruen(loc1) -> Attachment 0
     * = rot, Attachment 1 = gruen. Beweist getrennte Ausgaben (nicht vertauscht/nur eine). */
    ic.format = VK_FORMAT_B8G8R8A8_UNORM;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage cimg2 = VK_NULL_HANDLE;
    int ok_mrt = (vkCreateImage(dev, &ic, 0, &cimg2) == VK_SUCCESS);
    VkMemoryRequirements cr2;
    vkGetImageMemoryRequirements(dev, cimg2, &cr2);
    mai.allocationSize = cr2.size;
    VkDeviceMemory cmem2 = VK_NULL_HANDLE;
    ok_mrt = ok_mrt && (vkAllocateMemory(dev, &mai, 0, &cmem2) == VK_SUCCESS) &&
             (vkBindImageMemory(dev, cimg2, cmem2, 0) == VK_SUCCESS);
    VkImageView cview2 = VK_NULL_HANDLE;
    vci.image = cimg2; vci.format = VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ok_mrt = ok_mrt && (vkCreateImageView(dev, &vci, 0, &cview2) == VK_SUCCESS);

    VkAttachmentDescription matt[3];
    memset(matt, 0, sizeof(matt));
    for (int i = 0; i < 2; i++) {
        matt[i].format = VK_FORMAT_B8G8R8A8_UNORM; matt[i].samples = VK_SAMPLE_COUNT_1_BIT;
        matt[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; matt[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        matt[i].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    matt[2].format = VK_FORMAT_D32_SFLOAT; matt[2].samples = VK_SAMPLE_COUNT_1_BIT;
    matt[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; matt[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    matt[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference mcref[2], mdref;
    memset(mcref, 0, sizeof(mcref)); memset(&mdref, 0, sizeof(mdref));
    mcref[0].attachment = 0; mcref[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    mcref[1].attachment = 1; mcref[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    mdref.attachment = 2; mdref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription msub;
    memset(&msub, 0, sizeof(msub));
    msub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    msub.colorAttachmentCount = 2; msub.pColorAttachments = mcref;
    msub.pDepthStencilAttachment = &mdref;
    VkRenderPassCreateInfo mrpi;
    memset(&mrpi, 0, sizeof(mrpi));
    mrpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    mrpi.attachmentCount = 3; mrpi.pAttachments = matt;
    mrpi.subpassCount = 1; mrpi.pSubpasses = &msub;
    VkRenderPass mrp = VK_NULL_HANDLE;
    ok_mrt = ok_mrt && (vkCreateRenderPass(dev, &mrpi, 0, &mrp) == VK_SUCCESS);
    VkImageView mfbat[3] = { cview, cview2, dview };
    VkFramebufferCreateInfo mfbi;
    memset(&mfbi, 0, sizeof(mfbi));
    mfbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    mfbi.renderPass = mrp; mfbi.attachmentCount = 3; mfbi.pAttachments = mfbat;
    mfbi.width = 64; mfbi.height = 64; mfbi.layers = 1;
    VkFramebuffer mfb = VK_NULL_HANDLE;
    ok_mrt = ok_mrt && (vkCreateFramebuffer(dev, &mfbi, 0, &mfb) == VK_SUCCESS);
    VkShaderModule mfs = VK_NULL_HANDLE;
    smi.codeSize = sizeof(spv_fragmrt_words); smi.pCode = spv_fragmrt_words;
    ok_mrt = ok_mrt && (vkCreateShaderModule(dev, &smi, 0, &mfs) == VK_SUCCESS);
    VkPipelineColorBlendAttachmentState mba[2];
    memset(mba, 0, sizeof(mba));
    mba[0].colorWriteMask = 0xF; mba[1].colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo mcb;
    memset(&mcb, 0, sizeof(mcb));
    mcb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    mcb.attachmentCount = 2; mcb.pAttachments = mba;
    VkPipelineShaderStageCreateInfo mstages[2] = { stages[0], stages[1] };
    mstages[1].module = mfs;
    VkGraphicsPipelineCreateInfo mgpi = gpi;
    mgpi.pStages = mstages; mgpi.renderPass = mrp; mgpi.pColorBlendState = &mcb;
    VkPipeline mpipe = VK_NULL_HANDLE;
    ok_mrt = ok_mrt && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &mgpi, 0, &mpipe) == VK_SUCCESS);
    if (ok_mrt) {
        VkClearValue mclr[3];
        memset(mclr, 0, sizeof(mclr));
        mclr[0].color.float32[2] = 1.0f; mclr[0].color.float32[3] = 1.0f;   /* blau */
        mclr[1].color.float32[2] = 1.0f; mclr[1].color.float32[3] = 1.0f;
        mclr[2].depthStencil.depth = 1.0f;
        VkRenderPassBeginInfo mrbi;
        memset(&mrbi, 0, sizeof(mrbi));
        mrbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        mrbi.renderPass = mrp; mrbi.framebuffer = mfb;
        mrbi.renderArea.extent.width = 64; mrbi.renderArea.extent.height = 64;
        mrbi.clearValueCount = 3; mrbi.pClearValues = mclr;
        vkBeginCommandBuffer(cmd, &cbb);
        vkCmdBeginRenderPass(cmd, &mrbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mpipe);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &zero_off);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        vkResetFences(dev, 1, &fence);
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        void *m0 = 0, *m1 = 0; int a0 = 0, a1 = 0;
        if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &m0) == VK_SUCCESS) {
            a0 = (((const unsigned *)m0)[32 * 64 + 32] == 0xFFFF0000u); vkUnmapMemory(dev, cmem);
        }
        if (vkMapMemory(dev, cmem2, 0, VK_WHOLE_SIZE, 0, &m1) == VK_SUCCESS) {
            a1 = (((const unsigned *)m1)[32 * 64 + 32] == 0xFF00FF00u); vkUnmapMemory(dev, cmem2);
        }
        ok_mrt = a0 && a1;
        vkDestroyPipeline(dev, mpipe, 0);
    }
    all = all && ok_mrt;
    uwrite("[vktest] vkdraw: mrt (2 attachments: o0=rot@0, o1=gruen@1)=");
    uwrite(ok_mrt ? "ok" : "FEHLER");
    uwrite("\n");
    if (mfs) { vkDestroyShaderModule(dev, mfs, 0); }
    vkDestroyFramebuffer(dev, mfb, 0); vkDestroyRenderPass(dev, mrp, 0);
    vkDestroyImageView(dev, cview2, 0); vkDestroyImage(dev, cimg2, 0); vkFreeMemory(dev, cmem2, 0);

    /* --- V1.9 TRANSFER: FillBuffer(A, 0xDEADBEEF) -> CopyBuffer(A->B) -> B enthaelt das Muster.
     * Plus ClearColorImage(cimg, gelb) -> cmem-Mitte = 0xFFFFFF00. */
    VkBuffer bufA = VK_NULL_HANDLE, bufB = VK_NULL_HANDLE;
    bci.size = 16; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    int ok_xfer = (vkCreateBuffer(dev, &bci, 0, &bufA) == VK_SUCCESS) &&
                  (vkCreateBuffer(dev, &bci, 0, &bufB) == VK_SUCCESS);
    VkMemoryRequirements xar, xbr;
    vkGetBufferMemoryRequirements(dev, bufA, &xar);
    vkGetBufferMemoryRequirements(dev, bufB, &xbr);
    VkDeviceMemory xamem = VK_NULL_HANDLE, xbmem = VK_NULL_HANDLE;
    mai.allocationSize = xar.size;
    ok_xfer = ok_xfer && (vkAllocateMemory(dev, &mai, 0, &xamem) == VK_SUCCESS) &&
              (vkBindBufferMemory(dev, bufA, xamem, 0) == VK_SUCCESS);
    mai.allocationSize = xbr.size;
    ok_xfer = ok_xfer && (vkAllocateMemory(dev, &mai, 0, &xbmem) == VK_SUCCESS) &&
              (vkBindBufferMemory(dev, bufB, xbmem, 0) == VK_SUCCESS);
    if (ok_xfer) {
        VkBufferCopy bcpy;
        memset(&bcpy, 0, sizeof(bcpy));
        bcpy.srcOffset = 0; bcpy.dstOffset = 0; bcpy.size = 16;
        VkClearColorValue ccol;
        memset(&ccol, 0, sizeof(ccol));
        ccol.float32[0] = 1.0f; ccol.float32[1] = 1.0f; ccol.float32[2] = 0.0f; ccol.float32[3] = 1.0f; /* gelb */
        VkImageSubresourceRange rng;
        memset(&rng, 0, sizeof(rng));
        rng.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rng.levelCount = 1; rng.layerCount = 1;
        vkBeginCommandBuffer(cmd, &cbb);
        vkCmdFillBuffer(cmd, bufA, 0, 16, 0xDEADBEEFu);
        vkCmdCopyBuffer(cmd, bufA, bufB, 1, &bcpy);
        vkCmdClearColorImage(cmd, cimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccol, 1, &rng);
        vkEndCommandBuffer(cmd);
        vkResetFences(dev, 1, &fence);
        vkQueueSubmit(q, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull);
        int okB = 0, okC = 0;
        void *mb = 0, *mc = 0;
        if (vkMapMemory(dev, xbmem, 0, VK_WHOLE_SIZE, 0, &mb) == VK_SUCCESS) {
            const unsigned *bw = (const unsigned *)mb;
            okB = (bw[0] == 0xDEADBEEFu && bw[3] == 0xDEADBEEFu);   /* Fill->Copy angekommen */
            vkUnmapMemory(dev, xbmem);
        }
        if (vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, &mc) == VK_SUCCESS) {
            okC = (((const unsigned *)mc)[32 * 64 + 32] == 0xFFFFFF00u);   /* Clear-Image gelb */
            vkUnmapMemory(dev, cmem);
        }
        ok_xfer = okB && okC;
    }
    all = all && ok_xfer;
    uwrite("[vktest] vkdraw: transfer (FillBuffer+CopyBuffer=0xDEADBEEF, ClearColorImage=gelb)=");
    uwrite(ok_xfer ? "ok" : "FEHLER");
    uwrite("\n");
    vkDestroyBuffer(dev, bufA, 0); vkFreeMemory(dev, xamem, 0);
    vkDestroyBuffer(dev, bufB, 0); vkFreeMemory(dev, xbmem, 0);

    /* --- V1.8: Query-Pools (Occlusion + Timestamp) + Events ---
     * Occlusion zweiseitig: Query 0 umschliesst einen Dreieck-Draw -> Sample-Count > 0;
     * Query 1 umschliesst KEINEN Draw -> exakt 0. Timestamp: t1 > t0 (monoton). Events: Host
     * set/reset/get + CmdSetEvent im Submit. Wiederverwendet rp/fb/pipe/layout/vbuf/cpool. */
    int ok_q = 1, ok_occl = 0, ok_ts = 0, ok_ev = 0;
    {
        VkQueryPoolCreateInfo qpi;
        memset(&qpi, 0, sizeof(qpi));
        qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType = VK_QUERY_TYPE_OCCLUSION; qpi.queryCount = 2;
        VkQueryPool occl = VK_NULL_HANDLE;
        ok_q = ok_q && (vkCreateQueryPool(dev, &qpi, 0, &occl) == VK_SUCCESS);
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = 2;
        VkQueryPool tsp = VK_NULL_HANDLE;
        ok_q = ok_q && (vkCreateQueryPool(dev, &qpi, 0, &tsp) == VK_SUCCESS);

        VkCommandBufferAllocateInfo qcbi;
        memset(&qcbi, 0, sizeof(qcbi));
        qcbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        qcbi.commandPool = cpool; qcbi.commandBufferCount = 1;
        VkCommandBuffer qcmd = VK_NULL_HANDLE;
        ok_q = ok_q && (vkAllocateCommandBuffers(dev, &qcbi, &qcmd) == VK_SUCCESS);

        VkEventCreateInfo eci; memset(&eci, 0, sizeof(eci));
        eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        VkEvent ev = VK_NULL_HANDLE, ev2 = VK_NULL_HANDLE;
        ok_q = ok_q && (vkCreateEvent(dev, &eci, 0, &ev) == VK_SUCCESS);
        ok_q = ok_q && (vkCreateEvent(dev, &eci, 0, &ev2) == VK_SUCCESS);

        /* Host-Event: frisch = RESET, nach Set = SET, nach Reset = RESET. */
        int e0 = (vkGetEventStatus(dev, ev) == VK_EVENT_RESET);
        vkSetEvent(dev, ev);
        int e1 = (vkGetEventStatus(dev, ev) == VK_EVENT_SET);
        vkResetEvent(dev, ev);
        int e2 = (vkGetEventStatus(dev, ev) == VK_EVENT_RESET);

        VkCommandBufferBeginInfo qbb; memset(&qbb, 0, sizeof(qbb));
        qbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(qcmd, &qbb);
        vkCmdResetQueryPool(qcmd, occl, 0, 2);          /* Queries vor Gebrauch reset (ausserhalb RP) */
        vkCmdResetQueryPool(qcmd, tsp, 0, 2);
        vkCmdWriteTimestamp2(qcmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, tsp, 0);   /* V3: Sync2-Timestamp */
        VkClearValue qclears[2];
        memset(qclears, 0, sizeof(qclears));
        qclears[1].depthStencil.depth = 1.0f;           /* Tiefe auf 1.0 -> Dreieck(0.25) besteht */
        VkRenderPassBeginInfo qrbi; memset(&qrbi, 0, sizeof(qrbi));
        qrbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        qrbi.renderPass = rp; qrbi.framebuffer = fb;
        qrbi.renderArea.extent.width = 64; qrbi.renderArea.extent.height = 64;
        qrbi.clearValueCount = 2; qrbi.pClearValues = qclears;
        vkCmdBeginRenderPass(qcmd, &qrbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(qcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize qz = 0;
        vkCmdBindVertexBuffers(qcmd, 0, 1, &vbuf, &qz);
        vkCmdPushConstants(qcmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdBeginQuery(qcmd, occl, 0, 0);
        vkCmdDraw(qcmd, 6, 1, 0, 0);                     /* Dreieck(e) -> Fragmente werden gezaehlt */
        vkCmdEndQuery(qcmd, occl, 0);
        vkCmdBeginQuery(qcmd, occl, 1, 0);
        /* absichtlich KEIN Draw -> Query 1 muss 0 zaehlen */
        vkCmdEndQuery(qcmd, occl, 1);
        vkCmdEndRenderPass(qcmd);
        vkCmdWriteTimestamp2(qcmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, tsp, 1);   /* V3: Sync2-Timestamp */
        vkCmdSetEvent(qcmd, ev2, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);   /* Device setzt ev2 */
        vkEndCommandBuffer(qcmd);

        VkFence qfence = VK_NULL_HANDLE;
        vkCreateFence(dev, &fci, 0, &qfence);
        VkSubmitInfo qsi; memset(&qsi, 0, sizeof(qsi));
        qsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        qsi.commandBufferCount = 1; qsi.pCommandBuffers = &qcmd;
        vkQueueSubmit(q, 1, &qsi, qfence);
        vkWaitForFences(dev, 1, &qfence, VK_TRUE, ~0ull);

        unsigned long long occres[2] = { 123, 123 };
        VkResult qr = vkGetQueryPoolResults(dev, occl, 0, 2, sizeof(occres), occres,
                                            sizeof(unsigned long long),
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        ok_occl = (qr == VK_SUCCESS) && (occres[0] > 0) && (occres[1] == 0);

        unsigned long long tsres[2] = { 0, 0 };
        vkGetQueryPoolResults(dev, tsp, 0, 2, sizeof(tsres), tsres, sizeof(unsigned long long),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        ok_ts = (tsres[1] > tsres[0]);

        int e3 = (vkGetEventStatus(dev, ev2) == VK_EVENT_SET);   /* im Submit gesetzt */
        ok_ev = e0 && e1 && e2 && e3;

        vkDestroyEvent(dev, ev, 0); vkDestroyEvent(dev, ev2, 0);
        vkDestroyQueryPool(dev, occl, 0); vkDestroyQueryPool(dev, tsp, 0);
        vkDestroyFence(dev, qfence, 0);
        vkFreeCommandBuffers(dev, cpool, 1, &qcmd);
    }
    all = all && ok_q && ok_occl && ok_ts && ok_ev;
    uwrite("[vktest] vkdraw V1.8: query+event=");
    uwrite((ok_q && ok_occl && ok_ts && ok_ev) ? "ok" : "FEHLER");
    uwrite(" (occlusion draw>0+leer=0, timestamp t1>t0, event host+device)\n");

    /* --- V1.7: COMPUTE -- Compute-Shader schreibt data[gl_GlobalInvocationID.x] = x*3 in einen
     * Storage-Buffer; Dispatch(8,1,1) -> data[i]==i*3. Beweist Compute-Pipeline + gl_GlobalInvocationID
     * + Array-Index (AccessChain) + StorageBuffer-Write im Interpreter. SSBO mit 0xFFFFFFFF vorbelegt:
     * ein Nicht-Schreiben faellt auf. Wiederverwendet bci/mai/smi/fci/cpool. */
    int ok_comp = 1, ok_compdata = 0;
    {
        const unsigned NELEM = 8;
        VkBuffer sbuf = VK_NULL_HANDLE;
        bci.size = NELEM * sizeof(unsigned);
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        ok_comp = ok_comp && (vkCreateBuffer(dev, &bci, 0, &sbuf) == VK_SUCCESS);
        VkMemoryRequirements sbr;
        vkGetBufferMemoryRequirements(dev, sbuf, &sbr);
        mai.allocationSize = sbr.size;
        VkDeviceMemory smem = VK_NULL_HANDLE;
        ok_comp = ok_comp && (vkAllocateMemory(dev, &mai, 0, &smem) == VK_SUCCESS) &&
                  (vkBindBufferMemory(dev, sbuf, smem, 0) == VK_SUCCESS);
        void *smap = 0;
        if (ok_comp && vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &smap) == VK_SUCCESS) {
            for (unsigned i = 0; i < NELEM; i++) { ((unsigned *)smap)[i] = 0xFFFFFFFFu; }
            vkUnmapMemory(dev, smem);
        } else { ok_comp = 0; }

        VkDescriptorSetLayoutBinding cdlb;
        memset(&cdlb, 0, sizeof(cdlb));
        cdlb.binding = 0; cdlb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cdlb.descriptorCount = 1; cdlb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo cdlci;
        memset(&cdlci, 0, sizeof(cdlci));
        cdlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        cdlci.bindingCount = 1; cdlci.pBindings = &cdlb;
        VkDescriptorSetLayout cdsl = VK_NULL_HANDLE;
        ok_comp = ok_comp && (vkCreateDescriptorSetLayout(dev, &cdlci, 0, &cdsl) == VK_SUCCESS);
        VkDescriptorPoolSize cdps;
        memset(&cdps, 0, sizeof(cdps));
        cdps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cdps.descriptorCount = 1;
        VkDescriptorPoolCreateInfo cdpci;
        memset(&cdpci, 0, sizeof(cdpci));
        cdpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        cdpci.maxSets = 1; cdpci.poolSizeCount = 1; cdpci.pPoolSizes = &cdps;
        VkDescriptorPool cdpool = VK_NULL_HANDLE;
        ok_comp = ok_comp && (vkCreateDescriptorPool(dev, &cdpci, 0, &cdpool) == VK_SUCCESS);
        VkDescriptorSetAllocateInfo cdsai;
        memset(&cdsai, 0, sizeof(cdsai));
        cdsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        cdsai.descriptorPool = cdpool; cdsai.descriptorSetCount = 1; cdsai.pSetLayouts = &cdsl;
        VkDescriptorSet cdset = VK_NULL_HANDLE;
        ok_comp = ok_comp && (vkAllocateDescriptorSets(dev, &cdsai, &cdset) == VK_SUCCESS);
        VkDescriptorBufferInfo cdbi;
        memset(&cdbi, 0, sizeof(cdbi));
        cdbi.buffer = sbuf; cdbi.offset = 0; cdbi.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet cwds;
        memset(&cwds, 0, sizeof(cwds));
        cwds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        cwds.dstSet = cdset; cwds.dstBinding = 0; cwds.descriptorCount = 1;
        cwds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cwds.pBufferInfo = &cdbi;
        /* V3: Descriptor via Update-Template statt direkt binden -> beweist das Template durch die
         * data[i]-Assertion des Compute-Tests (falsches Template -> SSBO nicht gebunden -> data falsch). */
        if (ok_comp) {
            VkDescriptorUpdateTemplateEntry uent; memset(&uent, 0, sizeof(uent));
            uent.dstBinding = 0; uent.dstArrayElement = 0; uent.descriptorCount = 1;
            uent.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; uent.offset = 0; uent.stride = 0;
            VkDescriptorUpdateTemplateCreateInfo utci; memset(&utci, 0, sizeof(utci));
            utci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
            utci.descriptorUpdateEntryCount = 1; utci.pDescriptorUpdateEntries = &uent;
            utci.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
            utci.descriptorSetLayout = cdsl;
            VkDescriptorUpdateTemplate utempl = VK_NULL_HANDLE;
            if (vkCreateDescriptorUpdateTemplate(dev, &utci, 0, &utempl) == VK_SUCCESS) {
                vkUpdateDescriptorSetWithTemplate(dev, cdset, utempl, &cdbi);
                vkDestroyDescriptorUpdateTemplate(dev, utempl, 0);
            } else { ok_comp = 0; }
        }

        VkShaderModule cs = VK_NULL_HANDLE;
        smi.codeSize = sizeof(spv_comp_words); smi.pCode = spv_comp_words;
        ok_comp = ok_comp && (vkCreateShaderModule(dev, &smi, 0, &cs) == VK_SUCCESS);
        VkPipelineLayoutCreateInfo cpli;
        memset(&cpli, 0, sizeof(cpli));
        cpli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        cpli.setLayoutCount = 1; cpli.pSetLayouts = &cdsl;
        VkPipelineLayout clayout = VK_NULL_HANDLE;
        ok_comp = ok_comp && (vkCreatePipelineLayout(dev, &cpli, 0, &clayout) == VK_SUCCESS);
        VkComputePipelineCreateInfo cpci;
        memset(&cpci, 0, sizeof(cpci));
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = cs; cpci.stage.pName = "main";
        cpci.layout = clayout;
        VkPipeline cpipe = VK_NULL_HANDLE;
        ok_comp = ok_comp && (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, 0, &cpipe) == VK_SUCCESS);

        VkCommandBufferAllocateInfo ccbi;
        memset(&ccbi, 0, sizeof(ccbi));
        ccbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ccbi.commandPool = cpool; ccbi.commandBufferCount = 1;
        VkCommandBuffer ccmd = VK_NULL_HANDLE;
        ok_comp = ok_comp && (vkAllocateCommandBuffers(dev, &ccbi, &ccmd) == VK_SUCCESS);
        VkCommandBufferBeginInfo ccbb; memset(&ccbb, 0, sizeof(ccbb));
        ccbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(ccmd, &ccbb);
        vkCmdBindPipeline(ccmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
        vkCmdBindDescriptorSets(ccmd, VK_PIPELINE_BIND_POINT_COMPUTE, clayout, 0, 1, &cdset, 0, 0);
        vkCmdDispatch(ccmd, NELEM, 1, 1);
        vkEndCommandBuffer(ccmd);

        VkFence cfence = VK_NULL_HANDLE;
        vkCreateFence(dev, &fci, 0, &cfence);
        VkSubmitInfo csi; memset(&csi, 0, sizeof(csi));
        csi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        csi.commandBufferCount = 1; csi.pCommandBuffers = &ccmd;
        vkQueueSubmit(q, 1, &csi, cfence);
        vkWaitForFences(dev, 1, &cfence, VK_TRUE, ~0ull);

        void *smap2 = 0;
        if (ok_comp && vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &smap2) == VK_SUCCESS) {
            ok_compdata = 1;
            for (unsigned i = 0; i < NELEM; i++) {   /* V2.2/V2.6: Integer- + Bitfeld-Ops referenz-berechnet */
                unsigned a = i * 7u;
                unsigned br = 0; for (int k = 0; k < 32; k++) { br |= ((i >> k) & 1u) << (31 - k); }  /* bitReverse(i) */
                unsigned bc = 0; { unsigned v = i; while (v) { bc += v & 1u; v >>= 1; } }             /* bitCount(i) */
                unsigned expect = (a / 3u) + (a % 5u) + (i << 2) + (i & 6u) + (i | 1u) + (i ^ 3u)
                                  + bc + br + (i & 3u);
                if (((unsigned *)smap2)[i] != expect) { ok_compdata = 0; }
            }
            vkUnmapMemory(dev, smem);
        }

        /* V2.6 ATOMICS: atomicAdd(data[0], gid.x+1) ueber 8 Invokationen -> data[0]==36 (1+..+8).
         * Reuse SSBO/Descriptor/Layout/CmdBuf des Compute-Tests; data[0] als Akkumulator genullt. */
        int ok_atomic = ok_comp;
        {
            void *am = 0;
            if (ok_atomic && vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &am) == VK_SUCCESS) {
                ((unsigned *)am)[0] = 0;
                vkUnmapMemory(dev, smem);
            } else { ok_atomic = 0; }
            VkShaderModule acs = VK_NULL_HANDLE;
            smi.codeSize = sizeof(spv_compatomic_words); smi.pCode = spv_compatomic_words;
            ok_atomic = ok_atomic && (vkCreateShaderModule(dev, &smi, 0, &acs) == VK_SUCCESS);
            VkComputePipelineCreateInfo acpci; memset(&acpci, 0, sizeof(acpci));
            acpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            acpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            acpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            acpci.stage.module = acs; acpci.stage.pName = "main";
            acpci.layout = clayout;
            VkPipeline acpipe = VK_NULL_HANDLE;
            ok_atomic = ok_atomic && (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &acpci, 0, &acpipe) == VK_SUCCESS);
            if (ok_atomic) {
                vkBeginCommandBuffer(ccmd, &ccbb);
                vkCmdBindPipeline(ccmd, VK_PIPELINE_BIND_POINT_COMPUTE, acpipe);
                vkCmdBindDescriptorSets(ccmd, VK_PIPELINE_BIND_POINT_COMPUTE, clayout, 0, 1, &cdset, 0, 0);
                vkCmdSetDeviceMask(ccmd, 0x1);                          /* V3: Core-1.1 No-op (1 Geraet) */
                vkCmdDispatchBase(ccmd, 0, 0, 0, NELEM, 1, 1);          /* V3: Core-1.1 DispatchBase (Basis 0) */
                vkEndCommandBuffer(ccmd);
                vkResetFences(dev, 1, &cfence);
                vkQueueSubmit(q, 1, &csi, cfence);
                vkWaitForFences(dev, 1, &cfence, VK_TRUE, ~0ull);
                void *am2 = 0; int got = 0;
                if (vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &am2) == VK_SUCCESS) {
                    got = (((unsigned *)am2)[0] == 36u);   /* 1+2+...+8 */
                    vkUnmapMemory(dev, smem);
                }
                ok_atomic = got;
                vkDestroyPipeline(dev, acpipe, 0);
                vkDestroyShaderModule(dev, acs, 0);
            }
        }
        all = all && ok_atomic;
        uwrite("[vktest] vkdraw V2.6: atomics (atomicAdd(data[0], gid+1) x8 -> 36)=");
        uwrite(ok_atomic ? "ok" : "FEHLER");
        uwrite("\n");

        /* V3b SUBGROUP: subgroupAdd(x+5)+gl_SubgroupSize*1000+gl_SubgroupInvocationID+
         * (subgroupElect()?1000000:0). Interpreter faehrt 1 Lane -> data[x]==x+1001005.
         * Reuse SSBO/Descriptor/Layout/CmdBuf; SSBO mit 0xFFFFFFFF vorbelegt (Nicht-Schreiben faellt auf). */
        int ok_sg = ok_comp;
        {
            void *sm = 0;
            if (ok_sg && vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &sm) == VK_SUCCESS) {
                for (unsigned i = 0; i < NELEM; i++) { ((unsigned *)sm)[i] = 0xFFFFFFFFu; }
                vkUnmapMemory(dev, smem);
            } else { ok_sg = 0; }
            VkShaderModule sgcs = VK_NULL_HANDLE;
            smi.codeSize = sizeof(spv_compsubgroup_words); smi.pCode = spv_compsubgroup_words;
            ok_sg = ok_sg && (vkCreateShaderModule(dev, &smi, 0, &sgcs) == VK_SUCCESS);
            VkComputePipelineCreateInfo sgpci; memset(&sgpci, 0, sizeof(sgpci));
            sgpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            sgpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            sgpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            sgpci.stage.module = sgcs; sgpci.stage.pName = "main";
            sgpci.layout = clayout;
            VkPipeline sgpipe = VK_NULL_HANDLE;
            ok_sg = ok_sg && (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &sgpci, 0, &sgpipe) == VK_SUCCESS);
            if (ok_sg) {
                vkBeginCommandBuffer(ccmd, &ccbb);
                vkCmdBindPipeline(ccmd, VK_PIPELINE_BIND_POINT_COMPUTE, sgpipe);
                vkCmdBindDescriptorSets(ccmd, VK_PIPELINE_BIND_POINT_COMPUTE, clayout, 0, 1, &cdset, 0, 0);
                vkCmdDispatch(ccmd, NELEM, 1, 1);
                vkEndCommandBuffer(ccmd);
                vkResetFences(dev, 1, &cfence);
                vkQueueSubmit(q, 1, &csi, cfence);
                vkWaitForFences(dev, 1, &cfence, VK_TRUE, ~0ull);
                void *sm2 = 0; int got = 0;
                if (vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &sm2) == VK_SUCCESS) {
                    got = 1;
                    for (unsigned i = 0; i < NELEM; i++) {
                        if (((unsigned *)sm2)[i] != i + 1001005u) { got = 0; }
                    }
                    vkUnmapMemory(dev, smem);
                }
                ok_sg = got;
                vkDestroyPipeline(dev, sgpipe, 0);
                vkDestroyShaderModule(dev, sgcs, 0);
            }
        }
        all = all && ok_sg;
        uwrite("[vktest] vk V3b: subgroup-ops (subgroupAdd+SubgroupSize+Elect, 1-Lane -> data[x]==x+1001005)=");
        uwrite(ok_sg ? "ok" : "FEHLER");
        uwrite("\n");

        vkDestroyPipeline(dev, cpipe, 0);
        vkDestroyPipelineLayout(dev, clayout, 0);
        vkDestroyShaderModule(dev, cs, 0);
        vkDestroyDescriptorPool(dev, cdpool, 0);
        vkDestroyDescriptorSetLayout(dev, cdsl, 0);
        vkDestroyFence(dev, cfence, 0);
        vkFreeCommandBuffers(dev, cpool, 1, &ccmd);
        vkDestroyBuffer(dev, sbuf, 0);
        vkFreeMemory(dev, smem, 0);
    }
    all = all && ok_comp && ok_compdata;
    uwrite("[vktest] vkdraw V1.7: compute=");
    uwrite((ok_comp && ok_compdata) ? "ok" : "FEHLER");
    uwrite(" (dispatch(8): SSBO data[i]==i*3 -- gl_GlobalInvocationID+Array+Store)\n");

    /* --- V2.6 STORAGE-IMAGE: 4x1 rgba8 image; Compute vertauscht R<->B je Pixel (imageLoad+imageStore).
     * Pre-fill rot/gruen/blau/gelb -> nach Swap blau/gruen/rot/cyan. Beweist OpImageRead+Write+Koord. */
    int ok_stimg = 1;
    {
        VkImageCreateInfo sic; memset(&sic, 0, sizeof(sic));
        sic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        sic.imageType = VK_IMAGE_TYPE_2D; sic.format = VK_FORMAT_B8G8R8A8_UNORM;
        sic.extent.width = 4; sic.extent.height = 1; sic.extent.depth = 1;
        sic.mipLevels = 1; sic.arrayLayers = 1; sic.samples = VK_SAMPLE_COUNT_1_BIT;
        sic.tiling = VK_IMAGE_TILING_LINEAR;
        sic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage siimg = VK_NULL_HANDLE;
        ok_stimg = (vkCreateImage(dev, &sic, 0, &siimg) == VK_SUCCESS);
        VkMemoryRequirements simr; vkGetImageMemoryRequirements(dev, siimg, &simr);
        mai.allocationSize = simr.size;
        VkDeviceMemory simem = VK_NULL_HANDLE;
        ok_stimg = ok_stimg && (vkAllocateMemory(dev, &mai, 0, &simem) == VK_SUCCESS) &&
                   (vkBindImageMemory(dev, siimg, simem, 0) == VK_SUCCESS);
        void *simap = 0;
        if (ok_stimg && vkMapMemory(dev, simem, 0, VK_WHOLE_SIZE, 0, &simap) == VK_SUCCESS) {
            unsigned *sp = (unsigned *)simap;
            sp[0] = 0xFFFF0000u; sp[1] = 0xFF00FF00u; sp[2] = 0xFF0000FFu; sp[3] = 0xFFFFFF00u;  /* rot,gruen,blau,gelb */
            vkUnmapMemory(dev, simem);
        } else { ok_stimg = 0; }
        VkImageViewCreateInfo sivci; memset(&sivci, 0, sizeof(sivci));
        sivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        sivci.image = siimg; sivci.viewType = VK_IMAGE_VIEW_TYPE_2D; sivci.format = VK_FORMAT_B8G8R8A8_UNORM;
        sivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sivci.subresourceRange.levelCount = 1; sivci.subresourceRange.layerCount = 1;
        VkImageView siview = VK_NULL_HANDLE;
        ok_stimg = ok_stimg && (vkCreateImageView(dev, &sivci, 0, &siview) == VK_SUCCESS);
        VkDescriptorSetLayoutBinding silb; memset(&silb, 0, sizeof(silb));
        silb.binding = 0; silb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        silb.descriptorCount = 1; silb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo sidlci; memset(&sidlci, 0, sizeof(sidlci));
        sidlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sidlci.bindingCount = 1; sidlci.pBindings = &silb;
        VkDescriptorSetLayout sidsl = VK_NULL_HANDLE;
        ok_stimg = ok_stimg && (vkCreateDescriptorSetLayout(dev, &sidlci, 0, &sidsl) == VK_SUCCESS);
        VkDescriptorPoolSize sidps; memset(&sidps, 0, sizeof(sidps));
        sidps.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; sidps.descriptorCount = 1;
        VkDescriptorPoolCreateInfo sidpci; memset(&sidpci, 0, sizeof(sidpci));
        sidpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        sidpci.maxSets = 1; sidpci.poolSizeCount = 1; sidpci.pPoolSizes = &sidps;
        VkDescriptorPool sidpool = VK_NULL_HANDLE;
        ok_stimg = ok_stimg && (vkCreateDescriptorPool(dev, &sidpci, 0, &sidpool) == VK_SUCCESS);
        VkDescriptorSetAllocateInfo sidsai; memset(&sidsai, 0, sizeof(sidsai));
        sidsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sidsai.descriptorPool = sidpool; sidsai.descriptorSetCount = 1; sidsai.pSetLayouts = &sidsl;
        VkDescriptorSet sidset = VK_NULL_HANDLE;
        ok_stimg = ok_stimg && (vkAllocateDescriptorSets(dev, &sidsai, &sidset) == VK_SUCCESS);
        VkDescriptorImageInfo siii; memset(&siii, 0, sizeof(siii));
        siii.imageView = siview; siii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet siwds; memset(&siwds, 0, sizeof(siwds));
        siwds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        siwds.dstSet = sidset; siwds.dstBinding = 0; siwds.descriptorCount = 1;
        siwds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; siwds.pImageInfo = &siii;
        if (ok_stimg) { vkUpdateDescriptorSets(dev, 1, &siwds, 0, 0); }
        VkShaderModule sics = VK_NULL_HANDLE;
        smi.codeSize = sizeof(spv_compimage_words); smi.pCode = spv_compimage_words;
        ok_stimg = ok_stimg && (vkCreateShaderModule(dev, &smi, 0, &sics) == VK_SUCCESS);
        VkPipelineLayoutCreateInfo sipli; memset(&sipli, 0, sizeof(sipli));
        sipli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        sipli.setLayoutCount = 1; sipli.pSetLayouts = &sidsl;
        VkPipelineLayout silayout = VK_NULL_HANDLE;
        ok_stimg = ok_stimg && (vkCreatePipelineLayout(dev, &sipli, 0, &silayout) == VK_SUCCESS);
        VkComputePipelineCreateInfo sicpci; memset(&sicpci, 0, sizeof(sicpci));
        sicpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        sicpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sicpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; sicpci.stage.module = sics; sicpci.stage.pName = "main";
        sicpci.layout = silayout;
        VkPipeline sipipe = VK_NULL_HANDLE;
        ok_stimg = ok_stimg && (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &sicpci, 0, &sipipe) == VK_SUCCESS);
        VkCommandBufferAllocateInfo sicbi; memset(&sicbi, 0, sizeof(sicbi));
        sicbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        sicbi.commandPool = cpool; sicbi.commandBufferCount = 1;
        VkCommandBuffer sicmd = VK_NULL_HANDLE;
        ok_stimg = ok_stimg && (vkAllocateCommandBuffers(dev, &sicbi, &sicmd) == VK_SUCCESS);
        if (ok_stimg) {
            VkCommandBufferBeginInfo sibb; memset(&sibb, 0, sizeof(sibb));
            sibb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(sicmd, &sibb);
            vkCmdBindPipeline(sicmd, VK_PIPELINE_BIND_POINT_COMPUTE, sipipe);
            vkCmdBindDescriptorSets(sicmd, VK_PIPELINE_BIND_POINT_COMPUTE, silayout, 0, 1, &sidset, 0, 0);
            vkCmdDispatch(sicmd, 4, 1, 1);
            vkEndCommandBuffer(sicmd);
            VkFence sifence = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &sifence);
            VkSubmitInfo sisi; memset(&sisi, 0, sizeof(sisi));
            sisi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; sisi.commandBufferCount = 1; sisi.pCommandBuffers = &sicmd;
            vkQueueSubmit(q, 1, &sisi, sifence);
            vkWaitForFences(dev, 1, &sifence, VK_TRUE, ~0ull);
            void *sm2 = 0; int got = 0;
            if (vkMapMemory(dev, simem, 0, VK_WHOLE_SIZE, 0, &sm2) == VK_SUCCESS) {
                const unsigned *sp = (const unsigned *)sm2;
                got = (sp[0] == 0xFF0000FFu && sp[1] == 0xFF00FF00u &&
                       sp[2] == 0xFFFF0000u && sp[3] == 0xFF00FFFFu);   /* blau,gruen,rot,cyan */
                vkUnmapMemory(dev, simem);
            }
            ok_stimg = got;
            vkDestroyFence(dev, sifence, 0);
            vkDestroyPipeline(dev, sipipe, 0); vkDestroyShaderModule(dev, sics, 0);
            vkDestroyPipelineLayout(dev, silayout, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &sicmd);
        }
        vkDestroyDescriptorPool(dev, sidpool, 0); vkDestroyDescriptorSetLayout(dev, sidsl, 0);
        vkDestroyImageView(dev, siview, 0); vkDestroyImage(dev, siimg, 0); vkFreeMemory(dev, simem, 0);
    }
    all = all && ok_stimg;
    uwrite("[vktest] vkdraw V2.6: storage-image (imageLoad/Store R<->B swap 4px -> blau/gruen/rot/cyan)=");
    uwrite(ok_stimg ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 vkQueueSubmit2 -- fuehrt einen Command-Buffer (FillBuffer) via Submit2 AUS
     * (nicht nur Fence signalisieren): Buffer mit Sentinel -> nach Submit2 == 0xCAFEBABE.
     * Uebt zugleich vkCmdPipelineBarrier2 (No-op) im CB. */
    int ok_submit2 = 1;
    {
        VkBufferCreateInfo s2bci; memset(&s2bci, 0, sizeof(s2bci));
        s2bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        s2bci.size = 16; s2bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer s2buf = VK_NULL_HANDLE;
        ok_submit2 = (vkCreateBuffer(dev, &s2bci, 0, &s2buf) == VK_SUCCESS);
        VkMemoryRequirements s2mr; vkGetBufferMemoryRequirements(dev, s2buf, &s2mr);
        VkMemoryAllocateInfo s2mai; memset(&s2mai, 0, sizeof(s2mai));
        s2mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; s2mai.allocationSize = s2mr.size;
        VkDeviceMemory s2mem = VK_NULL_HANDLE;
        ok_submit2 = ok_submit2 && (vkAllocateMemory(dev, &s2mai, 0, &s2mem) == VK_SUCCESS) &&
                     (vkBindBufferMemory(dev, s2buf, s2mem, 0) == VK_SUCCESS);
        void *s2map = 0;
        if (ok_submit2 && vkMapMemory(dev, s2mem, 0, VK_WHOLE_SIZE, 0, &s2map) == VK_SUCCESS) {
            ((unsigned *)s2map)[0] = 0xFFFFFFFFu; vkUnmapMemory(dev, s2mem);
        } else { ok_submit2 = 0; }
        VkCommandBufferAllocateInfo s2cbi; memset(&s2cbi, 0, sizeof(s2cbi));
        s2cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        s2cbi.commandPool = cpool; s2cbi.commandBufferCount = 1;
        VkCommandBuffer s2cmd = VK_NULL_HANDLE;
        ok_submit2 = ok_submit2 && (vkAllocateCommandBuffers(dev, &s2cbi, &s2cmd) == VK_SUCCESS);
        if (ok_submit2) {
            VkCommandBufferBeginInfo s2bb; memset(&s2bb, 0, sizeof(s2bb));
            s2bb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(s2cmd, &s2bb);
            vkCmdFillBuffer(s2cmd, s2buf, 0, 16, 0xCAFEBABEu);   /* explizite Groesse (Fill nutzt (int)size) */
            VkDependencyInfo s2dep; memset(&s2dep, 0, sizeof(s2dep));
            s2dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            vkCmdPipelineBarrier2(s2cmd, &s2dep);          /* V3 sync2 No-op, muss harmlos sein */
            vkEndCommandBuffer(s2cmd);
            VkCommandBufferSubmitInfo cbsi; memset(&cbsi, 0, sizeof(cbsi));
            cbsi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO; cbsi.commandBuffer = s2cmd;
            VkSubmitInfo2 si2; memset(&si2, 0, sizeof(si2));
            si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
            si2.commandBufferInfoCount = 1; si2.pCommandBufferInfos = &cbsi;
            VkFence s2fence = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &s2fence);
            VkResult sr = vkQueueSubmit2(q, 1, &si2, s2fence);
            vkWaitForFences(dev, 1, &s2fence, VK_TRUE, ~0ull);
            void *s2m2 = 0; int got = 0;
            if (vkMapMemory(dev, s2mem, 0, VK_WHOLE_SIZE, 0, &s2m2) == VK_SUCCESS) {
                got = (((unsigned *)s2m2)[0] == 0xCAFEBABEu);
                vkUnmapMemory(dev, s2mem);
            }
            ok_submit2 = (sr == VK_SUCCESS) && got &&
                         (vkGetFenceStatus(dev, s2fence) == VK_SUCCESS);
            vkDestroyFence(dev, s2fence, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &s2cmd);
        }
        vkDestroyBuffer(dev, s2buf, 0); vkFreeMemory(dev, s2mem, 0);
    }
    all = all && ok_submit2;
    uwrite("[vktest] vk V3: sync2 vkQueueSubmit2 (FillBuffer via Submit2 -> 0xCAFEBABE)=");
    uwrite(ok_submit2 ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 vkCmdCopyBuffer2 -- src(0x1234ABCD) via Copy2 nach dst -> dst == 0x1234ABCD. */
    int ok_copy2 = 1;
    {
        VkBufferCreateInfo cbci; memset(&cbci, 0, sizeof(cbci));
        cbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; cbci.size = 16;
        cbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer csrc = VK_NULL_HANDLE, cdst = VK_NULL_HANDLE;
        ok_copy2 = (vkCreateBuffer(dev, &cbci, 0, &csrc) == VK_SUCCESS) &&
                   (vkCreateBuffer(dev, &cbci, 0, &cdst) == VK_SUCCESS);
        VkMemoryRequirements cmrq; vkGetBufferMemoryRequirements(dev, csrc, &cmrq);
        VkMemoryAllocateInfo cmai; memset(&cmai, 0, sizeof(cmai));
        cmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; cmai.allocationSize = cmrq.size;
        VkDeviceMemory csmem = VK_NULL_HANDLE, cdmem = VK_NULL_HANDLE;
        ok_copy2 = ok_copy2 && (vkAllocateMemory(dev, &cmai, 0, &csmem) == VK_SUCCESS) &&
                   (vkAllocateMemory(dev, &cmai, 0, &cdmem) == VK_SUCCESS) &&
                   (vkBindBufferMemory(dev, csrc, csmem, 0) == VK_SUCCESS) &&
                   (vkBindBufferMemory(dev, cdst, cdmem, 0) == VK_SUCCESS);
        void *csm = 0, *cdm = 0;
        if (ok_copy2 && vkMapMemory(dev, csmem, 0, VK_WHOLE_SIZE, 0, &csm) == VK_SUCCESS) {
            ((unsigned *)csm)[0] = 0x1234ABCDu; vkUnmapMemory(dev, csmem);
        } else { ok_copy2 = 0; }
        if (ok_copy2 && vkMapMemory(dev, cdmem, 0, VK_WHOLE_SIZE, 0, &cdm) == VK_SUCCESS) {
            ((unsigned *)cdm)[0] = 0u; vkUnmapMemory(dev, cdmem);
        } else { ok_copy2 = 0; }
        VkCommandBufferAllocateInfo ccbi2; memset(&ccbi2, 0, sizeof(ccbi2));
        ccbi2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ccbi2.commandPool = cpool; ccbi2.commandBufferCount = 1;
        VkCommandBuffer ccmd2 = VK_NULL_HANDLE;
        ok_copy2 = ok_copy2 && (vkAllocateCommandBuffers(dev, &ccbi2, &ccmd2) == VK_SUCCESS);
        if (ok_copy2) {
            VkCommandBufferBeginInfo cbb2; memset(&cbb2, 0, sizeof(cbb2));
            cbb2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(ccmd2, &cbb2);
            VkBufferCopy2 reg; memset(&reg, 0, sizeof(reg));
            reg.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2; reg.srcOffset = 0; reg.dstOffset = 0; reg.size = 16;
            VkCopyBufferInfo2 ci2; memset(&ci2, 0, sizeof(ci2));
            ci2.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
            ci2.srcBuffer = csrc; ci2.dstBuffer = cdst; ci2.regionCount = 1; ci2.pRegions = &reg;
            vkCmdCopyBuffer2(ccmd2, &ci2);
            vkEndCommandBuffer(ccmd2);
            VkFence cf2 = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &cf2);
            VkSubmitInfo csi2; memset(&csi2, 0, sizeof(csi2));
            csi2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; csi2.commandBufferCount = 1; csi2.pCommandBuffers = &ccmd2;
            vkQueueSubmit(q, 1, &csi2, cf2);
            vkWaitForFences(dev, 1, &cf2, VK_TRUE, ~0ull);
            void *cdm2 = 0; int got = 0;
            if (vkMapMemory(dev, cdmem, 0, VK_WHOLE_SIZE, 0, &cdm2) == VK_SUCCESS) {
                got = (((unsigned *)cdm2)[0] == 0x1234ABCDu);
                vkUnmapMemory(dev, cdmem);
            }
            ok_copy2 = got;
            vkDestroyFence(dev, cf2, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &ccmd2);
        }
        vkDestroyBuffer(dev, csrc, 0); vkDestroyBuffer(dev, cdst, 0);
        vkFreeMemory(dev, csmem, 0); vkFreeMemory(dev, cdmem, 0);
    }
    all = all && ok_copy2;
    uwrite("[vktest] vk V3: copy2 vkCmdCopyBuffer2 (src->dst via Copy2 -> 0x1234ABCD)=");
    uwrite(ok_copy2 ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.2 RenderPass2 -- rp2 (1 Farb-Attachment, loadOp CLEAR) + FB + BeginRenderPass2/
     * EndRenderPass2 clear-only (32x32) -> nach Submit Clear-Farbe 0xFF00FF00 (gruen) im Bild. */
    int ok_rp2 = 1;
    {
        VkAttachmentDescription2 ad2; memset(&ad2, 0, sizeof(ad2));
        ad2.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        ad2.format = VK_FORMAT_B8G8R8A8_UNORM; ad2.samples = VK_SAMPLE_COUNT_1_BIT;
        ad2.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ad2.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad2.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad2.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ad2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; ad2.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference2 ar2; memset(&ar2, 0, sizeof(ar2));
        ar2.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2; ar2.attachment = 0;
        ar2.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription2 sd2; memset(&sd2, 0, sizeof(sd2));
        sd2.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2; sd2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sd2.colorAttachmentCount = 1; sd2.pColorAttachments = &ar2;
        VkRenderPassCreateInfo2 rpci2; memset(&rpci2, 0, sizeof(rpci2));
        rpci2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
        rpci2.attachmentCount = 1; rpci2.pAttachments = &ad2;
        rpci2.subpassCount = 1; rpci2.pSubpasses = &sd2;
        VkRenderPass rp2 = VK_NULL_HANDLE;
        ok_rp2 = (vkCreateRenderPass2(dev, &rpci2, 0, &rp2) == VK_SUCCESS);
        VkImageCreateInfo r2ic; memset(&r2ic, 0, sizeof(r2ic));
        r2ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; r2ic.imageType = VK_IMAGE_TYPE_2D;
        r2ic.format = VK_FORMAT_B8G8R8A8_UNORM; r2ic.extent.width = 32; r2ic.extent.height = 32; r2ic.extent.depth = 1;
        r2ic.mipLevels = 1; r2ic.arrayLayers = 1; r2ic.samples = VK_SAMPLE_COUNT_1_BIT;
        r2ic.tiling = VK_IMAGE_TILING_LINEAR;
        r2ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage r2img = VK_NULL_HANDLE;
        ok_rp2 = ok_rp2 && (vkCreateImage(dev, &r2ic, 0, &r2img) == VK_SUCCESS);
        VkMemoryRequirements r2mr; vkGetImageMemoryRequirements(dev, r2img, &r2mr);
        VkMemoryAllocateInfo r2mai; memset(&r2mai, 0, sizeof(r2mai));
        r2mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; r2mai.allocationSize = r2mr.size;
        VkDeviceMemory r2mem = VK_NULL_HANDLE;
        ok_rp2 = ok_rp2 && (vkAllocateMemory(dev, &r2mai, 0, &r2mem) == VK_SUCCESS) &&
                 (vkBindImageMemory(dev, r2img, r2mem, 0) == VK_SUCCESS);
        VkImageViewCreateInfo r2vci; memset(&r2vci, 0, sizeof(r2vci));
        r2vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; r2vci.image = r2img;
        r2vci.viewType = VK_IMAGE_VIEW_TYPE_2D; r2vci.format = VK_FORMAT_B8G8R8A8_UNORM;
        r2vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r2vci.subresourceRange.levelCount = 1; r2vci.subresourceRange.layerCount = 1;
        VkImageView r2view = VK_NULL_HANDLE;
        ok_rp2 = ok_rp2 && (vkCreateImageView(dev, &r2vci, 0, &r2view) == VK_SUCCESS);
        VkFramebufferCreateInfo r2fci; memset(&r2fci, 0, sizeof(r2fci));
        r2fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; r2fci.renderPass = rp2;
        r2fci.attachmentCount = 1; r2fci.pAttachments = &r2view; r2fci.width = 32; r2fci.height = 32; r2fci.layers = 1;
        VkFramebuffer r2fb = VK_NULL_HANDLE;
        ok_rp2 = ok_rp2 && (vkCreateFramebuffer(dev, &r2fci, 0, &r2fb) == VK_SUCCESS);
        VkCommandBufferAllocateInfo r2cbi; memset(&r2cbi, 0, sizeof(r2cbi));
        r2cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; r2cbi.commandPool = cpool; r2cbi.commandBufferCount = 1;
        VkCommandBuffer r2cmd = VK_NULL_HANDLE;
        ok_rp2 = ok_rp2 && (vkAllocateCommandBuffers(dev, &r2cbi, &r2cmd) == VK_SUCCESS);
        if (ok_rp2) {
            VkCommandBufferBeginInfo r2bb; memset(&r2bb, 0, sizeof(r2bb));
            r2bb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(r2cmd, &r2bb);
            VkClearValue cv; memset(&cv, 0, sizeof(cv));
            cv.color.float32[0] = 0.0f; cv.color.float32[1] = 1.0f; cv.color.float32[2] = 0.0f; cv.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo r2rbi; memset(&r2rbi, 0, sizeof(r2rbi));
            r2rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; r2rbi.renderPass = rp2; r2rbi.framebuffer = r2fb;
            r2rbi.renderArea.extent.width = 32; r2rbi.renderArea.extent.height = 32;
            r2rbi.clearValueCount = 1; r2rbi.pClearValues = &cv;
            VkSubpassBeginInfo sbi; memset(&sbi, 0, sizeof(sbi));
            sbi.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO; sbi.contents = VK_SUBPASS_CONTENTS_INLINE;
            vkCmdBeginRenderPass2(r2cmd, &r2rbi, &sbi);
            VkSubpassEndInfo sei; memset(&sei, 0, sizeof(sei)); sei.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;
            vkCmdEndRenderPass2(r2cmd, &sei);
            vkEndCommandBuffer(r2cmd);
            VkFence r2f = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &r2f);
            VkSubmitInfo r2si; memset(&r2si, 0, sizeof(r2si));
            r2si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; r2si.commandBufferCount = 1; r2si.pCommandBuffers = &r2cmd;
            vkQueueSubmit(q, 1, &r2si, r2f);
            vkWaitForFences(dev, 1, &r2f, VK_TRUE, ~0ull);
            void *r2m = 0; int got = 0;
            if (vkMapMemory(dev, r2mem, 0, VK_WHOLE_SIZE, 0, &r2m) == VK_SUCCESS) {
                got = (((unsigned *)r2m)[16 * 32 + 16] == 0xFF00FF00u);   /* Clear-Gruen */
                vkUnmapMemory(dev, r2mem);
            }
            ok_rp2 = got;
            vkDestroyFence(dev, r2f, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &r2cmd);
        }
        vkDestroyFramebuffer(dev, r2fb, 0); vkDestroyImageView(dev, r2view, 0);
        vkDestroyImage(dev, r2img, 0); vkFreeMemory(dev, r2mem, 0);
        vkDestroyRenderPass(dev, rp2, 0);
    }
    all = all && ok_rp2;
    uwrite("[vktest] vk V3: renderpass2 (CreateRenderPass2 + Begin/EndRenderPass2 clear -> 0xFF00FF00)=");
    uwrite(ok_rp2 ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.2 Timeline-Semaphore -- init=5, signal 42, GetCounterValue, Wait 42 ok / 100 timeout. */
    int ok_tl = 1;
    {
        VkSemaphoreTypeCreateInfo stci; memset(&stci, 0, sizeof(stci));
        stci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; stci.initialValue = 5;
        VkSemaphoreCreateInfo tsci; memset(&tsci, 0, sizeof(tsci));
        tsci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; tsci.pNext = &stci;
        VkSemaphore tsem = VK_NULL_HANDLE;
        ok_tl = (vkCreateSemaphore(dev, &tsci, 0, &tsem) == VK_SUCCESS);
        uint64_t v0 = 0;
        ok_tl = ok_tl && (vkGetSemaphoreCounterValue(dev, tsem, &v0) == VK_SUCCESS) && (v0 == 5);
        VkSemaphoreSignalInfo ssi; memset(&ssi, 0, sizeof(ssi));
        ssi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO; ssi.semaphore = tsem; ssi.value = 42;
        ok_tl = ok_tl && (vkSignalSemaphore(dev, &ssi) == VK_SUCCESS);
        uint64_t v1 = 0;
        ok_tl = ok_tl && (vkGetSemaphoreCounterValue(dev, tsem, &v1) == VK_SUCCESS) && (v1 == 42);
        uint64_t wv_ok = 42, wv_hi = 100;
        VkSemaphoreWaitInfo swi; memset(&swi, 0, sizeof(swi));
        swi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        swi.semaphoreCount = 1; swi.pSemaphores = &tsem; swi.pValues = &wv_ok;
        int ok_wait_ok = (vkWaitSemaphores(dev, &swi, 0) == VK_SUCCESS);
        swi.pValues = &wv_hi;
        int ok_wait_to = (vkWaitSemaphores(dev, &swi, 0) == VK_TIMEOUT);
        ok_tl = ok_tl && ok_wait_ok && ok_wait_to;
        vkDestroySemaphore(dev, tsem, 0);
    }
    all = all && ok_tl;
    uwrite("[vktest] vk V3: timeline-semaphore (init=5 -> signal 42 -> wait 42 ok / 100 timeout)=");
    uwrite(ok_tl ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.2 vkGetBufferDeviceAddress -- Adresse != 0, stabil, == gemappter Zeiger (CPU-Impl). */
    int ok_bda = 1;
    {
        VkBufferCreateInfo adbci; memset(&adbci, 0, sizeof(adbci));
        adbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; adbci.size = 64;
        adbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkBuffer adbuf = VK_NULL_HANDLE;
        ok_bda = (vkCreateBuffer(dev, &adbci, 0, &adbuf) == VK_SUCCESS);
        VkMemoryRequirements admr; vkGetBufferMemoryRequirements(dev, adbuf, &admr);
        VkMemoryAllocateInfo admai; memset(&admai, 0, sizeof(admai));
        admai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; admai.allocationSize = admr.size;
        VkDeviceMemory admem = VK_NULL_HANDLE;
        ok_bda = ok_bda && (vkAllocateMemory(dev, &admai, 0, &admem) == VK_SUCCESS) &&
                 (vkBindBufferMemory(dev, adbuf, admem, 0) == VK_SUCCESS);
        VkBufferDeviceAddressInfo bdai; memset(&bdai, 0, sizeof(bdai));
        bdai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO; bdai.buffer = adbuf;
        VkDeviceAddress addr = vkGetBufferDeviceAddress(dev, &bdai);
        VkDeviceAddress addr2 = vkGetBufferDeviceAddress(dev, &bdai);
        void *mapped = 0;
        ok_bda = ok_bda && (vkMapMemory(dev, admem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS);
        ok_bda = ok_bda && (addr != 0) && (addr == addr2) &&
                 (addr == (VkDeviceAddress)(unsigned long)mapped);
        if (mapped) { vkUnmapMemory(dev, admem); }
        vkDestroyBuffer(dev, adbuf, 0); vkFreeMemory(dev, admem, 0);
    }
    all = all && ok_bda;
    uwrite("[vktest] vk V3: buffer-device-address (addr != 0, stabil, == mapped ptr)=");
    uwrite(ok_bda ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 Dynamic Rendering -- 32x32, BeginRendering(loadOp CLEAR gruen)+EndRendering
     * OHNE RenderPass/Framebuffer-Objekte -> nach Submit Clear-Farbe 0xFF00FF00 im Bild. */
    int ok_dynr = 1;
    {
        VkImageCreateInfo dic; memset(&dic, 0, sizeof(dic));
        dic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; dic.imageType = VK_IMAGE_TYPE_2D;
        dic.format = VK_FORMAT_B8G8R8A8_UNORM; dic.extent.width = 64; dic.extent.height = 64; dic.extent.depth = 1;
        dic.mipLevels = 1; dic.arrayLayers = 1; dic.samples = VK_SAMPLE_COUNT_1_BIT;
        dic.tiling = VK_IMAGE_TILING_LINEAR; dic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage dimg = VK_NULL_HANDLE;
        ok_dynr = (vkCreateImage(dev, &dic, 0, &dimg) == VK_SUCCESS);
        VkMemoryRequirements dmr; vkGetImageMemoryRequirements(dev, dimg, &dmr);
        VkMemoryAllocateInfo dmai; memset(&dmai, 0, sizeof(dmai));
        dmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; dmai.allocationSize = dmr.size;
        VkDeviceMemory dmem = VK_NULL_HANDLE;
        ok_dynr = ok_dynr && (vkAllocateMemory(dev, &dmai, 0, &dmem) == VK_SUCCESS) &&
                  (vkBindImageMemory(dev, dimg, dmem, 0) == VK_SUCCESS);
        VkImageViewCreateInfo dvci; memset(&dvci, 0, sizeof(dvci));
        dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; dvci.image = dimg;
        dvci.viewType = VK_IMAGE_VIEW_TYPE_2D; dvci.format = VK_FORMAT_B8G8R8A8_UNORM;
        dvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        dvci.subresourceRange.levelCount = 1; dvci.subresourceRange.layerCount = 1;
        VkImageView dview = VK_NULL_HANDLE;
        ok_dynr = ok_dynr && (vkCreateImageView(dev, &dvci, 0, &dview) == VK_SUCCESS);
        VkCommandBufferAllocateInfo dcbi; memset(&dcbi, 0, sizeof(dcbi));
        dcbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; dcbi.commandPool = cpool; dcbi.commandBufferCount = 1;
        VkCommandBuffer dcmd = VK_NULL_HANDLE;
        ok_dynr = ok_dynr && (vkAllocateCommandBuffers(dev, &dcbi, &dcmd) == VK_SUCCESS);
        if (ok_dynr) {
            VkCommandBufferBeginInfo dbb; memset(&dbb, 0, sizeof(dbb));
            dbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(dcmd, &dbb);
            VkRenderingAttachmentInfo cat; memset(&cat, 0, sizeof(cat));
            cat.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; cat.imageView = dview;
            cat.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            cat.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; cat.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            cat.clearValue.color.float32[0] = 0.1f; cat.clearValue.color.float32[1] = 0.2f;  /* blau 0xFF1A33CC */
            cat.clearValue.color.float32[2] = 0.8f; cat.clearValue.color.float32[3] = 1.0f;
            VkRenderingInfo dri; memset(&dri, 0, sizeof(dri));
            dri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            dri.renderArea.extent.width = 64; dri.renderArea.extent.height = 64; dri.layerCount = 1;
            dri.colorAttachmentCount = 1; dri.pColorAttachments = &cat;
            /* V3b: ECHTER Draw in Dynamic-Rendering -- Clear blau, dann gruenes Dreieck ueber pipe/vbuf. */
            vkCmdBeginRendering(dcmd, &dri);
            vkCmdBindPipeline(dcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindVertexBuffers(dcmd, 0, 1, &vbuf, &zero_off);
            vkCmdPushConstants(dcmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
            vkCmdDraw(dcmd, 3, 1, 0, 0);
            vkCmdEndRendering(dcmd);
            vkEndCommandBuffer(dcmd);
            VkFence df = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &df);
            VkSubmitInfo dsi; memset(&dsi, 0, sizeof(dsi));
            dsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; dsi.commandBufferCount = 1; dsi.pCommandBuffers = &dcmd;
            vkQueueSubmit(q, 1, &dsi, df);
            vkWaitForFences(dev, 1, &df, VK_TRUE, ~0ull);
            void *dm = 0; int got = 0;
            if (vkMapMemory(dev, dmem, 0, VK_WHOLE_SIZE, 0, &dm) == VK_SUCCESS) {
                const unsigned *dw = (const unsigned *)dm;
                int center_green = (dw[32 * 64 + 32] == 0xFF00FF00u);   /* Draw traf die Mitte */
                int corner_clear = (dw[2 * 64 + 2] == 0xFF1A33CCu);     /* Clear blieb in der Ecke */
                got = center_green && corner_clear;
                vkUnmapMemory(dev, dmem);
            }
            ok_dynr = got;
            vkDestroyFence(dev, df, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &dcmd);
        }
        vkDestroyImageView(dev, dview, 0); vkDestroyImage(dev, dimg, 0); vkFreeMemory(dev, dmem, 0);
    }
    all = all && ok_dynr;
    uwrite("[vktest] vk V3b: dynamic-rendering DRAW (Clear blau + gruenes Dreieck ohne RP/FB: Mitte gruen, Ecke Clear)=");
    uwrite(ok_dynr ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 Sync2-Events -- vkCmdSetEvent2 im CB signalisiert device-seitig, ResetEvent2 löscht. */
    int ok_ev2 = 1;
    {
        VkEventCreateInfo eci; memset(&eci, 0, sizeof(eci));
        eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        VkEvent ev = VK_NULL_HANDLE;
        ok_ev2 = (vkCreateEvent(dev, &eci, 0, &ev) == VK_SUCCESS);
        VkCommandBufferAllocateInfo ecbi; memset(&ecbi, 0, sizeof(ecbi));
        ecbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ecbi.commandPool = cpool; ecbi.commandBufferCount = 1;
        VkCommandBuffer ecmd = VK_NULL_HANDLE;
        ok_ev2 = ok_ev2 && (vkAllocateCommandBuffers(dev, &ecbi, &ecmd) == VK_SUCCESS);
        if (ok_ev2) {
            VkDependencyInfo edep; memset(&edep, 0, sizeof(edep)); edep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            VkCommandBufferBeginInfo ebb; memset(&ebb, 0, sizeof(ebb)); ebb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(ecmd, &ebb);
            vkCmdSetEvent2(ecmd, ev, &edep);
            vkEndCommandBuffer(ecmd);
            VkFence ef = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &ef);
            VkSubmitInfo esi; memset(&esi, 0, sizeof(esi));
            esi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; esi.commandBufferCount = 1; esi.pCommandBuffers = &ecmd;
            vkQueueSubmit(q, 1, &esi, ef);
            vkWaitForFences(dev, 1, &ef, VK_TRUE, ~0ull);
            int set_ok = (vkGetEventStatus(dev, ev) == VK_EVENT_SET);
            vkBeginCommandBuffer(ecmd, &ebb);
            vkCmdResetEvent2(ecmd, ev, 0);
            vkEndCommandBuffer(ecmd);
            vkResetFences(dev, 1, &ef);
            vkQueueSubmit(q, 1, &esi, ef);
            vkWaitForFences(dev, 1, &ef, VK_TRUE, ~0ull);
            int reset_ok = (vkGetEventStatus(dev, ev) == VK_EVENT_RESET);
            ok_ev2 = set_ok && reset_ok;
            vkDestroyFence(dev, ef, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &ecmd);
        }
        vkDestroyEvent(dev, ev, 0);
    }
    all = all && ok_ev2;
    uwrite("[vktest] vk V3: sync2-events (CmdSetEvent2 -> SET, CmdResetEvent2 -> RESET)=");
    uwrite(ok_ev2 ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 vkCmdCopyBufferToImage2 -- 2x2 Pixel (rot/gruen/blau/gelb) aus Buffer ins Bild. */
    int ok_b2i = 1;
    {
        VkBufferCreateInfo b2ci; memset(&b2ci, 0, sizeof(b2ci));
        b2ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; b2ci.size = 16;
        b2ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer b2buf = VK_NULL_HANDLE;
        ok_b2i = (vkCreateBuffer(dev, &b2ci, 0, &b2buf) == VK_SUCCESS);
        VkMemoryRequirements b2mr; vkGetBufferMemoryRequirements(dev, b2buf, &b2mr);
        VkMemoryAllocateInfo b2mai; memset(&b2mai, 0, sizeof(b2mai));
        b2mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; b2mai.allocationSize = b2mr.size;
        VkDeviceMemory b2mem = VK_NULL_HANDLE;
        ok_b2i = ok_b2i && (vkAllocateMemory(dev, &b2mai, 0, &b2mem) == VK_SUCCESS) &&
                 (vkBindBufferMemory(dev, b2buf, b2mem, 0) == VK_SUCCESS);
        void *b2map = 0;
        if (ok_b2i && vkMapMemory(dev, b2mem, 0, VK_WHOLE_SIZE, 0, &b2map) == VK_SUCCESS) {
            unsigned *bp = (unsigned *)b2map;
            bp[0] = 0xFFFF0000u; bp[1] = 0xFF00FF00u; bp[2] = 0xFF0000FFu; bp[3] = 0xFFFFFF00u;
            vkUnmapMemory(dev, b2mem);
        } else { ok_b2i = 0; }
        VkImageCreateInfo b2ic; memset(&b2ic, 0, sizeof(b2ic));
        b2ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; b2ic.imageType = VK_IMAGE_TYPE_2D;
        b2ic.format = VK_FORMAT_B8G8R8A8_UNORM; b2ic.extent.width = 2; b2ic.extent.height = 2; b2ic.extent.depth = 1;
        b2ic.mipLevels = 1; b2ic.arrayLayers = 1; b2ic.samples = VK_SAMPLE_COUNT_1_BIT;
        b2ic.tiling = VK_IMAGE_TILING_LINEAR; b2ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage b2img = VK_NULL_HANDLE;
        ok_b2i = ok_b2i && (vkCreateImage(dev, &b2ic, 0, &b2img) == VK_SUCCESS);
        VkMemoryRequirements b2imr; vkGetImageMemoryRequirements(dev, b2img, &b2imr);
        VkMemoryAllocateInfo b2imai; memset(&b2imai, 0, sizeof(b2imai));
        b2imai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; b2imai.allocationSize = b2imr.size;
        VkDeviceMemory b2imem = VK_NULL_HANDLE;
        ok_b2i = ok_b2i && (vkAllocateMemory(dev, &b2imai, 0, &b2imem) == VK_SUCCESS) &&
                 (vkBindImageMemory(dev, b2img, b2imem, 0) == VK_SUCCESS);
        VkCommandBufferAllocateInfo b2cbi; memset(&b2cbi, 0, sizeof(b2cbi));
        b2cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; b2cbi.commandPool = cpool; b2cbi.commandBufferCount = 1;
        VkCommandBuffer b2cmd = VK_NULL_HANDLE;
        ok_b2i = ok_b2i && (vkAllocateCommandBuffers(dev, &b2cbi, &b2cmd) == VK_SUCCESS);
        if (ok_b2i) {
            VkCommandBufferBeginInfo b2bb; memset(&b2bb, 0, sizeof(b2bb)); b2bb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(b2cmd, &b2bb);
            VkBufferImageCopy2 reg; memset(&reg, 0, sizeof(reg));
            reg.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2; reg.bufferOffset = 0;
            reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; reg.imageSubresource.layerCount = 1;
            reg.imageExtent.width = 2; reg.imageExtent.height = 2; reg.imageExtent.depth = 1;
            VkCopyBufferToImageInfo2 ci; memset(&ci, 0, sizeof(ci));
            ci.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
            ci.srcBuffer = b2buf; ci.dstImage = b2img; ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ci.regionCount = 1; ci.pRegions = &reg;
            vkCmdCopyBufferToImage2(b2cmd, &ci);
            vkEndCommandBuffer(b2cmd);
            VkFence b2f = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &b2f);
            VkSubmitInfo b2si; memset(&b2si, 0, sizeof(b2si));
            b2si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; b2si.commandBufferCount = 1; b2si.pCommandBuffers = &b2cmd;
            vkQueueSubmit(q, 1, &b2si, b2f);
            vkWaitForFences(dev, 1, &b2f, VK_TRUE, ~0ull);
            void *b2im = 0; int got = 0;
            if (vkMapMemory(dev, b2imem, 0, VK_WHOLE_SIZE, 0, &b2im) == VK_SUCCESS) {
                const unsigned *ip = (const unsigned *)b2im;
                got = (ip[0] == 0xFFFF0000u && ip[1] == 0xFF00FF00u &&
                       ip[2] == 0xFF0000FFu && ip[3] == 0xFFFFFF00u);   /* row0 rot/gruen, row1 blau/gelb */
                vkUnmapMemory(dev, b2imem);
            }
            ok_b2i = got;
            vkDestroyFence(dev, b2f, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &b2cmd);
        }
        vkDestroyBuffer(dev, b2buf, 0); vkFreeMemory(dev, b2mem, 0);
        vkDestroyImage(dev, b2img, 0); vkFreeMemory(dev, b2imem, 0);
    }
    all = all && ok_b2i;
    uwrite("[vktest] vk V3: copy-buffer-to-image2 (2x2 buffer -> image: rot/gruen/blau/gelb)=");
    uwrite(ok_b2i ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 vkCmdCopyImageToBuffer2 -- 2x2 Bild (gemappt gefuellt) -> Buffer, Readback exakt. */
    int ok_i2b = 1;
    {
        VkImageCreateInfo iic; memset(&iic, 0, sizeof(iic));
        iic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; iic.imageType = VK_IMAGE_TYPE_2D;
        iic.format = VK_FORMAT_B8G8R8A8_UNORM; iic.extent.width = 2; iic.extent.height = 2; iic.extent.depth = 1;
        iic.mipLevels = 1; iic.arrayLayers = 1; iic.samples = VK_SAMPLE_COUNT_1_BIT;
        iic.tiling = VK_IMAGE_TILING_LINEAR; iic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage iimg = VK_NULL_HANDLE;
        ok_i2b = (vkCreateImage(dev, &iic, 0, &iimg) == VK_SUCCESS);
        VkMemoryRequirements iimr; vkGetImageMemoryRequirements(dev, iimg, &iimr);
        VkMemoryAllocateInfo iimai; memset(&iimai, 0, sizeof(iimai));
        iimai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; iimai.allocationSize = iimr.size;
        VkDeviceMemory iimem = VK_NULL_HANDLE;
        ok_i2b = ok_i2b && (vkAllocateMemory(dev, &iimai, 0, &iimem) == VK_SUCCESS) &&
                 (vkBindImageMemory(dev, iimg, iimem, 0) == VK_SUCCESS);
        void *iimap = 0;
        if (ok_i2b && vkMapMemory(dev, iimem, 0, VK_WHOLE_SIZE, 0, &iimap) == VK_SUCCESS) {
            unsigned *ipx = (unsigned *)iimap;   /* row_pitch/4 = 2 */
            ipx[0] = 0xFF112233u; ipx[1] = 0xFF445566u; ipx[2] = 0xFF778899u; ipx[3] = 0xFFAABBCCu;
            vkUnmapMemory(dev, iimem);
        } else { ok_i2b = 0; }
        VkBufferCreateInfo dbci; memset(&dbci, 0, sizeof(dbci));
        dbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; dbci.size = 16; dbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer dbuf = VK_NULL_HANDLE;
        ok_i2b = ok_i2b && (vkCreateBuffer(dev, &dbci, 0, &dbuf) == VK_SUCCESS);
        VkMemoryRequirements dbmr; vkGetBufferMemoryRequirements(dev, dbuf, &dbmr);
        VkMemoryAllocateInfo dbmai; memset(&dbmai, 0, sizeof(dbmai));
        dbmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; dbmai.allocationSize = dbmr.size;
        VkDeviceMemory dbmem = VK_NULL_HANDLE;
        ok_i2b = ok_i2b && (vkAllocateMemory(dev, &dbmai, 0, &dbmem) == VK_SUCCESS) &&
                 (vkBindBufferMemory(dev, dbuf, dbmem, 0) == VK_SUCCESS);
        VkCommandBufferAllocateInfo icbi; memset(&icbi, 0, sizeof(icbi));
        icbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; icbi.commandPool = cpool; icbi.commandBufferCount = 1;
        VkCommandBuffer icmd = VK_NULL_HANDLE;
        ok_i2b = ok_i2b && (vkAllocateCommandBuffers(dev, &icbi, &icmd) == VK_SUCCESS);
        if (ok_i2b) {
            VkCommandBufferBeginInfo ibb; memset(&ibb, 0, sizeof(ibb)); ibb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(icmd, &ibb);
            VkBufferImageCopy2 reg; memset(&reg, 0, sizeof(reg));
            reg.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2; reg.bufferOffset = 0;
            reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; reg.imageSubresource.layerCount = 1;
            reg.imageExtent.width = 2; reg.imageExtent.height = 2; reg.imageExtent.depth = 1;
            VkCopyImageToBufferInfo2 ci; memset(&ci, 0, sizeof(ci));
            ci.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
            ci.srcImage = iimg; ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            ci.dstBuffer = dbuf; ci.regionCount = 1; ci.pRegions = &reg;
            vkCmdCopyImageToBuffer2(icmd, &ci);
            vkEndCommandBuffer(icmd);
            VkFence iff = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &iff);
            VkSubmitInfo isi; memset(&isi, 0, sizeof(isi)); isi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            isi.commandBufferCount = 1; isi.pCommandBuffers = &icmd;
            vkQueueSubmit(q, 1, &isi, iff);
            vkWaitForFences(dev, 1, &iff, VK_TRUE, ~0ull);
            void *dbm = 0; int got = 0;
            if (vkMapMemory(dev, dbmem, 0, VK_WHOLE_SIZE, 0, &dbm) == VK_SUCCESS) {
                const unsigned *bp = (const unsigned *)dbm;
                got = (bp[0] == 0xFF112233u && bp[1] == 0xFF445566u && bp[2] == 0xFF778899u && bp[3] == 0xFFAABBCCu);
                vkUnmapMemory(dev, dbmem);
            }
            ok_i2b = got;
            vkDestroyFence(dev, iff, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &icmd);
        }
        vkDestroyImage(dev, iimg, 0); vkFreeMemory(dev, iimem, 0);
        vkDestroyBuffer(dev, dbuf, 0); vkFreeMemory(dev, dbmem, 0);
    }
    all = all && ok_i2b;
    uwrite("[vktest] vk V3: copy-image-to-buffer2 (2x2 image -> buffer, exakt)=");
    uwrite(ok_i2b ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 vkCmdCopyImage2 -- 2x2 Bild -> Bild (gleiche Groesse), Readback exakt. */
    int ok_i2i = 1;
    {
        VkImageCreateInfo cic; memset(&cic, 0, sizeof(cic));
        cic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; cic.imageType = VK_IMAGE_TYPE_2D;
        cic.format = VK_FORMAT_B8G8R8A8_UNORM; cic.extent.width = 2; cic.extent.height = 2; cic.extent.depth = 1;
        cic.mipLevels = 1; cic.arrayLayers = 1; cic.samples = VK_SAMPLE_COUNT_1_BIT;
        cic.tiling = VK_IMAGE_TILING_LINEAR; cic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage csrc = VK_NULL_HANDLE, cdst = VK_NULL_HANDLE;
        ok_i2i = (vkCreateImage(dev, &cic, 0, &csrc) == VK_SUCCESS) && (vkCreateImage(dev, &cic, 0, &cdst) == VK_SUCCESS);
        VkMemoryRequirements cimr; vkGetImageMemoryRequirements(dev, csrc, &cimr);
        VkMemoryAllocateInfo cimai; memset(&cimai, 0, sizeof(cimai));
        cimai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; cimai.allocationSize = cimr.size;
        VkDeviceMemory csmem = VK_NULL_HANDLE, cdmem = VK_NULL_HANDLE;
        ok_i2i = ok_i2i && (vkAllocateMemory(dev, &cimai, 0, &csmem) == VK_SUCCESS) &&
                 (vkAllocateMemory(dev, &cimai, 0, &cdmem) == VK_SUCCESS) &&
                 (vkBindImageMemory(dev, csrc, csmem, 0) == VK_SUCCESS) &&
                 (vkBindImageMemory(dev, cdst, cdmem, 0) == VK_SUCCESS);
        void *csm = 0;
        if (ok_i2i && vkMapMemory(dev, csmem, 0, VK_WHOLE_SIZE, 0, &csm) == VK_SUCCESS) {
            unsigned *sp = (unsigned *)csm; sp[0]=0xFF010203u; sp[1]=0xFF040506u; sp[2]=0xFF070809u; sp[3]=0xFF0A0B0Cu;
            vkUnmapMemory(dev, csmem);
        } else { ok_i2i = 0; }
        void *cdm0 = 0;
        if (ok_i2i && vkMapMemory(dev, cdmem, 0, VK_WHOLE_SIZE, 0, &cdm0) == VK_SUCCESS) {
            unsigned *dp = (unsigned *)cdm0; dp[0]=dp[1]=dp[2]=dp[3]=0xFFFFFFFFu; vkUnmapMemory(dev, cdmem);
        }
        VkCommandBufferAllocateInfo cicbi; memset(&cicbi, 0, sizeof(cicbi));
        cicbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cicbi.commandPool = cpool; cicbi.commandBufferCount = 1;
        VkCommandBuffer cicmd = VK_NULL_HANDLE;
        ok_i2i = ok_i2i && (vkAllocateCommandBuffers(dev, &cicbi, &cicmd) == VK_SUCCESS);
        if (ok_i2i) {
            VkCommandBufferBeginInfo cibb; memset(&cibb, 0, sizeof(cibb)); cibb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cicmd, &cibb);
            VkImageCopy2 reg; memset(&reg, 0, sizeof(reg));
            reg.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
            reg.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; reg.srcSubresource.layerCount = 1;
            reg.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; reg.dstSubresource.layerCount = 1;
            reg.extent.width = 2; reg.extent.height = 2; reg.extent.depth = 1;
            VkCopyImageInfo2 ci; memset(&ci, 0, sizeof(ci));
            ci.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
            ci.srcImage = csrc; ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            ci.dstImage = cdst; ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ci.regionCount = 1; ci.pRegions = &reg;
            vkCmdCopyImage2(cicmd, &ci);
            vkEndCommandBuffer(cicmd);
            VkFence cif = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &cif);
            VkSubmitInfo cisi; memset(&cisi, 0, sizeof(cisi)); cisi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            cisi.commandBufferCount = 1; cisi.pCommandBuffers = &cicmd;
            vkQueueSubmit(q, 1, &cisi, cif);
            vkWaitForFences(dev, 1, &cif, VK_TRUE, ~0ull);
            void *cdm = 0; int got = 0;
            if (vkMapMemory(dev, cdmem, 0, VK_WHOLE_SIZE, 0, &cdm) == VK_SUCCESS) {
                const unsigned *dp = (const unsigned *)cdm;
                got = (dp[0]==0xFF010203u && dp[1]==0xFF040506u && dp[2]==0xFF070809u && dp[3]==0xFF0A0B0Cu);
                vkUnmapMemory(dev, cdmem);
            }
            ok_i2i = got;
            vkDestroyFence(dev, cif, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &cicmd);
        }
        vkDestroyImage(dev, csrc, 0); vkDestroyImage(dev, cdst, 0);
        vkFreeMemory(dev, csmem, 0); vkFreeMemory(dev, cdmem, 0);
    }
    all = all && ok_i2i;
    uwrite("[vktest] vk V3: copy-image2 (2x2 image -> image, exakt)=");
    uwrite(ok_i2i ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3: Core-1.3 vkCmdBlitImage2 -- 2x2 -> 4x4 nearest-Upscale: jedes Quell-Pixel -> 2x2 Block. */
    int ok_blit = 1;
    {
        VkImageCreateInfo bsic; memset(&bsic, 0, sizeof(bsic));
        bsic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; bsic.imageType = VK_IMAGE_TYPE_2D;
        bsic.format = VK_FORMAT_B8G8R8A8_UNORM; bsic.extent.width = 2; bsic.extent.height = 2; bsic.extent.depth = 1;
        bsic.mipLevels = 1; bsic.arrayLayers = 1; bsic.samples = VK_SAMPLE_COUNT_1_BIT;
        bsic.tiling = VK_IMAGE_TILING_LINEAR; bsic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageCreateInfo bdic = bsic; bdic.extent.width = 4; bdic.extent.height = 4;
        VkImage bsrc = VK_NULL_HANDLE, bdst = VK_NULL_HANDLE;
        ok_blit = (vkCreateImage(dev, &bsic, 0, &bsrc) == VK_SUCCESS) && (vkCreateImage(dev, &bdic, 0, &bdst) == VK_SUCCESS);
        VkMemoryRequirements bsmr, bdmr; vkGetImageMemoryRequirements(dev, bsrc, &bsmr); vkGetImageMemoryRequirements(dev, bdst, &bdmr);
        VkMemoryAllocateInfo bsmai; memset(&bsmai, 0, sizeof(bsmai)); bsmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; bsmai.allocationSize = bsmr.size;
        VkMemoryAllocateInfo bdmai; memset(&bdmai, 0, sizeof(bdmai)); bdmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; bdmai.allocationSize = bdmr.size;
        VkDeviceMemory bsmem = VK_NULL_HANDLE, bdmem = VK_NULL_HANDLE;
        ok_blit = ok_blit && (vkAllocateMemory(dev, &bsmai, 0, &bsmem) == VK_SUCCESS) &&
                  (vkAllocateMemory(dev, &bdmai, 0, &bdmem) == VK_SUCCESS) &&
                  (vkBindImageMemory(dev, bsrc, bsmem, 0) == VK_SUCCESS) &&
                  (vkBindImageMemory(dev, bdst, bdmem, 0) == VK_SUCCESS);
        void *bsm = 0;
        if (ok_blit && vkMapMemory(dev, bsmem, 0, VK_WHOLE_SIZE, 0, &bsm) == VK_SUCCESS) {
            unsigned *sp = (unsigned *)bsm;   /* row pitch 2 */
            sp[0]=0xFF111111u; sp[1]=0xFF222222u; sp[2]=0xFF333333u; sp[3]=0xFF444444u;
            vkUnmapMemory(dev, bsmem);
        } else { ok_blit = 0; }
        VkCommandBufferAllocateInfo bcbi; memset(&bcbi, 0, sizeof(bcbi));
        bcbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; bcbi.commandPool = cpool; bcbi.commandBufferCount = 1;
        VkCommandBuffer bcmd = VK_NULL_HANDLE;
        ok_blit = ok_blit && (vkAllocateCommandBuffers(dev, &bcbi, &bcmd) == VK_SUCCESS);
        if (ok_blit) {
            VkCommandBufferBeginInfo bbb; memset(&bbb, 0, sizeof(bbb)); bbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(bcmd, &bbb);
            VkImageBlit2 reg; memset(&reg, 0, sizeof(reg));
            reg.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
            reg.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; reg.srcSubresource.layerCount = 1;
            reg.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; reg.dstSubresource.layerCount = 1;
            reg.srcOffsets[1].x = 2; reg.srcOffsets[1].y = 2; reg.srcOffsets[1].z = 1;
            reg.dstOffsets[1].x = 4; reg.dstOffsets[1].y = 4; reg.dstOffsets[1].z = 1;
            VkBlitImageInfo2 bi; memset(&bi, 0, sizeof(bi));
            bi.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
            bi.srcImage = bsrc; bi.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bi.dstImage = bdst; bi.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bi.regionCount = 1; bi.pRegions = &reg; bi.filter = VK_FILTER_NEAREST;
            vkCmdBlitImage2(bcmd, &bi);
            vkEndCommandBuffer(bcmd);
            VkFence bf = VK_NULL_HANDLE; vkCreateFence(dev, &fci, 0, &bf);
            VkSubmitInfo bsi; memset(&bsi, 0, sizeof(bsi)); bsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            bsi.commandBufferCount = 1; bsi.pCommandBuffers = &bcmd;
            vkQueueSubmit(q, 1, &bsi, bf);
            vkWaitForFences(dev, 1, &bf, VK_TRUE, ~0ull);
            void *bdm = 0; int got = 0;
            if (vkMapMemory(dev, bdmem, 0, VK_WHOLE_SIZE, 0, &bdm) == VK_SUCCESS) {
                const unsigned *dp = (const unsigned *)bdm;   /* dst row pitch 4 */
                got = (dp[0]==0xFF111111u && dp[3]==0xFF222222u &&      /* row0: A..A B..B */
                       dp[8]==0xFF333333u && dp[15]==0xFF444444u &&     /* dst(0,2)=C, dst(3,3)=D */
                       dp[5]==0xFF111111u && dp[10]==0xFF444444u);      /* dst(1,1)=A, dst(2,2)=D */
                vkUnmapMemory(dev, bdmem);
            }
            ok_blit = got;
            vkDestroyFence(dev, bf, 0);
            vkFreeCommandBuffers(dev, cpool, 1, &bcmd);
        }
        vkDestroyImage(dev, bsrc, 0); vkDestroyImage(dev, bdst, 0);
        vkFreeMemory(dev, bsmem, 0); vkFreeMemory(dev, bdmem, 0);
    }
    all = all && ok_blit;
    uwrite("[vktest] vk V3: blit-image2 (2x2 -> 4x4 nearest-upscale, Bloecke exakt)=");
    uwrite(ok_blit ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V3.4: Core-1.4 vkMapMemory2/vkUnmapMemory2 -- map2 schreiben, unmap2, map2 lesen. */
    int ok_map2 = 1;
    {
        VkBufferCreateInfo mbci; memset(&mbci, 0, sizeof(mbci));
        mbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; mbci.size = 8; mbci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer mbuf = VK_NULL_HANDLE;
        ok_map2 = (vkCreateBuffer(dev, &mbci, 0, &mbuf) == VK_SUCCESS);
        VkMemoryRequirements mmr; vkGetBufferMemoryRequirements(dev, mbuf, &mmr);
        VkMemoryAllocateInfo mmai; memset(&mmai, 0, sizeof(mmai));
        mmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mmai.allocationSize = mmr.size;
        VkDeviceMemory mmem = VK_NULL_HANDLE;
        ok_map2 = ok_map2 && (vkAllocateMemory(dev, &mmai, 0, &mmem) == VK_SUCCESS) &&
                  (vkBindBufferMemory(dev, mbuf, mmem, 0) == VK_SUCCESS);
        VkMemoryMapInfo mmi; memset(&mmi, 0, sizeof(mmi));
        mmi.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO; mmi.memory = mmem; mmi.offset = 0; mmi.size = VK_WHOLE_SIZE;
        void *p1 = 0;
        ok_map2 = ok_map2 && (vkMapMemory2(dev, &mmi, &p1) == VK_SUCCESS) && (p1 != 0);
        if (ok_map2) { ((unsigned *)p1)[0] = 0xBEEF1234u; }
        VkMemoryUnmapInfo mui; memset(&mui, 0, sizeof(mui));
        mui.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO; mui.memory = mmem;
        ok_map2 = ok_map2 && (vkUnmapMemory2(dev, &mui) == VK_SUCCESS);
        void *p2 = 0;
        ok_map2 = ok_map2 && (vkMapMemory2(dev, &mmi, &p2) == VK_SUCCESS) && (p2 == p1) &&
                  (((unsigned *)p2)[0] == 0xBEEF1234u);
        vkUnmapMemory2(dev, &mui);
        vkDestroyBuffer(dev, mbuf, 0); vkFreeMemory(dev, mmem, 0);
    }
    all = all && ok_map2;
    uwrite("[vktest] vk V3.4: map-memory2 (map2 schreiben/unmap2/map2 lesen -> 0xBEEF1234)=");
    uwrite(ok_map2 ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- V1.6: MSAA -- 4x-Multisample-Farbe+Tiefe (32x32), flaches gruenes Dreieck mit schraegen
     * Kanten; Resolve (Mittelung) in ein Single-Sample-Ziel. Kanten-Pixel bekommen partielle Deckung
     * -> Zwischen-Gruen (AA); ohne MSAA waere jede Kante hart (0 oder 0xFF). Kleine 32x32 (Bump-Heap
     * teilt alle Allokationen). Wiederverwendet rp/vbuf/layout/ident/gpi/cpool/fci. */
    int ok_msaa = 1, ok_aa = 0, ok_full = 0, ok_bg = 0;
    {
        const unsigned MW = 32, MH = 32;
        VkImageCreateInfo mici;
        memset(&mici, 0, sizeof(mici));
        mici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        mici.imageType = VK_IMAGE_TYPE_2D;
        mici.format = VK_FORMAT_B8G8R8A8_UNORM;
        mici.extent.width = MW; mici.extent.height = MH; mici.extent.depth = 1;
        mici.mipLevels = 1; mici.arrayLayers = 1;
        mici.samples = VK_SAMPLE_COUNT_4_BIT;
        mici.tiling = VK_IMAGE_TILING_OPTIMAL;
        mici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage mscol = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkCreateImage(dev, &mici, 0, &mscol) == VK_SUCCESS);
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, mscol, &mr);
        mai.allocationSize = mr.size;
        VkDeviceMemory mscmem = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkAllocateMemory(dev, &mai, 0, &mscmem) == VK_SUCCESS) &&
                  (vkBindImageMemory(dev, mscol, mscmem, 0) == VK_SUCCESS);
        mici.format = VK_FORMAT_D32_SFLOAT;
        mici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VkImage msdep = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkCreateImage(dev, &mici, 0, &msdep) == VK_SUCCESS);
        vkGetImageMemoryRequirements(dev, msdep, &mr);
        mai.allocationSize = mr.size;
        VkDeviceMemory msdmem = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkAllocateMemory(dev, &mai, 0, &msdmem) == VK_SUCCESS) &&
                  (vkBindImageMemory(dev, msdep, msdmem, 0) == VK_SUCCESS);
        mici.format = VK_FORMAT_B8G8R8A8_UNORM;
        mici.samples = VK_SAMPLE_COUNT_1_BIT;
        mici.tiling = VK_IMAGE_TILING_LINEAR;              /* Resolve-Ziel: Readback + rowPitch-Query */
        mici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage msres = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkCreateImage(dev, &mici, 0, &msres) == VK_SUCCESS);
        vkGetImageMemoryRequirements(dev, msres, &mr);
        mai.allocationSize = mr.size;
        VkDeviceMemory msrmem = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkAllocateMemory(dev, &mai, 0, &msrmem) == VK_SUCCESS) &&
                  (vkBindImageMemory(dev, msres, msrmem, 0) == VK_SUCCESS);

        VkImageViewCreateInfo vci;
        memset(&vci, 0, sizeof(vci));
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.image = mscol; vci.format = VK_FORMAT_B8G8R8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
        VkImageView mscview = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkCreateImageView(dev, &vci, 0, &mscview) == VK_SUCCESS);
        vci.image = msdep; vci.format = VK_FORMAT_D32_SFLOAT;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        VkImageView msdview = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkCreateImageView(dev, &vci, 0, &msdview) == VK_SUCCESS);

        VkImageView msatt[2] = { mscview, msdview };
        VkFramebufferCreateInfo mfbi;
        memset(&mfbi, 0, sizeof(mfbi));
        mfbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        mfbi.renderPass = rp; mfbi.attachmentCount = 2; mfbi.pAttachments = msatt;
        mfbi.width = MW; mfbi.height = MH; mfbi.layers = 1;
        VkFramebuffer msfb = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkCreateFramebuffer(dev, &mfbi, 0, &msfb) == VK_SUCCESS);

        VkViewport mvp; VkRect2D msc;
        memset(&mvp, 0, sizeof(mvp)); memset(&msc, 0, sizeof(msc));
        mvp.width = (float)MW; mvp.height = (float)MH; mvp.maxDepth = 1.0f;
        msc.extent.width = MW; msc.extent.height = MH;
        VkPipelineViewportStateCreateInfo mvps;
        memset(&mvps, 0, sizeof(mvps));
        mvps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        mvps.viewportCount = 1; mvps.pViewports = &mvp;
        mvps.scissorCount = 1; mvps.pScissors = &msc;
        VkPipelineMultisampleStateCreateInfo mms;
        memset(&mms, 0, sizeof(mms));
        mms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        mms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
        VkGraphicsPipelineCreateInfo mgpi = gpi;           /* Basis-Pipeline kopieren ... */
        mgpi.pViewportState = &mvps;                       /* ... 32x32-Viewport ... */
        mgpi.pMultisampleState = &mms;                     /* ... + 4x MSAA. */
        VkPipeline mspipe = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &mgpi, 0, &mspipe) == VK_SUCCESS);

        VkCommandBufferAllocateInfo mcbi;
        memset(&mcbi, 0, sizeof(mcbi));
        mcbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        mcbi.commandPool = cpool; mcbi.commandBufferCount = 1;
        VkCommandBuffer mcmd = VK_NULL_HANDLE;
        ok_msaa = ok_msaa && (vkAllocateCommandBuffers(dev, &mcbi, &mcmd) == VK_SUCCESS);
        VkCommandBufferBeginInfo mbb; memset(&mbb, 0, sizeof(mbb));
        mbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(mcmd, &mbb);
        VkClearValue mclears[2];
        memset(mclears, 0, sizeof(mclears));
        mclears[0].color.float32[3] = 1.0f;               /* schwarz, alpha 1 */
        mclears[1].depthStencil.depth = 1.0f;
        VkRenderPassBeginInfo mrbi; memset(&mrbi, 0, sizeof(mrbi));
        mrbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        mrbi.renderPass = rp; mrbi.framebuffer = msfb;
        mrbi.renderArea.extent.width = MW; mrbi.renderArea.extent.height = MH;
        mrbi.clearValueCount = 2; mrbi.pClearValues = mclears;
        vkCmdBeginRenderPass(mcmd, &mrbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(mcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mspipe);
        VkDeviceSize mz = 0;
        vkCmdBindVertexBuffers(mcmd, 0, 1, &vbuf, &mz);
        vkCmdPushConstants(mcmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
        vkCmdDraw(mcmd, 3, 1, 0, 0);                       /* verts[0..2] = flaches gruenes Dreieck */
        vkCmdEndRenderPass(mcmd);
        VkImageResolve mregion;
        memset(&mregion, 0, sizeof(mregion));
        mregion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mregion.srcSubresource.layerCount = 1;
        mregion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mregion.dstSubresource.layerCount = 1;
        mregion.extent.width = MW; mregion.extent.height = MH; mregion.extent.depth = 1;
        vkCmdResolveImage(mcmd, mscol, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          msres, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mregion);
        vkEndCommandBuffer(mcmd);

        VkFence mfence = VK_NULL_HANDLE;
        vkCreateFence(dev, &fci, 0, &mfence);
        VkSubmitInfo msi; memset(&msi, 0, sizeof(msi));
        msi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        msi.commandBufferCount = 1; msi.pCommandBuffers = &mcmd;
        vkQueueSubmit(q, 1, &msi, mfence);
        vkWaitForFences(dev, 1, &mfence, VK_TRUE, ~0ull);

        void *rmap = 0;
        if (ok_msaa && vkMapMemory(dev, msrmem, 0, VK_WHOLE_SIZE, 0, &rmap) == VK_SUCCESS) {
            const unsigned *rw = (const unsigned *)rmap;
            VkImageSubresource sub; memset(&sub, 0, sizeof(sub));
            sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            VkSubresourceLayout sl; memset(&sl, 0, sizeof(sl));
            vkGetImageSubresourceLayout(dev, msres, &sub, &sl);
            unsigned rpp = (unsigned)(sl.rowPitch / 4);
            for (unsigned y = 0; y < MH; y++) {
                for (unsigned x = 0; x < MW; x++) {
                    unsigned g = (rw[y * rpp + x] >> 8) & 0xFF;
                    if (g > 0x10 && g < 0xF0) { ok_aa = 1; }   /* echtes Zwischen-Gruen (partielle Deckung) */
                }
            }
            ok_full = (((rw[16 * rpp + 16] >> 8) & 0xFF) == 0xFF);   /* Zentrum: voll gedeckt */
            ok_bg   = (((rw[0  * rpp + 0 ] >> 8) & 0xFF) == 0x00);   /* Ecke: Hintergrund */
            vkUnmapMemory(dev, msrmem);
        }

        vkDestroyPipeline(dev, mspipe, 0);
        vkDestroyFramebuffer(dev, msfb, 0);
        vkDestroyImageView(dev, mscview, 0); vkDestroyImageView(dev, msdview, 0);
        vkDestroyFence(dev, mfence, 0);
        vkFreeCommandBuffers(dev, cpool, 1, &mcmd);
        vkDestroyImage(dev, mscol, 0); vkFreeMemory(dev, mscmem, 0);
        vkDestroyImage(dev, msdep, 0); vkFreeMemory(dev, msdmem, 0);
        vkDestroyImage(dev, msres, 0); vkFreeMemory(dev, msrmem, 0);
    }
    all = all && ok_msaa && ok_aa && ok_full && ok_bg;
    uwrite("[vktest] vkdraw V1.6: msaa=");
    uwrite((ok_msaa && ok_aa && ok_full && ok_bg) ? "ok" : "FEHLER");
    uwrite(" (4x multisample: kanten-AA zwischen-gruen, interior voll, ecke hintergrund, resolve)\n");

    /* Aufraeumen. */
    vkDestroyFence(dev, fence, 0);
    vkFreeCommandBuffers(dev, cpool, 1, &cmd);
    vkDestroyCommandPool(dev, cpool, 0);
    vkDestroyPipeline(dev, pipe, 0);
    vkDestroyPipelineLayout(dev, layout, 0);
    vkDestroyShaderModule(dev, vs, 0);
    vkDestroyShaderModule(dev, fs, 0);
    vkDestroyFramebuffer(dev, fb, 0);
    vkDestroyRenderPass(dev, rp, 0);
    vkDestroyImageView(dev, cview, 0);
    vkDestroyImageView(dev, dview, 0);
    vkDestroyBuffer(dev, vbuf, 0);
    vkFreeMemory(dev, vmem, 0);
    vkDestroyImage(dev, cimg, 0);
    vkDestroyImage(dev, dimg, 0);
    vkFreeMemory(dev, cmem, 0);
    vkFreeMemory(dev, dmem, 0);
    vkDestroyDevice(dev, 0);
    vkDestroyInstance(inst, 0);
    return all;
}

void _start(void)
{
    gui_t g;
    char nb[24];
    if (gui_init(&g) != 0) {
        uwrite("[vktest] gui_init fehlgeschlagen (keine Bruecke/GUI-Cap)\n");
        sys3(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    r3d_target_t t;
    t.color = (unsigned *)g.bb;
    t.depth = g_depth;
    t.width = (int)g.width; t.height = (int)g.height; t.pitch_px = (int)g.wpr;
    t.vp_x = 0.0f; t.vp_y = 0.0f; t.vp_w = 640.0f; t.vp_h = 480.0f;
    t.vp_minz = 0.0f; t.vp_maxz = 1.0f;
    t.sc_x = 0; t.sc_y = 0; t.sc_w = 640; t.sc_h = 480;
    t.cull_mode = R3D_CULL_NONE; t.front_ccw = 1;
    t.depth_test = 1; t.depth_write = 1; t.depth_compare = 1;   /* V3b: LESS */
    t.samples = 1;

    gui_clear(&g, BG);
    r3d_depth_clear(g_depth, 640 * 480, 1.0f);

    /* --- (1) flat: konstant rotes Dreieck --- */
    {
        r3d_vtx_t a = sv(40.5f, 40.5f, 0.5f, 1, 0, 0);
        r3d_vtx_t b = sv(200.5f, 40.5f, 0.5f, 1, 0, 0);
        r3d_vtx_t c = sv(40.5f, 200.5f, 0.5f, 1, 0, 0);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    int ok_flat = (gui_get(&g, 100, 80) == 0xFF0000u) && (gui_get(&g, 300, 80) == BG);

    /* --- (2) gouraud: Zentroid EXAKT auf Pixelzentrum (320,200) -> 1/3,1/3,1/3 --- */
    {
        r3d_vtx_t a = sv(160.5f, 120.5f, 0.4f, 1, 0, 0);
        r3d_vtx_t b = sv(480.5f, 120.5f, 0.4f, 0, 1, 0);
        r3d_vtx_t c = sv(320.5f, 360.5f, 0.4f, 0, 0, 1);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    int ok_gour = (gui_get(&g, 320, 200) == 0x555555u);

    /* --- (3) tiefe: nah (gruen, z=0.2) zuerst, fern (rot, z=0.8) danach --- */
    {
        r3d_vtx_t a = sv(500.5f, 60.5f, 0.2f, 0, 1, 0);
        r3d_vtx_t b = sv(600.5f, 60.5f, 0.2f, 0, 1, 0);
        r3d_vtx_t c = sv(500.5f, 160.5f, 0.2f, 0, 1, 0);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
        a = sv(500.5f, 60.5f, 0.8f, 1, 0, 0);
        b = sv(600.5f, 60.5f, 0.8f, 1, 0, 0);
        c = sv(500.5f, 160.5f, 0.8f, 1, 0, 0);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    int ok_depth = (gui_get(&g, 520, 80) == 0x00FF00u);

    /* --- (4) cull: BACK verwerfen, Front-Winding erscheint --- */
    t.cull_mode = R3D_CULL_BACK;                    /* front_ccw = 1 (Vulkan-Stil) */
    {
        /* clockwise am Schirm = BACK -> verworfen */
        r3d_vtx_t a = sv(60.5f, 300.5f, 0.3f, 1, 1, 1);
        r3d_vtx_t b = sv(160.5f, 300.5f, 0.3f, 1, 1, 1);
        r3d_vtx_t c = sv(60.5f, 400.5f, 0.3f, 1, 1, 1);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    int ok_cull = (gui_get(&g, 80, 320) == BG);
    {
        /* counter-clockwise = FRONT -> erscheint */
        r3d_vtx_t a = sv(60.5f, 300.5f, 0.3f, 1, 1, 1);
        r3d_vtx_t b = sv(60.5f, 400.5f, 0.3f, 1, 1, 1);
        r3d_vtx_t c = sv(160.5f, 300.5f, 0.3f, 1, 1, 1);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    ok_cull = ok_cull && (gui_get(&g, 80, 320) == 0xFFFFFFu);
    t.cull_mode = R3D_CULL_NONE;

    /* --- (5) fillrule: geteilte Diagonale (TR->BL), Zaehl-Shader -> jedes Pixel GENAU 1x --- */
    int doppel = 0, luecken = 0;
    {
        int base[2] = { 240, 300 };
        r3d_vtx_t a = sv(240.5f, 300.5f, 0.25f, 0, 0, 0);
        r3d_vtx_t b = sv(304.5f, 300.5f, 0.25f, 0, 0, 0);
        r3d_vtx_t c = sv(240.5f, 364.5f, 0.25f, 0, 0, 0);
        r3d_draw_tri(&t, &a, &b, &c, 0, fs_count, base);
        a = sv(304.5f, 300.5f, 0.25f, 0, 0, 0);
        b = sv(304.5f, 364.5f, 0.25f, 0, 0, 0);
        c = sv(240.5f, 364.5f, 0.25f, 0, 0, 0);
        r3d_draw_tri(&t, &a, &b, &c, 0, fs_count, base);
        cov_eval(&doppel, &luecken);
    }
    int ok_fill = (doppel == 0 && luecken == 0);

    /* --- (5b) fillrule, ANDERE Diagonalrichtung (TL->BR) -- Review-Befund: beide Slopes --- */
    {
        int base[2] = { 320, 380 };
        for (int i = 0; i < 64 * 64; i++) { g_cov[i] = 0; }
        r3d_vtx_t a = sv(320.5f, 380.5f, 0.25f, 0, 0, 0);
        r3d_vtx_t b = sv(384.5f, 380.5f, 0.25f, 0, 0, 0);
        r3d_vtx_t c = sv(384.5f, 444.5f, 0.25f, 0, 0, 0);
        r3d_draw_tri(&t, &a, &b, &c, 0, fs_count, base);
        a = sv(320.5f, 380.5f, 0.25f, 0, 0, 0);
        b = sv(384.5f, 444.5f, 0.25f, 0, 0, 0);
        c = sv(320.5f, 444.5f, 0.25f, 0, 0, 0);
        r3d_draw_tri(&t, &a, &b, &c, 0, fs_count, base);
        int d2, l2;
        cov_eval(&d2, &l2);
        if (d2 != 0 || l2 != 0) { ok_fill = 0; doppel += d2; luecken += l2; }
    }

    /* --- (6) nearclip: eine Ecke mit z_clip < 0 (vor der Near-Plane) --- */
    {
        r3d_vtx_t a = sv(160.5f, 120.5f, 0.5f, 1, 1, 0);   /* wird unten manuell versetzt */
        r3d_vtx_t b = sv(480.5f, 120.5f, 0.5f, 1, 1, 0);
        r3d_vtx_t c = sv(320.5f, 360.5f, -1.0f, 1, 1, 0);  /* z<0 -> geclippt */
        /* eigenes Zielgebiet rechts unten, damit fruehere Proben unberuehrt bleiben */
        a = sv(400.5f, 250.5f, 0.5f, 1, 1, 0);
        b = sv(620.5f, 250.5f, 0.5f, 1, 1, 0);
        c = sv(510.5f, 460.5f, -1.0f, 1, 1, 0);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    /* Kept-Bereich: nahe der Oberkante; Cut-Bereich: nahe der z<0-Ecke. Die Schnittkante
     * liegt bei t=0.5/(0.5-(-1))=1/3 des Wegs -> y ~ 250.5+70 = ~320. */
    int ok_clip = (gui_get(&g, 510, 260) == 0xFFFF00u) && (gui_get(&g, 510, 430) == BG);

    /* --- (7) persp: perspektivisch korrekte Attribut-Interpolation (Review-Befund:
     * alle bisherigen Proben nutzten w=1 -> noperspective bliebe unentdeckt). Kante
     * v0(w=1,grau 0) -> v1(w=2,grau 1); Probe am MITTELPUNKT der Kante: korrekt ist
     * (0.5*0 + 0.5*0.5)/(0.5*1 + 0.5*0.5) = 1/3 -> 85; screen-linear waere 128. --- */
    {
        r3d_vtx_t a = svw(100.5f, 415.5f, 0.6f, 1.0f, 0, 0, 0);
        r3d_vtx_t b = svw(228.5f, 415.5f, 0.6f, 2.0f, 1, 1, 1);
        r3d_vtx_t c = svw(100.5f, 470.5f, 0.6f, 1.0f, 0, 0, 0);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    int ok_persp;
    {
        unsigned px = gui_get(&g, 164, 415);
        int ch = (int)((px >> 16) & 0xFF);
        ok_persp = (ch >= 83 && ch <= 87);            /* 1/3*255=85 (+/-2); linear=128 */
    }

    /* --- (8) farclip: eine Ecke HINTER der Far-Plane (z > w) -- Spiegel von (6) --- */
    {
        r3d_vtx_t a = sv(40.5f, 215.5f, 0.5f, 0, 1, 1);
        r3d_vtx_t b = sv(150.5f, 215.5f, 0.5f, 0, 1, 1);
        r3d_vtx_t c = sv(95.5f, 299.5f, 1.5f, 0, 1, 1);   /* z=1.5 > w=1 -> geclippt */
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    int ok_far = (gui_get(&g, 95, 225) == 0x00FFFFu) && (gui_get(&g, 95, 285) == BG);

    /* --- (9) cullfront: R3D_CULL_FRONT verwirft Front (CCW), laesst Back (CW) durch --- */
    t.cull_mode = R3D_CULL_FRONT;
    {
        r3d_vtx_t a = sv(560.5f, 380.5f, 0.3f, 1, 1, 1);   /* CCW = Front -> verworfen */
        r3d_vtx_t b = sv(560.5f, 440.5f, 0.3f, 1, 1, 1);
        r3d_vtx_t c = sv(620.5f, 380.5f, 0.3f, 1, 1, 1);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    int ok_cf = (gui_get(&g, 575, 395) == BG);
    {
        r3d_vtx_t a = sv(560.5f, 380.5f, 0.3f, 1, 0.5f, 1);  /* CW = Back -> erscheint */
        r3d_vtx_t b = sv(620.5f, 380.5f, 0.3f, 1, 0.5f, 1);
        r3d_vtx_t c = sv(560.5f, 440.5f, 0.3f, 1, 0.5f, 1);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    ok_cf = ok_cf && (gui_get(&g, 575, 395) == 0xFF80FFu);
    t.cull_mode = R3D_CULL_NONE;

    /* --- (10) scissor: Fragmente ausserhalb des Scissor-Rechtecks werden verworfen --- */
    t.sc_x = 8; t.sc_y = 8; t.sc_w = 40; t.sc_h = 40;      /* nur [8..48)x[8..48) */
    {
        r3d_vtx_t a = sv(0.5f, 0.5f, 0.1f, 1, 0, 1);        /* Magenta, deckt (0..100)^2 */
        r3d_vtx_t b = sv(100.5f, 0.5f, 0.1f, 1, 0, 1);
        r3d_vtx_t c = sv(0.5f, 100.5f, 0.1f, 1, 0, 1);
        r3d_draw_tri(&t, &a, &b, &c, 3, 0, 0);
    }
    /* innen: gemalt; aussen-im-Dreieck: (60,60) lag im roten Flat-Dreieck -> MUSS rot bleiben */
    int ok_sci = (gui_get(&g, 20, 20) == 0xFF00FFu) && (gui_get(&g, 60, 60) == 0xFF0000u);
    t.sc_x = 0; t.sc_y = 0; t.sc_w = 640; t.sc_h = 480;

    gui_flush_all(&g);

    uwrite("[vktest] r3d: flat=");    uwrite(ok_flat  ? "ok" : "FEHLER");
    uwrite(" gouraud=");              uwrite(ok_gour  ? "ok" : "FEHLER");
    uwrite(" tiefe=");                uwrite(ok_depth ? "ok" : "FEHLER");
    uwrite(" cull=");                 uwrite(ok_cull  ? "ok" : "FEHLER");
    uwrite(" fillrule=");
    if (ok_fill) { uwrite("ok(doppelt=0,luecken=0)"); }
    else {
        uwrite("FEHLER(doppelt=");
        fmt_u(nb, (unsigned long)doppel);  uwrite(nb);
        uwrite(",luecken=");
        fmt_u(nb, (unsigned long)luecken); uwrite(nb);
        uwrite(")");
    }
    uwrite(" nearclip=");             uwrite(ok_clip  ? "ok" : "FEHLER");
    uwrite(" persp=");                uwrite(ok_persp ? "ok" : "FEHLER");
    uwrite(" farclip=");              uwrite(ok_far   ? "ok" : "FEHLER");
    uwrite(" cullfront=");            uwrite(ok_cf    ? "ok" : "FEHLER");
    uwrite(" scissor=");              uwrite(ok_sci   ? "ok" : "FEHLER");
    uwrite("\n");

    /* --- Vulkan-Kern (echte Khronos-API) --- */
    int ok_vk = run_vk_core_tests();
    uwrite(ok_vk ? "[vktest] vk-kern fertig\n" : "[vktest] vk-kern FEHLGESCHLAGEN\n");

    /* --- SPIR-V-Interpreter (Shader von tools/gen_spirv.py, Referenz: Python) --- */
    int ok_spv = run_spirv_tests();
    uwrite(ok_spv ? "[vktest] spirv fertig\n" : "[vktest] spirv FEHLGESCHLAGEN\n");

    /* --- Draw-Pfad -- Dreieck+Tiefentest KOMPLETT durch die Vulkan-API --- */
    int ok_draw = run_vkdraw_tests();
    uwrite(ok_draw ? "[vktest] vkdraw fertig\n" : "[vktest] vkdraw FEHLGESCHLAGEN\n");

    /* --- Demo: 2 interpenetrierende, beleuchtete Wuerfel (Z-Buffer live) --- */
    r3d_mat4 proj, view, vp;
    r3d_mat4_perspective(&proj, 1.0471976f /* 60 Grad */, 640.0f / 480.0f, 0.1f, 10.0f);
    r3d_mat4_translate(&view, 0.0f, 0.0f, -3.6f);
    r3d_mat4_mul(&vp, &proj, &view);

    t.cull_mode = R3D_CULL_BACK;      /* geschlossene Koerper: Back-Faces sparen */
    float ang = 0.0f;
    int frames = 60;                  /* 60 statt 120: haelt das -Vk-Zeitfenster mit VKCUBE-Rendern (Review #30) */
    for (int fr = 0; fr < frames; fr++) {
        gui_clear(&g, BG);
        r3d_depth_clear(g_depth, 640 * 480, 1.0f);

        r3d_mat4 rx, ry, mm;
        r3d_mat4_rot_y(&ry, ang);
        r3d_mat4_rot_x(&rx, ang * 0.7f);
        r3d_mat4_mul(&mm, &ry, &rx);
        draw_cube(&t, &vp, &mm, 1.0f);

        r3d_mat4_rot_y(&ry, -ang * 1.3f);
        r3d_mat4_rot_x(&rx, 0.5f + ang * 0.5f);
        r3d_mat4_mul(&mm, &rx, &ry);
        r3d_mat4 tr;
        r3d_mat4_translate(&tr, 1.05f, 0.0f, 0.0f);
        r3d_mat4_mul(&mm, &tr, &mm);
        draw_cube(&t, &vp, &mm, 0.62f);

        gui_flush_all(&g);
        ang += 0.045f;
        sys1(SYS_SLEEP_MS, 15);
    }
    uwrite("[vktest] demo: ");
    fmt_u(nb, (unsigned long)frames); uwrite(nb);
    uwrite(" frames wuerfel+wuerfel (z-buffer, beleuchtet) gerendert\n");
    uwrite("[vktest] stufe1 fertig\n");

    /* --- Reuse-Guardian (Review-Befund): DRITTE FPTEST-Instanz NACH dem Exit der
     * beiden Boot-Instanzen auf Kern 1 starten. Sie bekommt einen RECYCELTEN Task-Slot,
     * dessen fpctx zuletzt per fpctx_save mit Mustern befuellt wurde -> ihre Start-Probe
     * testet die Zero-Init bei Slot-WIEDERVERWENDUNG (die Boot-Instanzen liefen in
     * frischen Slots, wo .bss ohnehin 0 ist). */
    {
        /* hdd1: EL0-SYS_SPAWN darf nur hdd1/hdd2 (hdd0 = System bleibt EL0-unzugaenglich). */
        long tid3 = sys3(SYS_SPAWN, (long)"hdd1:FPTEST.ELF", 1, 0);
        uwrite(tid3 >= 0
            ? "[vktest] fpctx-reuse: dritte FPTEST-Instanz auf Kern 1 gestartet (recycelter Slot)\n"
            : "[vktest] fpctx-reuse: SPAWN FEHLGESCHLAGEN\n");
    }

    /* --- VKCUBE.ELF starten (echte Vulkan-App: Surface/Swapchain/Present).
     * Erbt unsere Caps (GUI). Slot-Budget: vktest endet gleich -> max. 3 gleichzeitig. */
    {
        long tidc = sys3(SYS_SPAWN, (long)"hdd1:VKCUBE.ELF", 0, 0);
        uwrite(tidc >= 0
            ? "[vktest] vkcube gestartet (Vulkan-Swapchain-Demo auf Kern 0)\n"
            : "[vktest] vkcube: SPAWN FEHLGESCHLAGEN\n");
    }

    int all_ok = ok_flat && ok_gour && ok_depth && ok_cull && ok_fill && ok_clip &&
                 ok_persp && ok_far && ok_cf && ok_sci && ok_vk && ok_spv && ok_draw;
    sys3(SYS_EXIT, all_ok ? 0 : 1, 0, 0);
    for (;;) { }
}
