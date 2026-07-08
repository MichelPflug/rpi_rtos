/*
 * include/fbcon.h  --  Text-Konsole auf dem Framebuffer (8x8-Font)
 */
#ifndef RPI_RTOS_FBCON_H
#define RPI_RTOS_FBCON_H

#include <stdint.h>

/* Initialisiert die Konsole auf dem (zuvor per fb_init bereiten) Framebuffer.
 * fg/bg als 0xRRGGBB. 0 = ok, -1 = kein Framebuffer. */
int  fbcon_init(uint32_t fg, uint32_t bg);

void fbcon_putc(char c);
void fbcon_puts(const char *s);
void fbcon_putdec(unsigned long long v);

#endif /* RPI_RTOS_FBCON_H */
