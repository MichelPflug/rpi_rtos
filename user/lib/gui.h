/*
 * user/lib/gui.h  --  Geteilte EL0-Zeichenbibliothek.
 */
#ifndef RPI_RTOS_GUI_H
#define RPI_RTOS_GUI_H

#include "gui_abi.h"

#define GUI_GLYPH_W 8
#define GUI_GLYPH_H 8

typedef struct {
    volatile unsigned *bb;   /* Backbuffer-Basis (EL0-VA), 0xRRGGBB je Pixel */
    unsigned width, height;  /* Pixel */
    unsigned wpr;            /* Woerter (Pixel) je Zeile = pitch/4 */
} gui_t;

/* Initialisiert den Kontext aus SYS_GUI_INFO. 0 = ok, -1 = keine Bruecke / keine GUI-Cap. */
int  gui_init(gui_t *g);

/* Backbuffer-Zeilen [y, y+nrows) auf den echten Framebuffer schieben (SYS_GUI_FLUSH). */
void gui_flush(const gui_t *g, int y, int nrows);
void gui_flush_all(const gui_t *g);

/* Zeichenprimitive -- alle clippen auf [0,width) x [0,height). */
void gui_clear(const gui_t *g, unsigned rgb);
void gui_pixel(const gui_t *g, int x, int y, unsigned rgb);
void gui_fill_rect(const gui_t *g, int x, int y, int w, int h, unsigned rgb);
void gui_rect(const gui_t *g, int x, int y, int w, int h, unsigned rgb);   /* 1px-Rahmen */
void gui_hline(const gui_t *g, int x, int y, int len, unsigned rgb);
void gui_vline(const gui_t *g, int x, int y, int len, unsigned rgb);

/* Ein Zeichen / eine Zeichenkette (8x8-Font, scale >= 1 -> 8*scale x 8*scale).
 * bg == GUI_TRANSPARENT: Hintergrundpixel nicht setzen (nur Vordergrund malen). */
#define GUI_TRANSPARENT 0xFF000000u
void gui_char(const gui_t *g, int x, int y, char c, unsigned fg, unsigned bg, int scale);
void gui_text(const gui_t *g, int x, int y, const char *s, unsigned fg, unsigned bg, int scale);

/* Pixel auslesen (fuer Selbsttests); ausserhalb -> 0. */
unsigned gui_get(const gui_t *g, int x, int y);

/* --- Proportionale TTF-Bitmap-Fonts (.rfn, beim Build aus TrueType gerastert, siehe tools/gen_font.py) ---
 * Anti-aliased Graustufen-Glyphen, zur Laufzeit rein integer geblittet (EL0 ist FP-frei). */
typedef struct {
    const unsigned char *data;      /* das .rfn im Speicher (eingebettet oder von Datei) */
    int line_height, ascent;
    int first, count;               /* erstes Zeichen + Anzahl (0x20.. aufwaerts) */
    const unsigned char *table;     /* Glyph-Tabelle (count * 16 Byte) */
} gui_font_t;

/* .rfn aus dem Speicher an einen Font-Kontext binden. 0 = ok, -1 = ungueltig. */
int  gui_font_from_mem(gui_font_t *font, const unsigned char *data, unsigned len);

/* --- Laufzeit-TrueType (gui_ttf.c, FP; nur GUI-Builds) --- */
/* Smoke-Test: beweist, dass FP am EL0 laeuft (Rueckgabe 778 bei aktivem FP). */
int  gui_ttf_fp_smoke(void);

/* Eine .ttf im Speicher zur Laufzeit auf ASCII (0x20..0x7E) in Pixelhoehe pixel_em rastern (FP) und
 * als .rfn ins arena schreiben; font wird darauf gebunden. 0 = ok, -1 = ungueltig/zu klein. */
int  gui_font_rasterize_ttf(gui_font_t *font, const unsigned char *ttf, unsigned ttf_len, int pixel_em,
                            unsigned char *arena, unsigned arena_size);

/* Pixelbreite (Summe der Advances) der Zeichenkette im Font. */
int  gui_text_ttf_width(const gui_font_t *font, const char *s);

/* Zeichenkette mit dem TTF-Font zeichnen: y ist die OBERKANTE der Zeile. fg ueber bg anti-aliased
 * gemischt (bg = Hintergrundfarbe hinter dem Text). Glyphen ab pen >= clip_right entfallen. */
void gui_text_ttf(const gui_t *g, int x, int y, const char *s, const gui_font_t *font,
                  unsigned fg, unsigned bg, int clip_right);

#endif /* RPI_RTOS_GUI_H */
