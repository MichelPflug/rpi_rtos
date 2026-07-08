/*
 * include/fb.h  --  Framebuffer (VideoCore, 32 bpp) via Mailbox
 */
#ifndef RPI_RTOS_FB_H
#define RPI_RTOS_FB_H

#include <stdint.h>

typedef struct {
    volatile uint8_t *base;     /* CPU-zugaengliche (physische) Framebuffer-Basis */
    uint32_t width, height;     /* Pixel */
    uint32_t pitch;             /* Bytes pro Zeile */
    uint32_t size;             /* Framebuffer-Groesse in Byte */
} fb_t;

/* Fordert per Mailbox einen Framebuffer width x height @ 32 bpp an. 0 = ok. */
int fb_init(uint32_t width, uint32_t height);

const fb_t *fb_get(void);

/* Einen Pixel setzen (0xRRGGBB) bzw. den ganzen Schirm fuellen. */
void fb_pixel(uint32_t x, uint32_t y, uint32_t rgb);
void fb_clear(uint32_t rgb);

/* Daten-Cache der angegebenen Zeilen bzw. des ganzen FB zum PoC bereinigen, damit
 * die GPU (nicht cache-kohaerent) die gerenderten Pixel sieht. In QEMU ein No-op. */
void fb_flush_rows(uint32_t y, uint32_t nrows);
void fb_flush(void);

#endif /* RPI_RTOS_FB_H */
