/*
 * drivers/video/fbcon.c  --  Text-Konsole auf dem Framebuffer (8x8-Font)
 *
 * Schlanker Zeichensatz (Grossbuchstaben + Ziffern + gaengige Satzzeichen);
 * Kleinbuchstaben werden auf Grossbuchstaben abgebildet, unbekannte druckbare
 * Zeichen als Kasten dargestellt. Cursor mit Zeilenumbruch und Scroll.
 *
 * Hinweis (echte HW): Der Framebuffer ist Write-Back-cacheable (schnelles Rendern).
 * Nach jedem Glyph/Scroll werden die betroffenen Zeilen per fb_flush_rows()/fb_flush()
 * (dc cvac) in den RAM getrieben, damit die nicht-kohaerente GPU sie sieht. In QEMU
 * ist dc cvac ein No-op (kohaerent).
 */
#include <stdint.h>
#include "fb.h"
#include "fbcon.h"
#include "kmem.h"

#define GLYPH_W 8
#define GLYPH_H 8

typedef struct { uint8_t ch; uint8_t bm[8]; } glyph_t;

static const glyph_t s_font[] = {
    { ' ', {0,0,0,0,0,0,0,0} },
    { '!', {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00} },
    { '#', {0x24,0x24,0x7E,0x24,0x7E,0x24,0x24,0x00} },
    { '$', {0x18,0x3E,0x58,0x3C,0x1A,0x7C,0x18,0x00} },
    { '(', {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00} },
    { ')', {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00} },
    { '+', {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00} },
    { ',', {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30} },
    { '-', {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00} },
    { '.', {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00} },
    { '/', {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00} },
    { '0', {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00} },
    { '1', {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00} },
    { '2', {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00} },
    { '3', {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00} },
    { '4', {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00} },
    { '5', {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00} },
    { '6', {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00} },
    { '7', {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00} },
    { '8', {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00} },
    { '9', {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00} },
    { ':', {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00} },
    { '=', {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00} },
    { '>', {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00} },
    { '?', {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00} },
    { '[', {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00} },
    { ']', {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00} },
    { '_', {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE} },
    { 'A', {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00} },
    { 'B', {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00} },
    { 'C', {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00} },
    { 'D', {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00} },
    { 'E', {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00} },
    { 'F', {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00} },
    { 'G', {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00} },
    { 'H', {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00} },
    { 'I', {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00} },
    { 'J', {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00} },
    { 'K', {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00} },
    { 'L', {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00} },
    { 'M', {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00} },
    { 'N', {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00} },
    { 'O', {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00} },
    { 'P', {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00} },
    { 'Q', {0x3C,0x66,0x66,0x66,0x6E,0x6C,0x36,0x00} },
    { 'R', {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00} },
    { 'S', {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00} },
    { 'T', {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00} },
    { 'U', {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00} },
    { 'V', {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00} },
    { 'W', {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00} },
    { 'X', {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00} },
    { 'Y', {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00} },
    { 'Z', {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00} },
};

static const uint8_t s_box[8] = {0x00,0x7E,0x42,0x42,0x42,0x42,0x7E,0x00};

static uint32_t s_fg, s_bg;
static uint32_t s_def_fg, s_def_bg;   /* Standardfarben (fuer SGR-Reset) */
static uint32_t s_cols, s_rows, s_cx, s_cy;
static int      s_ready;          /* erst nach fbcon_init zeichnen (sicher als uart-Mirror) */

/* ANSI-Standardfarben (0xRRGGBB), Indizes 0..7 = schwarz/rot/gruen/gelb/blau/
 * magenta/cyan/weiss. Blau aufgehellt fuer Lesbarkeit auf dunklem Grund. */
static const uint32_t s_ansi[8] = {
    0x000000, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x4060FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

/* SGR-Parser-Zustand: ESC '[' <num>(';'<num>)* 'm' */
static enum { ESC_NONE, ESC_ESC, ESC_CSI } s_esc;
static uint32_t s_num;            /* aktueller Parameter waehrend des Parsens */

static void sgr_apply(uint32_t p)
{
    if (p == 0) {                 /* Reset auf Standardfarben */
        s_fg = s_def_fg;
        s_bg = s_def_bg;
    } else if (p >= 30 && p <= 37) {
        s_fg = s_ansi[p - 30];
    } else if (p >= 90 && p <= 97) {
        s_fg = s_ansi[p - 90];    /* "bright" -> selbe Palette */
    } else if (p >= 40 && p <= 47) {
        s_bg = s_ansi[p - 40];
    }
    /* andere Codes (z.B. 1=bold) ignoriert */
}

static const uint8_t *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 32);                 /* Kleinbuchstaben -> Grossbuchstaben */
    }
    for (unsigned i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].ch == (uint8_t)c) {
            return s_font[i].bm;
        }
    }
    if (c >= 0x21 && c <= 0x7E) {
        return s_box;                        /* unbekanntes druckbares Zeichen */
    }
    return s_font[0].bm;                      /* Leerzeichen */
}

