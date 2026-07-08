/*
 * user/lib/vk/vk_rtos.h  --  rpi_rtos-Vulkan-Implementierung: interner Header + Plattform-Teil.
 */
#ifndef RPI_RTOS_VK_RTOS_H
#define RPI_RTOS_VK_RTOS_H

#include "vulkan/vulkan.h"

/* ---- Plattform-Bootstrap (vor vkCreateInstance aufrufen) ---- */
void vk_rtos_set_heap(void *base, VkDeviceSize bytes);

/* ---- Pool-Groessen (ehrliche, dokumentierte Limits) ---- */
#define RT_MAX_MEMOBJ   16
#define RT_MAX_BUFFER   16
#define RT_MAX_IMAGE    8
#define RT_MAX_VIEW     8
#define RT_MEM_ALIGN    64

/* ---- interne Objekt-Strukturen (Handles sind Zeiger hierauf) ---- */
typedef struct rt_mem {
    int          used;
    unsigned char *base;          /* Zeiger in die Arena */
    VkDeviceSize size;
} rt_mem_t;

typedef struct rt_buffer {
    int           used;
    VkDeviceSize  size;
    VkBufferUsageFlags usage;
    rt_mem_t     *mem;            /* nach vkBindBufferMemory */
    VkDeviceSize  off;
} rt_buffer_t;

typedef struct rt_image {
    int           used;
    VkFormat      format;
    VkExtent3D    extent;
    VkImageUsageFlags usage;
    VkDeviceSize  row_pitch;      /* Bytes je Zeile (linear intern) */
    VkDeviceSize  bytes;          /* Gesamtgroesse */
    rt_mem_t     *mem;            /* nach vkBindImageMemory */
    VkDeviceSize  off;
    int           is_swapchain;   /* 1: Pixel liegen im GUI-Backbuffer (T3.6) */
    unsigned char *ext_pixels;    /* Swapchain: direkte Pixel-Basis (GUI-Backbuffer) */
    int           samples;        /* V1.6 MSAA: 1 oder 4; bei 4 ist der Speicher SAMPLE-MAJOR (samples×) */
} rt_image_t;

typedef struct rt_view {
    int         used;
    rt_image_t *image;
    VkFormat    format;
} rt_view_t;

typedef struct rt_queue { int alive; } rt_queue_t;
typedef struct rt_device {
    int        alive;
    rt_queue_t queue;
} rt_device_t;

/* Pixel-Basisadresse eines gebundenen Images (0 wenn ungebunden). */
unsigned char *rt_image_pixels(const rt_image_t *img);

/* Handle-Aufloesung fuer die weiteren Uebersetzungseinheiten (vk_cmd.c usw.). */
rt_image_t  *rt_image_from_handle(VkImage h);
rt_view_t   *rt_view_from_handle(VkImageView h);
rt_buffer_t *rt_buffer_from_handle(VkBuffer h);

/* Swapchain-Image anlegen (Pixel = externer Puffer, z.B. GUI-Backbuffer). intern (vk_wsi.c). */
rt_image_t *rt_image_alloc_swapchain(VkFormat fmt, unsigned w, unsigned h,
                                     unsigned pitch_bytes, unsigned char *pixels);
/* Fence signalisieren (vk_cmd.c besitzt den Fence-Pool; vkAcquireNextImageKHR nutzt es). */
void rt_fence_signal(VkFence f);

/* ---- Plattform-WSI (T3.6): rpi_rtos-Surface ueber die GUI-Bruecke ----
 * Wie auf jeder Vulkan-Plattform ist die SURFACE-ERZEUGUNG plattformspezifisch
 * (vgl. vkCreateWin32SurfaceKHR/vkCreateXlibSurfaceKHR); alles danach (Swapchain,
 * Acquire, Present) ist Standard-VK_KHR_swapchain. Hinweis: "RTOS" ist KEIN bei
 * Khronos registriertes Autoren-Tag -- eigenstaendige Plattform, dokumentiert. */
#define VK_RTOS_SURFACE_EXTENSION_NAME "VK_RTOS_surface"
#define VK_STRUCTURE_TYPE_RTOS_SURFACE_CREATE_INFO_RTOS ((VkStructureType)1000999000)

typedef struct VkRtosSurfaceCreateInfoRTOS {
    VkStructureType sType;        /* VK_STRUCTURE_TYPE_RTOS_SURFACE_CREATE_INFO_RTOS */
    const void     *pNext;
    VkFlags         flags;
} VkRtosSurfaceCreateInfoRTOS;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRtosSurfaceRTOS(
    VkInstance instance, const VkRtosSurfaceCreateInfoRTOS *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface);

