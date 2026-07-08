/*
 * include/gui_abi.h  --  User<->Kernel-ABI der Grafik-Bruecke, von Kernel UND GUI-Apps genutzt.
 *
 * Eine EL0-GUI-App holt sich per SYS_GUI_INFO die Framebuffer-Geometrie und die feste EL0-VA
 * eines geteilten Backbuffers, zeichnet direkt in diesen Backbuffer (cachend) und ruft
 * SYS_GUI_FLUSH, damit der Kernel die Zeilen in den echten VideoCore-Framebuffer kopiert und
 * fuer die (nicht cache-kohaerente) GPU sichtbar macht.
 */
#ifndef RPI_RTOS_GUI_ABI_H
#define RPI_RTOS_GUI_ABI_H

#include <stdint.h>

typedef struct {
    uint32_t width;      /* Pixel je Zeile          */
    uint32_t height;     /* Zeilen                  */
    uint32_t pitch;      /* Bytes je Zeile (>= width*4) */
    uint32_t bpp;        /* Bits pro Pixel (32)     */
    uint64_t fb_va;      /* EL0-VA des Backbuffers (0xRRGGBB je uint32_t, little-endian im RAM) */
    uint64_t fb_bytes;   /* nutzbare Groesse des Backbuffers (height*pitch)                     */
} gui_fb_info_t;

/* --- Eingabe-Events: Kernel-Event-Queue <-> SYS_POLL_EVENT/SYS_WAIT_EVENT --- */
#define GUI_EV_NONE   0
#define GUI_EV_MOUSE  1      /* x,y = Cursor-Position, buttons = aktuelle Maske */
#define GUI_EV_KEY    2      /* key = ASCII-Zeichen */

typedef struct {
    uint8_t  type;           /* GUI_EV_* */
    uint8_t  buttons;        /* Maus-Buttons: bit0 links, bit1 rechts, bit2 mitte */
    uint8_t  key;            /* ASCII (bei GUI_EV_KEY) */
    uint8_t  _pad;
    int16_t  x, y;           /* Cursor-Position (bei GUI_EV_MOUSE) */
} gui_event_t;

#endif /* RPI_RTOS_GUI_ABI_H */
