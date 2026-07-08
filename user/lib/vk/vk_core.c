/*
 * user/lib/vk/vk_core.c  --  Vulkan-1.0-Kern: Instance/PhysicalDevice/Device/Queue/
 * Memory/Buffer/Image/ImageView + ProcAddr. Spec-treues Subset (siehe vk_rtos.h).
 * MIT FP kompiliert (Teil der GUI_FP-App-Welt). Externally-synchronized (Spez) -> lockfrei.
 */
#include "vk_rtos.h"

void *memset(void *dst, int c, unsigned long n);

/* Pool-Reset-Hooks der anderen Uebersetzungseinheiten (vkDestroyDevice ruft sie -> Review #16).
 * Schwach genug entkoppelt: vk_cmd/vk_wsi definieren sie; werden immer mitgelinkt. */
void rt_cmd_reset_pools(void);
void rt_wsi_reset_device(void);

/* ---------------- Zustand ---------------- */
static unsigned char *g_heap_base;
static VkDeviceSize   g_heap_size;
static VkDeviceSize   g_heap_used;
static int            g_live_allocs;

static int g_instance_alive;
static int g_physdev_token;                 /* Handle-Ziel (Adresse zaehlt, Inhalt egal) */
static rt_device_t g_device;

static rt_mem_t    g_mem[RT_MAX_MEMOBJ];
static rt_buffer_t g_buf[RT_MAX_BUFFER];
static rt_image_t  g_img[RT_MAX_IMAGE];
static rt_view_t   g_view[RT_MAX_VIEW];

void vk_rtos_set_heap(void *base, VkDeviceSize bytes)
{
    g_heap_base = (unsigned char *)base;
    g_heap_size = bytes;
    g_heap_used = 0;
    g_live_allocs = 0;
}

/* ---------------- kleine Helfer ---------------- */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void copystr(char *dst, int cap, const char *src)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a)
{
    return (v + a - 1) & ~(a - 1);
}

static int format_supported_color(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM;
}
static int format_supported_depth(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT; }
/* V1.10: Formate, die der Vertex-Input als Attribut lesen kann (roher Byte-Offset). */
static int format_supported_vertex(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32_SFLOAT:       case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM:   case VK_FORMAT_B8G8R8A8_UNORM:
        return 1;
    default: return 0;
    }
}

rt_image_t  *rt_image_from_handle(VkImage h)   { return (rt_image_t *)(void *)h; }
rt_view_t   *rt_view_from_handle(VkImageView h){ return (rt_view_t *)(void *)h; }
rt_buffer_t *rt_buffer_from_handle(VkBuffer h) { return (rt_buffer_t *)(void *)h; }

unsigned char *rt_image_pixels(const rt_image_t *img)
{
    if (!img) { return 0; }
    if (img->ext_pixels) { return img->ext_pixels; }  /* Swapchain: GUI-Backbuffer */
    if (!img->mem) { return 0; }
    return img->mem->base + img->off;
}

rt_image_t *rt_image_alloc_swapchain(VkFormat fmt, unsigned w, unsigned h,
                                     unsigned pitch_bytes, unsigned char *pixels)
{
    int slot = -1;
    for (int i = 0; i < RT_MAX_IMAGE; i++) { if (!g_img[i].used) { slot = i; break; } }
    if (slot < 0) { return 0; }
    rt_image_t *im = &g_img[slot];
    memset(im, 0, sizeof(*im));
    im->used = 1;
    im->format = fmt;
    im->extent.width = w; im->extent.height = h; im->extent.depth = 1;
    im->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    im->row_pitch = pitch_bytes;
    im->bytes = (VkDeviceSize)pitch_bytes * h;
    im->is_swapchain = 1;
    im->ext_pixels = pixels;
    im->samples = 1;                  /* V1.6: Swapchain ist immer Single-Sample */
    return im;
}

/* ---------------- Instance ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *pApiVersion)
{
    if (pApiVersion) { *pApiVersion = VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION); }
    return VK_SUCCESS;
}

/* Instance-Extensions (T3.6): Surface-Basis + rpi_rtos-Plattform-Surface. */
static const struct { const char *name; uint32_t ver; } g_iext[] = {
    { VK_KHR_SURFACE_EXTENSION_NAME, 25 },
    { VK_RTOS_SURFACE_EXTENSION_NAME, 1 },
};
#define RT_NIEXT (sizeof(g_iext) / sizeof(g_iext[0]))

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    /* Spez: fuer eine benannte Schicht die Schicht-Extensions liefern -- wir haben KEINE
     * Schichten -> jeder pLayerName != NULL ist unbekannt -> VK_ERROR_LAYER_NOT_PRESENT. */
    if (pLayerName) { return VK_ERROR_LAYER_NOT_PRESENT; }
    if (!pProperties) { *pPropertyCount = (uint32_t)RT_NIEXT; return VK_SUCCESS; }
    uint32_t n = *pPropertyCount < RT_NIEXT ? *pPropertyCount : (uint32_t)RT_NIEXT;
    for (uint32_t i = 0; i < n; i++) {
        copystr(pProperties[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE, g_iext[i].name);
        pProperties[i].specVersion = g_iext[i].ver;
    }
    *pPropertyCount = n;
    return (n < RT_NIEXT) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
    (void)pProperties;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance)
{
    (void)pAllocator;
    if (!pCreateInfo || !pInstance) { return VK_ERROR_INITIALIZATION_FAILED; }
    if (g_instance_alive) { return VK_ERROR_OUT_OF_HOST_MEMORY; }   /* 1 Instance (Limit) */
    /* Unbekannte Layer/Extensions -> spez-konforme Fehlercodes. */
    for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; i++) {
        (void)pCreateInfo->ppEnabledLayerNames[i];
        return VK_ERROR_LAYER_NOT_PRESENT;                          /* wir haben keine Layer */
    }
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char *e = pCreateInfo->ppEnabledExtensionNames[i];
        int known = 0;
        for (unsigned k = 0; k < RT_NIEXT; k++) { if (streq(e, g_iext[k].name)) { known = 1; } }
        if (!known) { return VK_ERROR_EXTENSION_NOT_PRESENT; }
    }
    g_instance_alive = 1;
    *pInstance = (VkInstance)(void *)&g_instance_alive;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
    (void)instance; (void)pAllocator;
    g_instance_alive = 0;
}