/* --- Core 1.4 (V3.4): Entry-Points, die nur bereits vorhandene 1.3-Typen nutzen, hier vorab
 * deklariert (Signaturen exakt nach Vulkan-1.4-Spez), bis der vendored Header auf 1.4.355
 * gebumpt wird. Erspart den grossen mechanischen Header-Austausch fuer diese Funktionen. */
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer2(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
    VkIndexType indexType);

/* Core 1.4 vkCmdPushConstants2: der zugehoerige Info-Struct wird hier deklariert (sType-Wert exakt
 * nach Spez; unsere Impl validiert sType nicht). So gehen auch 1.4-Funktionen mit NEUEN, einfachen
 * Structs ohne den vollen vendored-Header-Austausch. */
#define VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO ((VkStructureType)1000337003)
typedef struct VkPushConstantsInfo {
    VkStructureType     sType;
    const void         *pNext;
    VkPipelineLayout    layout;
    VkShaderStageFlags  stageFlags;
    uint32_t            offset;
    uint32_t            size;
    const void         *pValues;
} VkPushConstantsInfo;
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants2(
    VkCommandBuffer commandBuffer, const VkPushConstantsInfo *pPushConstantsInfo);

/* Core 1.4 vkCmdBindDescriptorSets2 (+ Info-Struct, sType nach Spez). */
#define VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO ((VkStructureType)1000337002)
typedef struct VkBindDescriptorSetsInfo {
    VkStructureType         sType;
    const void             *pNext;
    VkShaderStageFlags      stageFlags;
    VkPipelineLayout        layout;
    uint32_t                firstSet;
    uint32_t                descriptorSetCount;
    const VkDescriptorSet  *pDescriptorSets;
    uint32_t                dynamicOffsetCount;
    const uint32_t         *pDynamicOffsets;
} VkBindDescriptorSetsInfo;
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets2(
    VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo *pBindDescriptorSetsInfo);

/* Core 1.4 vkCmdPushDescriptorSet (Promotion von VK_KHR_push_descriptor; identische Signatur zur
 * KHR-Variante im vendored Header). Als Supplement deklariert, da der 1.3.290-Header nur die
 * KHR-Form kennt. */
VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSet(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites);

/* Core 1.4 / VK_EXT_host_image_copy -- synchrone Host<->Image-Kopie ohne Command-Buffer/Queue.
 * Als Supplement deklariert (die 1.3.290-Vendored-Header kennen die Extension nicht). sType-Werte
 * exakt nach Spez (EXT-Nummernraum 1000270xxx). Subset: B8G8R8A8 (4 Byte/Pixel), Mip/Layer 0. */
#define VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY              ((VkStructureType)1000270000)
#define VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY             ((VkStructureType)1000270001)
#define VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO        ((VkStructureType)1000270004)
#define VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO        ((VkStructureType)1000270005)
#define VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO ((VkStructureType)1000270007)
typedef struct VkMemoryToImageCopy {
    VkStructureType           sType;
    const void               *pNext;
    const void               *pHostPointer;
    uint32_t                  memoryRowLength;
    uint32_t                  memoryImageHeight;
    VkImageSubresourceLayers  imageSubresource;
    VkOffset3D                imageOffset;
    VkExtent3D                imageExtent;
} VkMemoryToImageCopy;
typedef struct VkImageToMemoryCopy {
    VkStructureType           sType;
    const void               *pNext;
    void                     *pHostPointer;
    uint32_t                  memoryRowLength;
    uint32_t                  memoryImageHeight;
    VkImageSubresourceLayers  imageSubresource;
    VkOffset3D                imageOffset;
    VkExtent3D                imageExtent;
} VkImageToMemoryCopy;
typedef struct VkCopyMemoryToImageInfo {
    VkStructureType            sType;
    const void                *pNext;
    VkFlags                    flags;
    VkImage                    dstImage;
    VkImageLayout              dstImageLayout;
    uint32_t                   regionCount;
    const VkMemoryToImageCopy *pRegions;
} VkCopyMemoryToImageInfo;
typedef struct VkCopyImageToMemoryInfo {
    VkStructureType            sType;
    const void                *pNext;
    VkFlags                    flags;
    VkImage                    srcImage;
    VkImageLayout              srcImageLayout;
    uint32_t                   regionCount;
    const VkImageToMemoryCopy *pRegions;
} VkCopyImageToMemoryInfo;
typedef struct VkHostImageLayoutTransitionInfo {
    VkStructureType          sType;
    const void             *pNext;
    VkImage                 image;
    VkImageLayout           oldLayout;
    VkImageLayout           newLayout;
    VkImageSubresourceRange subresourceRange;
} VkHostImageLayoutTransitionInfo;
VKAPI_ATTR VkResult VKAPI_CALL vkCopyMemoryToImage(
    VkDevice device, const VkCopyMemoryToImageInfo *pCopyMemoryToImageInfo);