static void draw_glyph(uint32_t cx, uint32_t cy, char c)
{
    const uint8_t *bm = glyph_for(c);
    for (uint32_t row = 0; row < GLYPH_H; row++) {
        uint8_t bits = bm[row];
        for (uint32_t col = 0; col < GLYPH_W; col++) {
            uint32_t color = (bits & (0x80u >> col)) ? s_fg : s_bg;
            fb_pixel(cx * GLYPH_W + col, cy * GLYPH_H + row, color);
        }
    }
    fb_flush_rows(cy * GLYPH_H, GLYPH_H);   /* gerendertes Zeichen zur GPU treiben */
}

static void scroll(void)
{
    const fb_t *fb = fb_get();
    uint32_t rowbytes = fb->pitch * GLYPH_H;
    uint8_t *base = (uint8_t *)fb->base;
    /* Inhalt um eine Textzeile (8 Pixel) nach oben schieben. */
    memmove(base, base + rowbytes, (size_t)(fb->height - GLYPH_H) * fb->pitch);
    /* Genau die letzte TEXTZEILE mit Hintergrund fuellen (nicht den evtl. ungenutzten
     * Rand bei nicht durch 8 teilbarer Hoehe). */
    uint32_t y0 = (s_rows - 1) * GLYPH_H;
    for (uint32_t y = y0; y < y0 + GLYPH_H; y++) {
        volatile uint32_t *r = (volatile uint32_t *)(fb->base + (size_t)y * fb->pitch);
        for (uint32_t x = 0; x < fb->width; x++) {
            r[x] = s_bg;
        }
    }
    fb_flush();                 /* gesamter Schirm wurde verschoben -> komplett flushen */
    s_cy = s_rows - 1;
}

/* Unterstrich-Cursor (unterste 2 Zeilen der Zelle) an (s_cx,s_cy) in 'color' malen. */
static void cursor_paint(uint32_t color)
{
    if (s_cx >= s_cols || s_cy >= s_rows) {
        return;
    }
    for (uint32_t row = GLYPH_H - 2; row < GLYPH_H; row++) {
        for (uint32_t col = 0; col < GLYPH_W; col++) {
            fb_pixel(s_cx * GLYPH_W + col, s_cy * GLYPH_H + row, color);
        }
    }
    fb_flush_rows(s_cy * GLYPH_H, GLYPH_H);
}

static void cursor_draw(void)  { cursor_paint(s_fg); }
static void cursor_erase(void) { cursor_paint(s_bg); }

/* SGR-Escape-Sequenzen (ESC[..m) konsumieren und Farben setzen, ohne sie zu zeichnen.
 * Gibt 1 zurueck, wenn c Teil einer Escape-Sequenz war (also nicht gedruckt wird). */
static int handle_escape(char c)
{
    switch (s_esc) {
    case ESC_NONE:
        if (c == 0x1B) { s_esc = ESC_ESC; return 1; }   /* ESC */
        return 0;
    case ESC_ESC:
        if (c == '[') { s_esc = ESC_CSI; s_num = 0; return 1; }
        s_esc = ESC_NONE;                                /* unbekannt -> abbrechen */
        return 0;
    case ESC_CSI:
        if (c >= '0' && c <= '9') {
            s_num = s_num * 10 + (uint32_t)(c - '0');
        } else if (c == ';') {
            sgr_apply(s_num);
            s_num = 0;
        } else {                                         /* 'm' oder Ende */
            if (c == 'm') { sgr_apply(s_num); }
            s_esc = ESC_NONE;
        }
        return 1;
    }
    return 0;
}

int fbcon_init(uint32_t fg, uint32_t bg)
{
    const fb_t *fb = fb_get();
    if (!fb->base || fb->width < GLYPH_W || fb->height < GLYPH_H) {
        return -1;
    }
    s_fg = s_def_fg = fg;
    s_bg = s_def_bg = bg;
    s_esc = ESC_NONE;
    s_num = 0;
    s_cols = fb->width / GLYPH_W;
    s_rows = fb->height / GLYPH_H;
    s_cx = 0;
    s_cy = 0;
    fb_clear(bg);
    s_ready = 1;
    cursor_draw();
    return 0;
}

void fbcon_putc(char c)
{
    if (!s_ready) {
        return;                    /* vor fbcon_init: kein Framebuffer -> no-op */
    }
    if (handle_escape(c)) {
        return;                    /* SGR-Sequenz konsumiert (kein Glyph, kein Cursor) */
    }
    cursor_erase();                /* Cursor von der bisherigen Position entfernen */
    if (c == '\n') {
        s_cx = 0;
        if (++s_cy >= s_rows) {
            scroll();
        }
    } else if (c == '\r') {
        s_cx = 0;
    } else if (c == '\b') {
        if (s_cx > 0) { s_cx--; }   /* Backspace: Cursor eine Spalte zurueck (kein Glyph). ed_del
                                     * loescht dann mit "\b space \b" -> Zeichen wird ueberschrieben. */
    } else {
        draw_glyph(s_cx, s_cy, c);
        if (++s_cx >= s_cols) {
            s_cx = 0;
            if (++s_cy >= s_rows) {
                scroll();
            }
        }
    }
    cursor_draw();                 /* Cursor an der neuen Position zeichnen */
}

void fbcon_puts(const char *s)
{
    for (; *s; s++) {
        fbcon_putc(*s);
    }
}

void fbcon_putdec(unsigned long long v)
{
    char buf[20];
    int i = 0;
    if (v == 0) {
        fbcon_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i-- > 0) {
        fbcon_putc(buf[i]);
    }
}
