/*
 * user/lib/vk/vk_wsi.c  --  WSI: VK_KHR_surface + VK_KHR_swapchain ueber die
 * GUI-Bruecke. Die SURFACE-Erzeugung ist -- wie auf jeder Vulkan-Plattform --
 * plattformspezifisch (vkCreateRtosSurfaceRTOS, vgl. vkCreateWin32SurfaceKHR); alles
 * weitere ist Standard-API:
 *   Swapchain = 1 Image, direkt vom GUI-BACKBUFFER (EL0-VA 0x18000000) gestuetzt ->
 *   der Draw-Pfad (vk_cmd.c) rendert ohne Kopie in den Backbuffer;
 *   vkQueuePresentKHR stoesst SYS_GUI_FLUSH an (Kernel kopiert bb->FB + Cache fuer GPU).
 * Subset: 1 Surface, 1 Swapchain, 1 Image, FIFO; Semaphoren sind (bei synchronem
 * Submit) triviale Objekte. MIT FP kompiliert.
 */
#include "vk_rtos.h"
#include "../gui.h"

void *memset(void *dst, int c, unsigned long n);

typedef struct {
    int   used;
    gui_t gui;                    /* Backbuffer-Kontext (SYS_GUI_INFO) */
} rt_surface_t;

typedef struct {
    int           used;
    rt_surface_t *surf;
    rt_image_t   *img;            /* 1 Swapchain-Image (Backbuffer-gestuetzt) */
} rt_swapchain_t;

typedef struct { int used; int is_timeline; unsigned long long value; } rt_sem_t;   /* V3: Timeline-Zähler */

static rt_surface_t   g_surf[1];
static rt_swapchain_t g_swap[1];
static rt_sem_t       g_sem[4];

/* T3-Review #16: von vkDestroyDevice -> Swapchain/Semaphoren-Slots freigeben (Surface
 * ueberlebt das Device per Spez -> NICHT zuruecksetzen). Ohne dies bliebe der einzige
 * Swapchain-Slot nach Device-Neuerstellung dauerhaft belegt. */
void rt_wsi_reset_device(void)
{
    if (g_swap[0].used && g_swap[0].img) { g_swap[0].img->used = 0; }
    memset(g_swap, 0, sizeof(g_swap));
    memset(g_sem, 0, sizeof(g_sem));
}

/* ---------------- Plattform-Surface ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRtosSurfaceRTOS(
    VkInstance instance, const VkRtosSurfaceCreateInfoRTOS *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    (void)instance; (void)pAllocator;
    if (!pCreateInfo || !pSurface ||
        pCreateInfo->sType != VK_STRUCTURE_TYPE_RTOS_SURFACE_CREATE_INFO_RTOS) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (g_surf[0].used) { return VK_ERROR_OUT_OF_HOST_MEMORY; }   /* 1 Surface (Limit) */
    if (gui_init(&g_surf[0].gui) != 0) {
        return VK_ERROR_INITIALIZATION_FAILED;         /* keine GUI-Bruecke/GUI-Cap */
    }
    g_surf[0].used = 1;
    *pSurface = (VkSurfaceKHR)(void *)&g_surf[0];
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator)
{
    (void)instance; (void)pAllocator;
    if (surface) { ((rt_surface_t *)(void *)surface)->used = 0; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface,
    VkBool32 *pSupported)
{
    (void)physicalDevice; (void)queueFamilyIndex; (void)surface;
    *pSupported = VK_TRUE;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
    (void)physicalDevice;
    rt_surface_t *s = (rt_surface_t *)(void *)surface;
    if (!s || !s->used) { return VK_ERROR_SURFACE_LOST_KHR; }
    VkSurfaceCapabilitiesKHR *c = pSurfaceCapabilities;
    memset(c, 0, sizeof(*c));
    c->minImageCount = 1;
    c->maxImageCount = 1;                              /* Backbuffer = genau 1 Image */
    c->currentExtent.width  = s->gui.width;
    c->currentExtent.height = s->gui.height;
    c->minImageExtent = c->currentExtent;
    c->maxImageExtent = c->currentExtent;
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform    = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
    (void)physicalDevice; (void)surface;
    if (!pSurfaceFormats) { *pSurfaceFormatCount = 1; return VK_SUCCESS; }
    if (*pSurfaceFormatCount < 1) { *pSurfaceFormatCount = 0; return VK_INCOMPLETE; }
    pSurfaceFormats[0].format     = VK_FORMAT_B8G8R8A8_UNORM;
    pSurfaceFormats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    *pSurfaceFormatCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
    (void)physicalDevice; (void)surface;
    if (!pPresentModes) { *pPresentModeCount = 1; return VK_SUCCESS; }
    if (*pPresentModeCount < 1) { *pPresentModeCount = 0; return VK_INCOMPLETE; }
    pPresentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *pPresentModeCount = 1;
    return VK_SUCCESS;
}

/* ---------------- Swapchain ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pSwapchain) { return VK_ERROR_INITIALIZATION_FAILED; }
    rt_surface_t *s = (rt_surface_t *)(void *)pCreateInfo->surface;
    if (!s || !s->used) { return VK_ERROR_SURFACE_LOST_KHR; }
    if (g_swap[0].used) { return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR; }
    if (pCreateInfo->imageFormat != VK_FORMAT_B8G8R8A8_UNORM) {
        return VK_ERROR_INITIALIZATION_FAILED;         /* Subset: natives Backbuffer-Format */
    }
    rt_image_t *im = rt_image_alloc_swapchain(
        VK_FORMAT_B8G8R8A8_UNORM, s->gui.width, s->gui.height,
        s->gui.wpr * 4u, (unsigned char *)(void *)s->gui.bb);
    if (!im) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    g_swap[0].used = 1;
    g_swap[0].surf = s;
    g_swap[0].img  = im;
    *pSwapchain = (VkSwapchainKHR)(void *)&g_swap[0];
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    rt_swapchain_t *sw = (rt_swapchain_t *)(void *)swapchain;
    if (sw && sw->used) {
        if (sw->img) { sw->img->used = 0; }
        sw->used = 0;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount,
    VkImage *pSwapchainImages)
{
    (void)device;
    rt_swapchain_t *sw = (rt_swapchain_t *)(void *)swapchain;
    if (!sw || !sw->used) { return VK_ERROR_INITIALIZATION_FAILED; }
    if (!pSwapchainImages) { *pSwapchainImageCount = 1; return VK_SUCCESS; }
    if (*pSwapchainImageCount < 1) { *pSwapchainImageCount = 0; return VK_INCOMPLETE; }
    pSwapchainImages[0] = (VkImage)(void *)sw->img;
    *pSwapchainImageCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    (void)device; (void)timeout; (void)semaphore;
    rt_swapchain_t *sw = (rt_swapchain_t *)(void *)swapchain;
    if (!sw || !sw->used) { return VK_ERROR_OUT_OF_DATE_KHR; }
    *pImageIndex = 0;                                  /* 1 Image, sofort verfuegbar */
    rt_fence_signal(fence);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    (void)queue;
    if (!pPresentInfo || pPresentInfo->swapchainCount < 1) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    rt_swapchain_t *sw = (rt_swapchain_t *)(void *)pPresentInfo->pSwapchains[0];
    if (!sw || !sw->used) { return VK_ERROR_OUT_OF_DATE_KHR; }
    gui_flush_all(&sw->surf->gui);                     /* SYS_GUI_FLUSH: bb -> echter FB + GPU-Cache */
    if (pPresentInfo->pResults) { pPresentInfo->pResults[0] = VK_SUCCESS; }
    return VK_SUCCESS;
}

