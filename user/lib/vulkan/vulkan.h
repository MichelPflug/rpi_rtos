/*
 * user/lib/vulkan/vulkan.h  --  Einstiegs-Header der Vulkan-API (rpi_rtos-Port).
 *
 * vulkan_core.h + vk_platform.h sind die OFFIZIELLEN, unveraenderten Khronos-Header
 * (Vulkan-Headers v1.3.290, Apache-2.0, SPDX im Datei-Kopf) -- damit sind alle Namen,
 * Enum-Werte, Struct-Layouts und Prototypen exakt spezifikationsgleich. Dieses
 * Wrapper-File ersetzt das offizielle vulkan.h nur um die (hier nicht existenten)
 * Plattform-WSI-Includes (Win32/Xlib/...); die rpi_rtos-Surface-Erzeugung deklariert
 * vk_rtos.h (plattformspezifischer Teil, wie auf jeder Vulkan-Plattform ueblich).
 */
#ifndef VULKAN_H_
#define VULKAN_H_ 1

#include "vk_platform.h"
#include "vulkan_core.h"

#endif /* VULKAN_H_ */