VKAPI_ATTR VkResult VKAPI_CALL vkCopyImageToMemory(
    VkDevice device, const VkCopyImageToMemoryInfo *pCopyImageToMemoryInfo);
VKAPI_ATTR VkResult VKAPI_CALL vkTransitionImageLayout(
    VkDevice device, uint32_t transitionCount, const VkHostImageLayoutTransitionInfo *pTransitions);

/* Core 1.4 vkMapMemory2/vkUnmapMemory2 (+ Info-Structs, sType nach Spez). */
#define VK_STRUCTURE_TYPE_MEMORY_MAP_INFO   ((VkStructureType)1000271000)
#define VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO ((VkStructureType)1000271001)
typedef struct VkMemoryMapInfo {
    VkStructureType   sType;
    const void       *pNext;
    VkMemoryMapFlags  flags;
    VkDeviceMemory    memory;
    VkDeviceSize      offset;
    VkDeviceSize      size;
} VkMemoryMapInfo;
typedef struct VkMemoryUnmapInfo {
    VkStructureType    sType;
    const void        *pNext;
    VkFlags            flags;
    VkDeviceMemory     memory;
} VkMemoryUnmapInfo;
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory2(
    VkDevice device, const VkMemoryMapInfo *pMemoryMapInfo, void **ppData);
VKAPI_ATTR VkResult VKAPI_CALL vkUnmapMemory2(
    VkDevice device, const VkMemoryUnmapInfo *pMemoryUnmapInfo);

/* Core 1.4 vkGetImageSubresourceLayout2 (+ Info-/Ergebnis-Structs, sType nach Spez). */
#define VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2  ((VkStructureType)1000338002)
#define VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2 ((VkStructureType)1000338003)
typedef struct VkImageSubresource2 {
    VkStructureType     sType;
    void               *pNext;
    VkImageSubresource  imageSubresource;
} VkImageSubresource2;
typedef struct VkSubresourceLayout2 {
    VkStructureType     sType;
    void               *pNext;
    VkSubresourceLayout subresourceLayout;
} VkSubresourceLayout2;
VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout2(
    VkDevice device, VkImage image, const VkImageSubresource2 *pSubresource,
    VkSubresourceLayout2 *pLayout);

/* Core 1.4 vkGetDeviceImageSubresourceLayout (objektlos, aus dem Create-Info). */
#define VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO ((VkStructureType)1000338001)
typedef struct VkDeviceImageSubresourceInfo {
    VkStructureType            sType;
    const void                *pNext;
    const VkImageCreateInfo   *pCreateInfo;
    const VkImageSubresource2 *pSubresource;
} VkDeviceImageSubresourceInfo;
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageSubresourceLayout(
    VkDevice device, const VkDeviceImageSubresourceInfo *pInfo, VkSubresourceLayout2 *pLayout);

/* Core 1.4 vkGetRenderingAreaGranularity (+ VkRenderingAreaInfo, sType nach Spez). */
#define VK_STRUCTURE_TYPE_RENDERING_AREA_INFO ((VkStructureType)1000044009)
typedef struct VkRenderingAreaInfo {
    VkStructureType  sType;
    const void      *pNext;
    uint32_t         viewMask;
    uint32_t         colorAttachmentCount;
    const VkFormat  *pColorAttachmentFormats;
    VkFormat         depthAttachmentFormat;
    VkFormat         stencilAttachmentFormat;
} VkRenderingAreaInfo;
VKAPI_ATTR void VKAPI_CALL vkGetRenderingAreaGranularity(
    VkDevice device, const VkRenderingAreaInfo *pRenderingAreaInfo, VkExtent2D *pGranularity);

/* Core 1.4 vkCmdSetLineStipple (nur basale Typen). */
VKAPI_ATTR void VKAPI_CALL vkCmdSetLineStipple(
    VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern);

/* --- V4: ICD-Interface (Loader-Anbindung) -- die Entry-Points, die ein echter Vulkan-Loader
 * beim Laden eines Installable Client Driver aufruft (vk_icd.h-Kontrakt). */
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName);

#endif /* RPI_RTOS_VK_RTOS_H */