/* ---------------- PhysicalDevice ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices)
{
    (void)instance;
    if (!pPhysicalDevices) { *pPhysicalDeviceCount = 1; return VK_SUCCESS; }
    if (*pPhysicalDeviceCount < 1) { *pPhysicalDeviceCount = 0; return VK_INCOMPLETE; }
    *pPhysicalDeviceCount = 1;
    pPhysicalDevices[0] = (VkPhysicalDevice)(void *)&g_physdev_token;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties)
{
    (void)physicalDevice;
    VkPhysicalDeviceProperties *p = pProperties;
    memset(p, 0, sizeof(*p));
    p->apiVersion    = VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION);
    p->driverVersion = 1;
    p->vendorID      = 0;
    p->deviceID      = 0;
    p->deviceType    = VK_PHYSICAL_DEVICE_TYPE_CPU;
    copystr(p->deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, "rpi_rtos swrast (CPU)");
    /* Ehrliche Limits DIESER Implementierung (Subset; einzelne liegen bewusst unter den
     * Spez-Minima fuer volle Konformitaet -- dokumentiert in docs/architecture/17-3d-plan.md). */
    VkPhysicalDeviceLimits *L = &p->limits;
    L->maxImageDimension1D = 4096;  L->maxImageDimension2D = 4096;
    L->maxImageDimension3D = 1;     L->maxImageDimensionCube = 0;
    L->maxImageArrayLayers = 1;
    L->maxMemoryAllocationCount = RT_MAX_MEMOBJ;
    L->maxSamplerAllocationCount = 0;
    L->bufferImageGranularity = RT_MEM_ALIGN;
    L->maxBoundDescriptorSets = 1;
    L->maxPerStageDescriptorUniformBuffers = 1;
    L->maxPerStageResources = 4;
    L->maxDescriptorSetUniformBuffers = 1;
    L->maxVertexInputAttributes = 8;  L->maxVertexInputBindings = 4;
    L->maxVertexInputAttributeOffset = 2047; L->maxVertexInputBindingStride = 2048;
    L->maxVertexOutputComponents = 32;
    L->maxFragmentInputComponents = 32;
    L->maxFragmentOutputAttachments = 1;
    L->maxComputeWorkGroupCount[0] = 0;      /* kein Compute (Subset) */
    L->maxDrawIndexedIndexValue = 0xFFFFFFu;
    L->maxDrawIndirectCount = 0;
    L->maxSamplerLodBias = 0.0f; L->maxSamplerAnisotropy = 1.0f;
    L->maxViewports = 1;
    L->maxViewportDimensions[0] = 4096; L->maxViewportDimensions[1] = 4096;
    L->viewportBoundsRange[0] = -8192.0f; L->viewportBoundsRange[1] = 8191.0f;
    L->viewportSubPixelBits = 4;                       /* 28.4-Rasterung */
    L->minMemoryMapAlignment = RT_MEM_ALIGN;
    L->minTexelBufferOffsetAlignment = RT_MEM_ALIGN;
    L->minUniformBufferOffsetAlignment = RT_MEM_ALIGN;
    L->minStorageBufferOffsetAlignment = RT_MEM_ALIGN;
    L->maxFramebufferWidth = 4096; L->maxFramebufferHeight = 4096; L->maxFramebufferLayers = 1;
    /* V1.6 MSAA: Framebuffer-Attachments unterstuetzen 1x und 4x. */
    L->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    L->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    L->maxColorAttachments = 8;                        /* V1.10/V1.5: MRT bis 8 (1.4-Pflicht) */
    L->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    L->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    L->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    L->maxPushConstantsSize = 256;                     /* V1.10: 1.4-Pflicht (war 128) */
    L->maxMemoryAllocationCount = RT_MAX_MEMOBJ;
    L->subPixelPrecisionBits = 4; L->subTexelPrecisionBits = 4; L->mipmapPrecisionBits = 4;
    L->maxClipDistances = 0; L->maxCullDistances = 0; L->maxCombinedClipAndCullDistances = 0;
    L->discreteQueuePriorities = 1;
    L->pointSizeRange[0] = 1.0f; L->pointSizeRange[1] = 1.0f;
    L->lineWidthRange[0] = 1.0f; L->lineWidthRange[1] = 1.0f;
    L->pointSizeGranularity = 0.0f; L->lineWidthGranularity = 0.0f;
    L->strictLines = VK_TRUE;
    L->standardSampleLocations = VK_TRUE;
    L->optimalBufferCopyOffsetAlignment = RT_MEM_ALIGN;
    L->optimalBufferCopyRowPitchAlignment = 4;
    L->nonCoherentAtomSize = RT_MEM_ALIGN;
    L->timestampComputeAndGraphics = VK_TRUE; L->timestampPeriod = 1.0f;   /* V1.8: 1 Tick = 1 ns */
    /* sparseProperties bleiben 0 (kein Sparse). */
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures)
{
    (void)physicalDevice;
    memset(pFeatures, 0, sizeof(*pFeatures));
    /* robustBufferAccess: der SPIR-V-Interpreter (T3.4) klemmt Out-of-Bounds-Zugriffe. */
    pFeatures->robustBufferAccess = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount,
    VkQueueFamilyProperties *pQueueFamilyProperties)
{
    (void)physicalDevice;
    if (!pQueueFamilyProperties) { *pQueueFamilyPropertyCount = 1; return; }
    if (*pQueueFamilyPropertyCount < 1) { return; }
    *pQueueFamilyPropertyCount = 1;
    VkQueueFamilyProperties *q = &pQueueFamilyProperties[0];
    memset(q, 0, sizeof(*q));
    q->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
    q->queueCount = 1;
    q->timestampValidBits = 64;                       /* V1.8: vkCmdWriteTimestamp voll gueltig */
    q->minImageTransferGranularity.width  = 1;
    q->minImageTransferGranularity.height = 1;
    q->minImageTransferGranularity.depth  = 1;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
    (void)physicalDevice;
    VkPhysicalDeviceMemoryProperties *m = pMemoryProperties;
    memset(m, 0, sizeof(*m));
    m->memoryTypeCount = 1;
    m->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m->memoryTypes[0].heapIndex = 0;
    m->memoryHeapCount = 1;
    m->memoryHeaps[0].size  = g_heap_size;
    m->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties)
{
    (void)physicalDevice;
    memset(pFormatProperties, 0, sizeof(*pFormatProperties));
    if (format_supported_color(format)) {
        /* V1.10: ehrlich alle Fähigkeiten, die die Runtime für Farb-Formate erbringt:
         * Farb-Attachment (V1), Blending (V1.1), Sampling (V1.4), Transfer (V1.9). */
        VkFormatFeatureFlags f = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                 VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                 VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                 VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        pFormatProperties->linearTilingFeatures  = f;
        pFormatProperties->optimalTilingFeatures = f;     /* optimal == linear intern */
    } else if (format_supported_depth(format)) {
        VkFormatFeatureFlags f = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                 VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        pFormatProperties->linearTilingFeatures  = f;
        pFormatProperties->optimalTilingFeatures = f;
    }
    /* V1.10: Vertex-Attribut-Formate melden bufferFeatures unabhängig vom Tiling. */
    if (format_supported_vertex(format)) {
        pFormatProperties->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *pImageFormatProperties)
{
    (void)physicalDevice; (void)tiling; (void)usage; (void)flags;
    if (type != VK_IMAGE_TYPE_2D ||
        (!format_supported_color(format) && !format_supported_depth(format))) {
        memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    pImageFormatProperties->maxExtent.width  = 4096;
    pImageFormatProperties->maxExtent.height = 4096;
    pImageFormatProperties->maxExtent.depth  = 1;
    pImageFormatProperties->maxMipLevels     = 1;      /* Subset: keine Mipmaps */
    pImageFormatProperties->maxArrayLayers   = 1;
    pImageFormatProperties->sampleCounts     = VK_SAMPLE_COUNT_1_BIT;
    pImageFormatProperties->maxResourceSize  = g_heap_size ? g_heap_size : 1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type,
    VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling,
    uint32_t *pPropertyCount, VkSparseImageFormatProperties *pProperties)
{
    (void)physicalDevice; (void)format; (void)type; (void)samples; (void)usage; (void)tiling;
    (void)pProperties;
    *pPropertyCount = 0;                               /* kein Sparse */
}

/* ---------------- Device + Queue ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    (void)physicalDevice;
    if (pLayerName) { return VK_ERROR_LAYER_NOT_PRESENT; }   /* keine Schichten (Spez) */
    if (!pProperties) { *pPropertyCount = 1; return VK_SUCCESS; }
    if (*pPropertyCount < 1) { *pPropertyCount = 0; return VK_INCOMPLETE; }
    copystr(pProperties[0].extensionName, VK_MAX_EXTENSION_NAME_SIZE,
            VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    pProperties[0].specVersion = 70;
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
    (void)physicalDevice; (void)pProperties;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
    (void)physicalDevice; (void)pAllocator;
    if (!pCreateInfo || !pDevice) { return VK_ERROR_INITIALIZATION_FAILED; }
    if (g_device.alive) { return VK_ERROR_OUT_OF_HOST_MEMORY; }     /* 1 Device (Limit) */
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char *e = pCreateInfo->ppEnabledExtensionNames[i];
        if (!streq(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    if (pCreateInfo->pEnabledFeatures) {
        /* Nur robustBufferAccess ist vorhanden; jedes andere angeforderte Feature ->
         * VK_ERROR_FEATURE_NOT_PRESENT (Spez-Verhalten, Negativ-Testpfad). */
        const VkBool32 *f = (const VkBool32 *)pCreateInfo->pEnabledFeatures;
        uint32_t nfeat = (uint32_t)(sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32));
        uint32_t robust_idx = 0;                       /* robustBufferAccess ist Feld 0 */
        for (uint32_t i = 0; i < nfeat; i++) {
            if (f[i] && i != robust_idx) { return VK_ERROR_FEATURE_NOT_PRESENT; }
        }
    }
    if (pCreateInfo->queueCreateInfoCount != 1 ||
        pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex != 0 ||
        pCreateInfo->pQueueCreateInfos[0].queueCount != 1) {
        return VK_ERROR_INITIALIZATION_FAILED;         /* Subset: genau 1 Queue in Familie 0 */
    }
    g_device.alive = 1;
    g_device.queue.alive = 1;
    *pDevice = (VkDevice)(void *)&g_device;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    g_device.alive = 0;
    g_device.queue.alive = 0;
    /* Objekt-Pools zuruecksetzen (Device-Lebenszeit endet) -- inkl. der vk_cmd-/vk_wsi-Pools,
     * damit kein Handle die Device-Zerstoerung ueberlebt (Review #16). */
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_buf, 0, sizeof(g_buf));
    memset(g_img, 0, sizeof(g_img));
    memset(g_view, 0, sizeof(g_view));
    g_heap_used = 0; g_live_allocs = 0;
    rt_cmd_reset_pools();
    rt_wsi_reset_device();
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue)
{
    (void)queueFamilyIndex; (void)queueIndex;
    rt_device_t *d = (rt_device_t *)(void *)device;
    *pQueue = (VkQueue)(void *)&d->queue;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
    (void)queue;                                        /* Submit ist synchron (T3.5) */
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
    (void)device;
    return VK_SUCCESS;
}

/* ---------------- Memory ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
    (void)device; (void)pAllocator;
    if (!pAllocateInfo || !pMemory) { return VK_ERROR_INITIALIZATION_FAILED; }
    if (pAllocateInfo->memoryTypeIndex != 0) { return VK_ERROR_INITIALIZATION_FAILED; }
    VkDeviceSize need = align_up(pAllocateInfo->allocationSize, RT_MEM_ALIGN);
    if (!g_heap_base || need > g_heap_size - g_heap_used) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;           /* ehrlich: Arena erschoepft/fehlt */
    }
    int slot = -1;
    for (int i = 0; i < RT_MAX_MEMOBJ; i++) { if (!g_mem[i].used) { slot = i; break; } }
    if (slot < 0) { return VK_ERROR_OUT_OF_HOST_MEMORY; }  /* maxMemoryAllocationCount */
    g_mem[slot].used = 1;
    g_mem[slot].base = g_heap_base + g_heap_used;
    g_mem[slot].size = pAllocateInfo->allocationSize;
    g_heap_used += need;
    g_live_allocs++;
    *pMemory = (VkDeviceMemory)(void *)&g_mem[slot];
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
    VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (!memory) { return; }
    rt_mem_t *m = (rt_mem_t *)(void *)memory;
    if (m->used) {
        m->used = 0;
        if (--g_live_allocs == 0) { g_heap_used = 0; }  /* Bump-Arena: Reset wenn alles frei */
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    VkMemoryMapFlags flags, void **ppData)
{
    (void)device; (void)size; (void)flags;
    rt_mem_t *m = (rt_mem_t *)(void *)memory;
    if (!m || !m->used || offset > m->size) { return VK_ERROR_MEMORY_MAP_FAILED; }
    *ppData = m->base + offset;                         /* HOST_VISIBLE|COHERENT, immer gemappt */
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    (void)device; (void)memory;
}

/* V3.4: Core-1.4 vkMapMemory2/vkUnmapMemory2 -- Info-Structs auf die 1.0-Varianten uebersetzt. */
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory2(
    VkDevice device, const VkMemoryMapInfo *pInfo, void **ppData)
{
    if (!pInfo) { return VK_ERROR_MEMORY_MAP_FAILED; }
    return vkMapMemory(device, pInfo->memory, pInfo->offset, pInfo->size, pInfo->flags, ppData);
}
VKAPI_ATTR VkResult VKAPI_CALL vkUnmapMemory2(
    VkDevice device, const VkMemoryUnmapInfo *pInfo)
{
    if (pInfo) { vkUnmapMemory(device, pInfo->memory); }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(
    VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    return VK_SUCCESS;                                  /* HOST_COHERENT -> No-op */
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(
    VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    return VK_SUCCESS;
}

/* ---------------- Buffer ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(
    VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pBuffer) { return VK_ERROR_INITIALIZATION_FAILED; }
    int slot = -1;
    for (int i = 0; i < RT_MAX_BUFFER; i++) { if (!g_buf[i].used) { slot = i; break; } }
    if (slot < 0) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    g_buf[slot].used  = 1;
    g_buf[slot].size  = pCreateInfo->size;
    g_buf[slot].usage = pCreateInfo->usage;
    g_buf[slot].mem   = 0;
    g_buf[slot].off   = 0;
    *pBuffer = (VkBuffer)(void *)&g_buf[slot];
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (buffer) { ((rt_buffer_t *)(void *)buffer)->used = 0; }
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements)
{
    (void)device;
    rt_buffer_t *b = (rt_buffer_t *)(void *)buffer;
    pMemoryRequirements->size           = align_up(b->size, RT_MEM_ALIGN);
    pMemoryRequirements->alignment      = RT_MEM_ALIGN;
    pMemoryRequirements->memoryTypeBits = 1u;           /* Typ 0 */
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
    (void)device;
    rt_buffer_t *b = (rt_buffer_t *)(void *)buffer;
    rt_mem_t    *m = (rt_mem_t *)(void *)memory;
    if (!b || !m || !m->used) { return VK_ERROR_INITIALIZATION_FAILED; }
    b->mem = m;
    b->off = memoryOffset;
    return VK_SUCCESS;
}

/* V3: Core-1.2 vkGetBufferDeviceAddress -- CPU-Implementierung: die „GPU-Adresse" IST die reale
 * Host-Adresse des Puffer-Backings (mem->base + off). Ehrliche, stabile, nicht-null Adresse. */
VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(
    VkDevice device, const VkBufferDeviceAddressInfo *pInfo)
{
    (void)device;
    if (!pInfo) { return 0; }
    rt_buffer_t *b = (rt_buffer_t *)(void *)pInfo->buffer;
    if (!b || !b->mem) { return 0; }
    return (VkDeviceAddress)(unsigned long)(void *)(b->mem->base + b->off);
}
VKAPI_ATTR uint64_t VKAPI_CALL vkGetBufferOpaqueCaptureAddress(
    VkDevice device, const VkBufferDeviceAddressInfo *pInfo)
{
    return (uint64_t)vkGetBufferDeviceAddress(device, pInfo);
}
VKAPI_ATTR uint64_t VKAPI_CALL vkGetDeviceMemoryOpaqueCaptureAddress(
    VkDevice device, const VkDeviceMemoryOpaqueCaptureAddressInfo *pInfo)
{
    (void)device;
    if (!pInfo) { return 0; }
    rt_mem_t *m = (rt_mem_t *)(void *)pInfo->memory;
    return (m && m->used) ? (uint64_t)(unsigned long)(void *)m->base : 0;
}

/* ---------------- Image + ImageView ---------------- */
static VkDeviceSize image_row_pitch(VkFormat f, uint32_t w)
{
    (void)f;                                            /* beide Formate: 4 B/Pixel */
    return (VkDeviceSize)w * 4u;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pImage) { return VK_ERROR_INITIALIZATION_FAILED; }
    /* V1.6 MSAA: 1 oder 4 Samples erlaubt (Subset). */
    if (pCreateInfo->imageType != VK_IMAGE_TYPE_2D ||
        pCreateInfo->mipLevels != 1 || pCreateInfo->arrayLayers != 1 ||
        (pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT &&
         pCreateInfo->samples != VK_SAMPLE_COUNT_4_BIT) ||
        pCreateInfo->extent.width == 0 || pCreateInfo->extent.width > 4096 ||
        pCreateInfo->extent.height == 0 || pCreateInfo->extent.height > 4096 ||
        pCreateInfo->extent.depth != 1 ||
        (!format_supported_color(pCreateInfo->format) &&
         !format_supported_depth(pCreateInfo->format))) {
        return VK_ERROR_FEATURE_NOT_PRESENT;            /* ausserhalb des Subsets */
    }
    int slot = -1;
    for (int i = 0; i < RT_MAX_IMAGE; i++) { if (!g_img[i].used) { slot = i; break; } }
    if (slot < 0) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    rt_image_t *im = &g_img[slot];
    im->used     = 1;
    im->format   = pCreateInfo->format;
    im->extent   = pCreateInfo->extent;
    im->usage    = pCreateInfo->usage;
    im->samples  = (pCreateInfo->samples == VK_SAMPLE_COUNT_4_BIT) ? 4 : 1;   /* V1.6 */
    im->row_pitch = image_row_pitch(pCreateInfo->format, pCreateInfo->extent.width);
    /* MSAA: SAMPLE-MAJOR -> samples× Speicher (jede Sample-Ebene ist ein volles Bild). */
    im->bytes    = im->row_pitch * pCreateInfo->extent.height * (VkDeviceSize)im->samples;
    im->mem      = 0;
    im->off      = 0;
    im->is_swapchain = 0;
    im->ext_pixels   = 0;              /* wichtig bei Slot-Reuse nach einem Swapchain-Image */
    *pImage = (VkImage)(void *)im;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (image) { ((rt_image_t *)(void *)image)->used = 0; }
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements *pMemoryRequirements)
{
    (void)device;
    rt_image_t *im = (rt_image_t *)(void *)image;
    pMemoryRequirements->size           = align_up(im->bytes, RT_MEM_ALIGN);
    pMemoryRequirements->alignment      = RT_MEM_ALIGN;
    pMemoryRequirements->memoryTypeBits = 1u;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(
    VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
    (void)device;
    rt_image_t *im = (rt_image_t *)(void *)image;
    rt_mem_t   *m  = (rt_mem_t *)(void *)memory;
    if (!im || !m || !m->used) { return VK_ERROR_INITIALIZATION_FAILED; }
    im->mem = m;
    im->off = memoryOffset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(
    VkDevice device, VkImage image, const VkImageSubresource *pSubresource,
    VkSubresourceLayout *pLayout)
{
    (void)device; (void)pSubresource;
    rt_image_t *im = (rt_image_t *)(void *)image;
    pLayout->offset     = 0;
    pLayout->size       = im->bytes;
    pLayout->rowPitch   = im->row_pitch;
    pLayout->arrayPitch = im->bytes;
    pLayout->depthPitch = im->bytes;
}

/* V3.4: Core-1.4 vkGetImageSubresourceLayout2 -- Info-/Ergebnis-Structs auf die 1.0-Variante. */
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2(
    VkDevice device, VkImage image, const VkImageSubresource2 *pSubresource,
    VkSubresourceLayout2 *pLayout)
{
    if (pSubresource && pLayout) {
        vkGetImageSubresourceLayout(device, image, &pSubresource->imageSubresource,
                                    &pLayout->subresourceLayout);
    }
}
/* V3.4: Core-1.4 vkGetDeviceImageSubresourceLayout -- Layout DIREKT aus dem Create-Info (objektlos),
 * gleiche Groessenlogik wie das objektbasierte Layout (row_pitch = w*4, size = pitch*h*samples). */
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSubresourceLayout(
    VkDevice device, const VkDeviceImageSubresourceInfo *pInfo, VkSubresourceLayout2 *pLayout)
{
    (void)device;
    if (!pInfo || !pInfo->pCreateInfo || !pLayout) { return; }
    const VkImageCreateInfo *ci = pInfo->pCreateInfo;
    unsigned samples = (ci->samples == VK_SAMPLE_COUNT_4_BIT) ? 4u : 1u;
    VkDeviceSize rp = image_row_pitch(ci->format, ci->extent.width);
    VkDeviceSize bytes = rp * ci->extent.height * (VkDeviceSize)samples;
    pLayout->subresourceLayout.offset     = 0;
    pLayout->subresourceLayout.size       = bytes;
    pLayout->subresourceLayout.rowPitch   = rp;
    pLayout->subresourceLayout.arrayPitch = bytes;
    pLayout->subresourceLayout.depthPitch = bytes;
}
/* V3.4: Core-1.4 vkGetRenderingAreaGranularity -- Subset: pixelgenau (1x1). */
VKAPI_ATTR void VKAPI_CALL vkGetRenderingAreaGranularity(
    VkDevice device, const VkRenderingAreaInfo *pRenderingAreaInfo, VkExtent2D *pGranularity)
{
    (void)device; (void)pRenderingAreaInfo;
    if (pGranularity) { pGranularity->width = 1; pGranularity->height = 1; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pView) { return VK_ERROR_INITIALIZATION_FAILED; }
    int slot = -1;
    for (int i = 0; i < RT_MAX_VIEW; i++) { if (!g_view[i].used) { slot = i; break; } }
    if (slot < 0) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    g_view[slot].used   = 1;
    g_view[slot].image  = (rt_image_t *)(void *)pCreateInfo->image;
    g_view[slot].format = pCreateInfo->format;
    *pView = (VkImageView)(void *)&g_view[slot];
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
    VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (imageView) { ((rt_view_t *)(void *)imageView)->used = 0; }
}

/* ---------------- Core 1.1: promovierte "2"-Varianten (V3) ----------------
 * Ehrliche Wrapper der 1.0-Basisabfragen: die eingebettete Basis-Struktur wird gefuellt,
 * pNext-Ketten (nicht unterstuetzte Feature-/Property-Erweiterungen) bleiben unberuehrt
 * (die App nullt sie vor dem Aufruf -> nicht unterstuetzte Felder lesen als 0/FALSE).
 * Auch via VK_KHR_get_physical_device_properties2 / bind_memory2 nutzbar. */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *pFeatures)
{
    if (!pFeatures) { return; }
    vkGetPhysicalDeviceFeatures(pd, &pFeatures->features);
    /* V3: die promovierten 1.2/1.3-Features EHRLICH melden -- TRUE nur fuer das, was voll
     * implementiert ist (Timeline-Semaphore, Synchronization2, DrawIndirectCount); alles andere
     * bleibt auf dem vom Aufrufer genullten FALSE (bufferDeviceAddress/dynamicRendering nur
     * teilweise -> nicht gemeldet). Sowohl die aggregierten Vulkan1x-Structs als auch die
     * einzelnen Feature-Structs in der pNext-Kette werden bedient. */
    VkBaseOutStructure *p = (VkBaseOutStructure *)pFeatures->pNext;
    while (p) {
        switch (p->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            VkPhysicalDeviceVulkan12Features *f = (VkPhysicalDeviceVulkan12Features *)(void *)p;
            f->timelineSemaphore = VK_TRUE;
            f->drawIndirectCount = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
            VkPhysicalDeviceVulkan13Features *f = (VkPhysicalDeviceVulkan13Features *)(void *)p;
            f->synchronization2 = VK_TRUE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            ((VkPhysicalDeviceTimelineSemaphoreFeatures *)(void *)p)->timelineSemaphore = VK_TRUE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            ((VkPhysicalDeviceSynchronization2Features *)(void *)p)->synchronization2 = VK_TRUE;
            break;
        default: break;
        }
        p = p->pNext;
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *pProperties)
{
    if (!pProperties) { return; }
    vkGetPhysicalDeviceProperties(pd, &pProperties->properties);
    /* V6: Treiber-/Konformitaets-Identifikation (VkPhysicalDeviceDriverProperties) ueber pNext.
     * conformanceVersion EHRLICH {0,0,0,0}: dieser Software-Treiber hat die Khronos-VK-GL-CTS
     * NICHT durchlaufen (Block V6 = externes CTS-Paket + reale HW, nicht in dieser Umgebung). */
    VkBaseOutStructure *p = (VkBaseOutStructure *)pProperties->pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *)(void *)p;
            d->driverID = VK_DRIVER_ID_MESA_LLVMPIPE;   /* naechstgelegen: reiner CPU-Software-Rasterizer */
            copystr(d->driverName, VK_MAX_DRIVER_NAME_SIZE, "rpi_rtos swvk");
            copystr(d->driverInfo, VK_MAX_DRIVER_INFO_SIZE, "software vulkan 1.4 subset (CPU)");
            d->conformanceVersion.major = 0; d->conformanceVersion.minor = 0;
            d->conformanceVersion.subminor = 0; d->conformanceVersion.patch = 0;
        }
        p = p->pNext;
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *pFormatProperties)
{
    if (pFormatProperties) {
        vkGetPhysicalDeviceFormatProperties(pd, format, &pFormatProperties->formatProperties);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *pInfo,
    VkImageFormatProperties2 *pProps)
{
    if (!pInfo || !pProps) { return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    return vkGetPhysicalDeviceImageFormatProperties(pd, pInfo->format, pInfo->type,
        pInfo->tiling, pInfo->usage, pInfo->flags, &pProps->imageFormatProperties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice pd, uint32_t *pCount, VkQueueFamilyProperties2 *pProps)
{
    if (!pProps) { vkGetPhysicalDeviceQueueFamilyProperties(pd, pCount, 0); return; }
    if (*pCount < 1) { return; }
    VkQueueFamilyProperties base; uint32_t n = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, &base);
    pProps[0].queueFamilyProperties = base;
    *pCount = 1;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *pProps)
{
    if (pProps) { vkGetPhysicalDeviceMemoryProperties(pd, &pProps->memoryProperties); }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *pInfo,
    uint32_t *pCount, VkSparseImageFormatProperties2 *pProps)
{
    (void)pd; (void)pInfo; (void)pProps;
    if (pCount) { *pCount = 0; }                        /* kein Sparse (Subset) */
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *pInfo, VkMemoryRequirements2 *pReq)
{
    if (pInfo && pReq) { vkGetBufferMemoryRequirements(device, pInfo->buffer, &pReq->memoryRequirements); }
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo, VkMemoryRequirements2 *pReq)
{
    if (pInfo && pReq) { vkGetImageMemoryRequirements(device, pInfo->image, &pReq->memoryRequirements); }
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(
    VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos)
{
    for (uint32_t i = 0; i < bindInfoCount; i++) {
        VkResult r = vkBindBufferMemory(device, pBindInfos[i].buffer,
                                        pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) { return r; }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(
    VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo *pBindInfos)
{
    for (uint32_t i = 0; i < bindInfoCount; i++) {
        VkResult r = vkBindImageMemory(device, pBindInfos[i].image,
                                       pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) { return r; }
    }
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue)
{
    if (pQueueInfo) { vkGetDeviceQueue(device, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex, pQueue); }
}

/* V3: Core-1.3 vkGetPhysicalDeviceToolProperties -- keine aktiven Tools (Layers/Debugger). */
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceToolProperties(
    VkPhysicalDevice pd, uint32_t *pToolCount, VkPhysicalDeviceToolProperties *pToolProperties)
{
    (void)pd; (void)pToolProperties;
    if (pToolCount) { *pToolCount = 0; }
    return VK_SUCCESS;
}

/* V3: Core-1.3 Private-Data-Slots -- pro Slot eine kleine (objectHandle -> uint64)-Tabelle. */
typedef struct { unsigned long long handle; unsigned long long value; } rt_pd_entry_t;
typedef struct { int used; int n; rt_pd_entry_t ents[16]; } rt_pdslot_t;
static rt_pdslot_t g_pdslot[4];

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePrivateDataSlot(
    VkDevice device, const VkPrivateDataSlotCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPrivateDataSlot *pPrivateDataSlot)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    if (!pPrivateDataSlot) { return VK_ERROR_INITIALIZATION_FAILED; }
    for (int i = 0; i < 4; i++) {
        if (!g_pdslot[i].used) {
            g_pdslot[i].used = 1; g_pdslot[i].n = 0;
            *pPrivateDataSlot = (VkPrivateDataSlot)(void *)&g_pdslot[i];
            return VK_SUCCESS;
        }
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPrivateDataSlot(
    VkDevice device, VkPrivateDataSlot privateDataSlot, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (privateDataSlot) { ((rt_pdslot_t *)(void *)privateDataSlot)->used = 0; }
}
VKAPI_ATTR VkResult VKAPI_CALL vkSetPrivateData(
    VkDevice device, VkObjectType objectType, uint64_t objectHandle,
    VkPrivateDataSlot privateDataSlot, uint64_t data)
{
    (void)device; (void)objectType;
    rt_pdslot_t *s = (rt_pdslot_t *)(void *)privateDataSlot;
    if (!s || !s->used) { return VK_ERROR_INITIALIZATION_FAILED; }
    for (int i = 0; i < s->n; i++) {
        if (s->ents[i].handle == objectHandle) { s->ents[i].value = data; return VK_SUCCESS; }
    }
    if (s->n < 16) { s->ents[s->n].handle = objectHandle; s->ents[s->n].value = data; s->n++; return VK_SUCCESS; }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
VKAPI_ATTR void VKAPI_CALL vkGetPrivateData(
    VkDevice device, VkObjectType objectType, uint64_t objectHandle,
    VkPrivateDataSlot privateDataSlot, uint64_t *pData)
{
    (void)device; (void)objectType;
    if (!pData) { return; }
    *pData = 0;                                         /* Spez: 0, falls nichts gesetzt */
    rt_pdslot_t *s = (rt_pdslot_t *)(void *)privateDataSlot;
    if (!s || !s->used) { return; }
    for (int i = 0; i < s->n; i++) {
        if (s->ents[i].handle == objectHandle) { *pData = s->ents[i].value; return; }
    }
}

/* ---------------- Core 1.3: objektlose Mem-Requirements (V3) ----------------
 * Berechnen den Speicherbedarf DIREKT aus dem Create-Info (ohne vorher ein Objekt zu
 * erzeugen); identische Groessenlogik wie die objektbasierten 1.0-Abfragen. */
VKAPI_ATTR void VKAPI_CALL vkGetDeviceBufferMemoryRequirements(
    VkDevice device, const VkDeviceBufferMemoryRequirements *pInfo, VkMemoryRequirements2 *pReq)
{
    (void)device;
    if (!pInfo || !pInfo->pCreateInfo || !pReq) { return; }
    pReq->memoryRequirements.size           = align_up(pInfo->pCreateInfo->size, RT_MEM_ALIGN);
    pReq->memoryRequirements.alignment      = RT_MEM_ALIGN;
    pReq->memoryRequirements.memoryTypeBits = 1u;
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements *pInfo, VkMemoryRequirements2 *pReq)
{
    (void)device;
    if (!pInfo || !pInfo->pCreateInfo || !pReq) { return; }
    const VkImageCreateInfo *ci = pInfo->pCreateInfo;
    unsigned samples = (ci->samples == VK_SAMPLE_COUNT_4_BIT) ? 4u : 1u;
    VkDeviceSize bytes = image_row_pitch(ci->format, ci->extent.width) *
                         ci->extent.height * (VkDeviceSize)samples;
    pReq->memoryRequirements.size           = align_up(bytes, RT_MEM_ALIGN);
    pReq->memoryRequirements.alignment      = RT_MEM_ALIGN;
    pReq->memoryRequirements.memoryTypeBits = 1u;
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSparseMemoryRequirements(
    VkDevice device, const VkDeviceImageMemoryRequirements *pInfo,
    uint32_t *pCount, VkSparseImageMemoryRequirements2 *pReq)
{
    (void)device; (void)pInfo; (void)pReq;
    if (pCount) { *pCount = 0; }                        /* kein Sparse (Subset) */
}

/* ---------------- Core 1.1: Device-Groups + External-Properties (V3) ----------------
 * Ein-Geraet-System: eine Geraetegruppe mit unserem einen PhysicalDevice; kein Import/Export
 * externer Handles (alle External-Features 0 -> ehrlich „nicht unterstuetzt"). */
VKAPI_ATTR void VKAPI_CALL vkTrimCommandPool(
    VkDevice device, VkCommandPool commandPool, VkCommandPoolTrimFlags flags)
{
    (void)device; (void)commandPool; (void)flags;      /* Bump-Pools trimmen nicht -> No-op */
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceGroupPeerMemoryFeatures(
    VkDevice device, uint32_t heapIndex, uint32_t localDeviceIndex,
    uint32_t remoteDeviceIndex, VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
    (void)device; (void)heapIndex; (void)localDeviceIndex; (void)remoteDeviceIndex;
    if (pPeerMemoryFeatures) {                          /* Geraet zu sich selbst: alle vier Features */
        *pPeerMemoryFeatures = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT | VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
                               VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT | VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
    }
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
    VkInstance instance, uint32_t *pCount, VkPhysicalDeviceGroupProperties *pProps)
{
    if (!pProps) { if (pCount) { *pCount = 1; } return VK_SUCCESS; }
    if (*pCount < 1) { *pCount = 0; return VK_INCOMPLETE; }
    uint32_t nd = 1; VkPhysicalDevice pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &nd, &pd);
    pProps[0].physicalDeviceCount = 1;
    pProps[0].physicalDevices[0]  = pd;
    pProps[0].subsetAllocation    = VK_FALSE;
    *pCount = 1;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *pInfo,
    VkExternalBufferProperties *pProps)
{
    (void)pd; (void)pInfo;
    if (pProps) {                                       /* kein External-Memory-Support */
        pProps->externalMemoryProperties.externalMemoryFeatures         = 0;
        pProps->externalMemoryProperties.exportFromImportedHandleTypes  = 0;
        pProps->externalMemoryProperties.compatibleHandleTypes          = 0;
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalFenceInfo *pInfo,
    VkExternalFenceProperties *pProps)
{
    (void)pd; (void)pInfo;
    if (pProps) {
        pProps->exportFromImportedHandleTypes = 0;
        pProps->compatibleHandleTypes         = 0;
        pProps->externalFenceFeatures         = 0;
    }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *pInfo,
    VkExternalSemaphoreProperties *pProps)
{
    (void)pd; (void)pInfo;
    if (pProps) {
        pProps->exportFromImportedHandleTypes = 0;
        pProps->compatibleHandleTypes         = 0;
        pProps->externalSemaphoreFeatures     = 0;
    }
}

/* ---------------- ProcAddr ---------------- */
typedef struct { const char *name; PFN_vkVoidFunction fn; } rt_entry_t;

#define RT_ENTRY(fn) { #fn, (PFN_vkVoidFunction)fn }
static const rt_entry_t g_entries[] = {
    RT_ENTRY(vkEnumerateInstanceVersion),
    RT_ENTRY(vkEnumerateInstanceExtensionProperties),
    RT_ENTRY(vkEnumerateInstanceLayerProperties),
    RT_ENTRY(vkCreateInstance), RT_ENTRY(vkDestroyInstance),
    RT_ENTRY(vkEnumeratePhysicalDevices),
    RT_ENTRY(vkGetPhysicalDeviceProperties), RT_ENTRY(vkGetPhysicalDeviceFeatures),
    RT_ENTRY(vkGetPhysicalDeviceQueueFamilyProperties),
    RT_ENTRY(vkGetPhysicalDeviceMemoryProperties),
    RT_ENTRY(vkGetPhysicalDeviceFormatProperties),
    RT_ENTRY(vkGetPhysicalDeviceImageFormatProperties),
    RT_ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties),
    RT_ENTRY(vkEnumerateDeviceExtensionProperties), RT_ENTRY(vkEnumerateDeviceLayerProperties),
    RT_ENTRY(vkCreateDevice), RT_ENTRY(vkDestroyDevice),
    RT_ENTRY(vkGetDeviceQueue), RT_ENTRY(vkQueueWaitIdle), RT_ENTRY(vkDeviceWaitIdle),
    RT_ENTRY(vkAllocateMemory), RT_ENTRY(vkFreeMemory),
    RT_ENTRY(vkMapMemory), RT_ENTRY(vkUnmapMemory),
    RT_ENTRY(vkFlushMappedMemoryRanges), RT_ENTRY(vkInvalidateMappedMemoryRanges),
    RT_ENTRY(vkCreateBuffer), RT_ENTRY(vkDestroyBuffer),
    RT_ENTRY(vkGetBufferMemoryRequirements), RT_ENTRY(vkBindBufferMemory),
    RT_ENTRY(vkCreateImage), RT_ENTRY(vkDestroyImage),
    RT_ENTRY(vkGetImageMemoryRequirements), RT_ENTRY(vkBindImageMemory),
    RT_ENTRY(vkGetImageSubresourceLayout),
    RT_ENTRY(vkCreateImageView), RT_ENTRY(vkDestroyImageView),
    /* V3: Core-1.1 promovierte "2"-Varianten */
    RT_ENTRY(vkGetPhysicalDeviceFeatures2), RT_ENTRY(vkGetPhysicalDeviceProperties2),
    RT_ENTRY(vkGetPhysicalDeviceFormatProperties2), RT_ENTRY(vkGetPhysicalDeviceImageFormatProperties2),
    RT_ENTRY(vkGetPhysicalDeviceQueueFamilyProperties2), RT_ENTRY(vkGetPhysicalDeviceMemoryProperties2),
    RT_ENTRY(vkGetPhysicalDeviceSparseImageFormatProperties2),
    RT_ENTRY(vkGetBufferMemoryRequirements2), RT_ENTRY(vkGetImageMemoryRequirements2),
    RT_ENTRY(vkBindBufferMemory2), RT_ENTRY(vkBindImageMemory2), RT_ENTRY(vkGetDeviceQueue2),
    /* V3: Core-1.3 objektlose Mem-Requirements */
    RT_ENTRY(vkGetDeviceBufferMemoryRequirements), RT_ENTRY(vkGetDeviceImageMemoryRequirements),
    RT_ENTRY(vkGetDeviceImageSparseMemoryRequirements),
    /* V3: Core-1.3 Synchronization2 */
    RT_ENTRY(vkQueueSubmit2), RT_ENTRY(vkCmdPipelineBarrier2),
    /* V3: Core-1.3 Copy2-Kommandos */
    RT_ENTRY(vkCmdCopyBuffer2), RT_ENTRY(vkCmdResolveImage2),
    RT_ENTRY(vkCmdCopyBufferToImage), RT_ENTRY(vkCmdCopyBufferToImage2),
    RT_ENTRY(vkCmdCopyImageToBuffer), RT_ENTRY(vkCmdCopyImageToBuffer2),
    RT_ENTRY(vkCmdCopyImage), RT_ENTRY(vkCmdCopyImage2),
    RT_ENTRY(vkCmdBlitImage), RT_ENTRY(vkCmdBlitImage2),
    /* V3: Core-1.2 RenderPass2 */
    RT_ENTRY(vkCreateRenderPass2), RT_ENTRY(vkCmdBeginRenderPass2),
    RT_ENTRY(vkCmdNextSubpass2), RT_ENTRY(vkCmdEndRenderPass2),
    /* V3: Core-1.2 Timeline-Semaphore */
    RT_ENTRY(vkGetSemaphoreCounterValue), RT_ENTRY(vkSignalSemaphore), RT_ENTRY(vkWaitSemaphores),
    /* V3: Core-1.2 Buffer-Device-Address */
    RT_ENTRY(vkGetBufferDeviceAddress), RT_ENTRY(vkGetBufferOpaqueCaptureAddress),
    RT_ENTRY(vkGetDeviceMemoryOpaqueCaptureAddress),
    /* V3: Core-1.1 Device-Group-Kommandos */
    RT_ENTRY(vkCmdSetDeviceMask), RT_ENTRY(vkCmdDispatchBase),
    /* V3: Core-1.3 Dynamic Rendering */
    RT_ENTRY(vkCmdBeginRendering), RT_ENTRY(vkCmdEndRendering),
    /* V3: Core-1.3 Synchronization2 Event-Kommandos */
    RT_ENTRY(vkCmdSetEvent2), RT_ENTRY(vkCmdResetEvent2), RT_ENTRY(vkCmdWaitEvents2),
    RT_ENTRY(vkCmdWriteTimestamp2),
    /* V3: Core-1.1 Descriptor-Set-Layout-Support + Update-Templates */
    RT_ENTRY(vkGetDescriptorSetLayoutSupport),
    RT_ENTRY(vkCreateDescriptorUpdateTemplate), RT_ENTRY(vkDestroyDescriptorUpdateTemplate),
    RT_ENTRY(vkUpdateDescriptorSetWithTemplate),
    /* V3: Core-1.3 Tool-Properties */
    RT_ENTRY(vkGetPhysicalDeviceToolProperties),
    /* V3: Core-1.3 Private-Data-Slots */
    RT_ENTRY(vkCreatePrivateDataSlot), RT_ENTRY(vkDestroyPrivateDataSlot),
    RT_ENTRY(vkSetPrivateData), RT_ENTRY(vkGetPrivateData),
    /* V3: Core-1.3 Extended-Dynamic-State */
    RT_ENTRY(vkCmdBindVertexBuffers2),
    RT_ENTRY(vkCmdSetCullMode), RT_ENTRY(vkCmdSetFrontFace),
    RT_ENTRY(vkCmdSetDepthTestEnable), RT_ENTRY(vkCmdSetDepthWriteEnable),
    /* V3b: EDS/EDS2/EDS3 -- dynamischer Viewport/Scissor/Discard/Blend */
    RT_ENTRY(vkCmdSetViewport), RT_ENTRY(vkCmdSetScissor),
    RT_ENTRY(vkCmdSetViewportWithCount), RT_ENTRY(vkCmdSetScissorWithCount),
    RT_ENTRY(vkCmdSetRasterizerDiscardEnable), RT_ENTRY(vkCmdSetDepthCompareOp),
    RT_ENTRY(vkCmdSetColorBlendEnableEXT), RT_ENTRY(vkCmdSetColorWriteMaskEXT),
    RT_ENTRY(vkCmdSetColorBlendEquationEXT), RT_ENTRY(vkCmdSetBlendConstants),
    RT_ENTRY(vkCmdSetPrimitiveTopology), RT_ENTRY(vkCmdSetPrimitiveRestartEnable),
    RT_ENTRY(vkCmdSetDepthBiasEnable), RT_ENTRY(vkCmdSetDepthBias),
    RT_ENTRY(vkCmdSetDepthBoundsTestEnable), RT_ENTRY(vkCmdSetDepthBounds),
    RT_ENTRY(vkCmdSetStencilTestEnable), RT_ENTRY(vkCmdSetStencilOp),
    RT_ENTRY(vkCmdSetStencilCompareMask), RT_ENTRY(vkCmdSetStencilWriteMask),
    RT_ENTRY(vkCmdSetStencilReference), RT_ENTRY(vkCmdSetLineWidth),
    /* V3: Core-1.1 Device-Groups + External-Properties */
    RT_ENTRY(vkTrimCommandPool), RT_ENTRY(vkGetDeviceGroupPeerMemoryFeatures),
    RT_ENTRY(vkEnumeratePhysicalDeviceGroups),
    RT_ENTRY(vkGetPhysicalDeviceExternalBufferProperties),
    RT_ENTRY(vkGetPhysicalDeviceExternalFenceProperties),
    RT_ENTRY(vkGetPhysicalDeviceExternalSemaphoreProperties),
    /* T3.5: Draw-Pfad (vk_cmd.c) */
    RT_ENTRY(vkCreateShaderModule), RT_ENTRY(vkDestroyShaderModule),
    RT_ENTRY(vkCreatePipelineLayout), RT_ENTRY(vkDestroyPipelineLayout),
    RT_ENTRY(vkCreateRenderPass), RT_ENTRY(vkDestroyRenderPass),
    RT_ENTRY(vkCreateFramebuffer), RT_ENTRY(vkDestroyFramebuffer),
    RT_ENTRY(vkCreateGraphicsPipelines), RT_ENTRY(vkCreateComputePipelines), RT_ENTRY(vkDestroyPipeline),
    RT_ENTRY(vkCreateCommandPool), RT_ENTRY(vkDestroyCommandPool),
    RT_ENTRY(vkAllocateCommandBuffers), RT_ENTRY(vkFreeCommandBuffers),
    RT_ENTRY(vkBeginCommandBuffer), RT_ENTRY(vkEndCommandBuffer),
    RT_ENTRY(vkCmdBeginRenderPass), RT_ENTRY(vkCmdEndRenderPass),
    RT_ENTRY(vkCmdBindPipeline), RT_ENTRY(vkCmdBindVertexBuffers),
    RT_ENTRY(vkCmdPushConstants), RT_ENTRY(vkCmdDraw), RT_ENTRY(vkCmdDispatch), RT_ENTRY(vkCmdPipelineBarrier),
    RT_ENTRY(vkCmdBindIndexBuffer), RT_ENTRY(vkCmdDrawIndexed),
    RT_ENTRY(vkCmdBindIndexBuffer2), RT_ENTRY(vkCmdPushConstants2),
    RT_ENTRY(vkCmdBindDescriptorSets2),   /* V3.4: Core-1.4 (Decl. in vk_rtos.h) */
    RT_ENTRY(vkCmdPushDescriptorSetKHR), RT_ENTRY(vkCmdPushDescriptorSet),   /* V3b: Push-Descriptors */
    RT_ENTRY(vkCopyMemoryToImage), RT_ENTRY(vkCopyImageToMemory),   /* V3b: Host-Image-Copy */
    RT_ENTRY(vkTransitionImageLayout),
    RT_ENTRY(vkMapMemory2), RT_ENTRY(vkUnmapMemory2), RT_ENTRY(vkGetImageSubresourceLayout2),
    RT_ENTRY(vkGetDeviceImageSubresourceLayout), RT_ENTRY(vkGetRenderingAreaGranularity),
    RT_ENTRY(vkCmdSetLineStipple),
    /* V3: Core-1.0/1.2 Indirect-Draws */
    RT_ENTRY(vkCmdDrawIndirect), RT_ENTRY(vkCmdDrawIndexedIndirect),
    RT_ENTRY(vkCmdDrawIndirectCount), RT_ENTRY(vkCmdDrawIndexedIndirectCount),
    RT_ENTRY(vkCreateDescriptorSetLayout), RT_ENTRY(vkDestroyDescriptorSetLayout),
    RT_ENTRY(vkCreateDescriptorPool), RT_ENTRY(vkDestroyDescriptorPool),
    RT_ENTRY(vkAllocateDescriptorSets), RT_ENTRY(vkUpdateDescriptorSets),
    RT_ENTRY(vkCmdBindDescriptorSets),
    RT_ENTRY(vkCreateSampler), RT_ENTRY(vkDestroySampler),
    RT_ENTRY(vkCmdFillBuffer), RT_ENTRY(vkCmdCopyBuffer), RT_ENTRY(vkCmdClearColorImage),
    RT_ENTRY(vkCmdResolveImage),   /* V1.6 MSAA */
    /* V1.8: Query-Pools + Events */
    RT_ENTRY(vkCreateQueryPool), RT_ENTRY(vkDestroyQueryPool), RT_ENTRY(vkResetQueryPool),
    RT_ENTRY(vkGetQueryPoolResults), RT_ENTRY(vkCmdResetQueryPool),
    RT_ENTRY(vkCmdBeginQuery), RT_ENTRY(vkCmdEndQuery), RT_ENTRY(vkCmdWriteTimestamp),
    RT_ENTRY(vkCreateEvent), RT_ENTRY(vkDestroyEvent), RT_ENTRY(vkGetEventStatus),
    RT_ENTRY(vkSetEvent), RT_ENTRY(vkResetEvent),
    RT_ENTRY(vkCmdSetEvent), RT_ENTRY(vkCmdResetEvent), RT_ENTRY(vkCmdWaitEvents),
    RT_ENTRY(vkCreateFence), RT_ENTRY(vkDestroyFence),
    RT_ENTRY(vkResetFences), RT_ENTRY(vkGetFenceStatus), RT_ENTRY(vkWaitForFences),
    RT_ENTRY(vkQueueSubmit),
    /* T3.6: WSI (vk_wsi.c) */
    RT_ENTRY(vkCreateRtosSurfaceRTOS), RT_ENTRY(vkDestroySurfaceKHR),
    RT_ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR),
    RT_ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR),
    RT_ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR),
    RT_ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR),
    RT_ENTRY(vkCreateSwapchainKHR), RT_ENTRY(vkDestroySwapchainKHR),
    RT_ENTRY(vkGetSwapchainImagesKHR), RT_ENTRY(vkAcquireNextImageKHR),
    RT_ENTRY(vkQueuePresentKHR),
    RT_ENTRY(vkCreateSemaphore), RT_ENTRY(vkDestroySemaphore),
};

static PFN_vkVoidFunction rt_lookup(const char *pName)
{
    if (!pName) { return 0; }
    for (unsigned i = 0; i < sizeof(g_entries) / sizeof(g_entries[0]); i++) {
        if (streq(g_entries[i].name, pName)) { return g_entries[i].fn; }
    }
    return 0;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    (void)instance;
    return rt_lookup(pName);
}

/* Global-/Instance-Level-Funktionen (erster Parameter ist KEIN VkDevice/-Queue/-CommandBuffer
 * bzw. Child davon). vkGetDeviceProcAddr MUSS fuer diese NULL liefern (Spez). */
static int rt_is_instance_level(const char *n)
{
    static const char *inst[] = {
        "vkEnumerateInstanceVersion", "vkEnumerateInstanceExtensionProperties",
        "vkEnumerateInstanceLayerProperties", "vkCreateInstance", "vkDestroyInstance",
        "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceProperties",
        "vkGetPhysicalDeviceFeatures", "vkGetPhysicalDeviceQueueFamilyProperties",
        "vkGetPhysicalDeviceMemoryProperties", "vkGetPhysicalDeviceFormatProperties",
        "vkGetPhysicalDeviceImageFormatProperties", "vkGetPhysicalDeviceSparseImageFormatProperties",
        "vkEnumerateDeviceExtensionProperties", "vkEnumerateDeviceLayerProperties",
        "vkCreateDevice", "vkGetInstanceProcAddr",
        /* V3: Core-1.1 PhysicalDevice-"2"-Abfragen sind Instance-Level */
        "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceProperties2",
        "vkGetPhysicalDeviceFormatProperties2", "vkGetPhysicalDeviceImageFormatProperties2",
        "vkGetPhysicalDeviceQueueFamilyProperties2", "vkGetPhysicalDeviceMemoryProperties2",
        "vkGetPhysicalDeviceSparseImageFormatProperties2",
        "vkEnumeratePhysicalDeviceGroups", "vkGetPhysicalDeviceExternalBufferProperties",
        "vkGetPhysicalDeviceExternalFenceProperties", "vkGetPhysicalDeviceExternalSemaphoreProperties",
        "vkGetPhysicalDeviceToolProperties",
        "vkDestroySurfaceKHR", "vkCreateRtosSurfaceRTOS",
        "vkGetPhysicalDeviceSurfaceSupportKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
        "vkGetPhysicalDeviceSurfaceFormatsKHR", "vkGetPhysicalDeviceSurfacePresentModesKHR",
    };
    for (unsigned i = 0; i < sizeof(inst) / sizeof(inst[0]); i++) {
        if (streq(inst[i], n)) { return 1; }
    }
    return 0;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    (void)device;
    if (!pName || rt_is_instance_level(pName)) { return 0; }   /* nur Device-Level (Spez) */
    return rt_lookup(pName);
}

/* ---------------- V4: ICD-Interface (Loader-Anbindung) ----------------
 * Ein Vulkan-Loader laedt eine ICD ueber diese drei Symbole (vk_icd.h-Kontrakt). */
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
    if (!pSupportedVersion) { return VK_ERROR_INITIALIZATION_FAILED; }
    /* Unsere ICD spricht Loader-ICD-Interface-Version 5; auf das Minimum mit dem Loader klemmen. */
    if (*pSupportedVersion > 5) { *pSupportedVersion = 5; }
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    return vkGetInstanceProcAddr(instance, pName);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
    (void)instance;                                    /* nur PhysicalDevice-Level (nimmt VkPhysicalDevice) */
    static const char pfx[] = "vkGetPhysicalDevice";
    if (pName) {
        int match = 1;
        for (int i = 0; pfx[i]; i++) { if (pName[i] != pfx[i]) { match = 0; break; } }
        if (match) { return rt_lookup(pName); }
    }
    return 0;
}