/* ---------------- Semaphoren (synchrones Submit -> triviale Objekte) ---------------- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(
    VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore)
{
    (void)device; (void)pAllocator;
    /* V3: Timeline-Semaphore via VkSemaphoreTypeCreateInfo in der pNext-Kette (Core 1.2). */
    int is_tl = 0; unsigned long long init = 0;
    if (pCreateInfo) {
        const VkBaseInStructure *p = (const VkBaseInStructure *)pCreateInfo->pNext;
        while (p) {
            if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
                const VkSemaphoreTypeCreateInfo *st = (const VkSemaphoreTypeCreateInfo *)(const void *)p;
                if (st->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) { is_tl = 1; init = st->initialValue; }
            }
            p = p->pNext;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!g_sem[i].used) {
            g_sem[i].used = 1; g_sem[i].is_timeline = is_tl; g_sem[i].value = init;
            *pSemaphore = (VkSemaphore)(void *)&g_sem[i];
            return VK_SUCCESS;
        }
    }
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(
    VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    if (semaphore) { ((rt_sem_t *)(void *)semaphore)->used = 0; }
}

/* V3: Core-1.2 Timeline-Semaphore-Operationen. Synchron/single-threaded: Signal setzt den Zähler,
 * GetCounterValue liest ihn, WaitSemaphores ist erfüllt sobald die aktuellen Werte die Wartewerte
 * erreichen (nichts signalisiert später -> sonst VK_TIMEOUT, kein Deadlock). */
VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(
    VkDevice device, VkSemaphore semaphore, uint64_t *pValue)
{
    (void)device;
    rt_sem_t *s = (rt_sem_t *)(void *)semaphore;
    if (!s || !pValue) { return VK_ERROR_INITIALIZATION_FAILED; }
    *pValue = s->value;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore(
    VkDevice device, const VkSemaphoreSignalInfo *pSignalInfo)
{
    (void)device;
    if (!pSignalInfo) { return VK_ERROR_INITIALIZATION_FAILED; }
    rt_sem_t *s = (rt_sem_t *)(void *)pSignalInfo->semaphore;
    if (s) { s->value = pSignalInfo->value; }
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(
    VkDevice device, const VkSemaphoreWaitInfo *pWaitInfo, uint64_t timeout)
{
    (void)device; (void)timeout;
    if (!pWaitInfo) { return VK_ERROR_INITIALIZATION_FAILED; }
    if (pWaitInfo->semaphoreCount == 0) { return VK_SUCCESS; }
    int any = (pWaitInfo->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0;
    int all_ok = 1, any_ok = 0;
    for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; i++) {
        rt_sem_t *s = (rt_sem_t *)(void *)pWaitInfo->pSemaphores[i];
        unsigned long long need = pWaitInfo->pValues[i];
        if (s && s->value >= need) { any_ok = 1; } else { all_ok = 0; }
    }
    return (any ? any_ok : all_ok) ? VK_SUCCESS : VK_TIMEOUT;
}
