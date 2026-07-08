/*
 * drivers/video/gui_fb.c  --  Kernel-Seite der GUI-Grafik-Bruecke
 *
 * Ein 2-MiB-aligned Backbuffer wird von der MMU in jeden EL0-Adressraum an GUI_FB_USER_VA
 * gemappt (RW, UXN). EL0-Apps zeichnen cachend hinein; gui_fb_flush kopiert die betroffenen
 * Zeilen in den echten VideoCore-Framebuffer und pflegt dessen Cache fuer die (nicht cache-
 * kohaerente) GPU.
 *
 * Kohaerenz: Backbuffer (EL0-VA) und Kernel-Zugriff (Identity-VA) liegen auf DERSELBEN
 * physischen, Normal-WB-cacheable, Inner-Shareable Adresse -> PIPT-Caches sind kohaerent, der
 * Flush-Syscall laeuft synchron auf dem schreibenden Kern. Daher genuegt beim Flush: Backbuffer
 * -> FB kopieren + fb_flush_rows() (dc cvac auf den FB fuer die GPU). Kein dc auf den Backbuffer.
 */
#include <stdint.h>
#include "gui_fb.h"
#include "fb.h"
#include "proc.h"      /* GUI_FB_USER_VA */
#include "spinlock.h"

/* GUI_BB_SIZE / GUI_BB_TILES kommen aus gui_fb.h (gegated FULLHD: 1 Kachel/2 MiB bzw. 5/10 MiB). */

/* 2-MiB-aligned -> die MMU mappt die GUI_BB_TILES Kacheln ab hier nach GUI_FB_USER_VA. */
static uint8_t g_gui_bb[GUI_BB_SIZE] __attribute__((aligned(0x200000)));
static int     s_ready;

/* --- Maus-Cursor: der Kernel komponiert ihn direkt in den FB. Der Backbuffer enthaelt
 * ihn NIE. Bewegt sich die Maus, restauriert gui_fb_move_cursor die zuletzt gezeichneten Zeilen
 * aus dem Backbuffer (loescht den alten Cursor) und zeichnet ihn am neuen Ort neu -> KEINE
 * Geisterspur, unabhaengig davon, welche Zeilen die EL0-App flusht (frueher blieb bei einem
 * Partial-Flush, der die alte Cursor-Zeile nicht abdeckte, ein Geist stehen). 8x12-Pfeil:
 * cur_edge = schwarze Kontur, cur_body = weisse Fuellung, MSB-first (0x80 = linkes Pixel). --- */
#define CURSOR_H 12
/* Cursor-Skalierung: bei Full-HD (1920x1080) ist ein 8x12-Pfeil ein kaum sichtbarer Fleck ->
 * jeden Cursor-Pixel als CURSOR_SCALE x CURSOR_SCALE-Block zeichnen. Bei 640x480 (kein FULLHD)
 * bleibt es 1:1 (8x12) -> -Vk/GUI-640-Builds unveraendert. */
#ifdef FULLHD
#define CURSOR_SCALE 3
#else
#define CURSOR_SCALE 1
#endif
#define CURSOR_PH (CURSOR_H * CURSOR_SCALE)   /* Cursor-Hoehe in Pixeln (Zeilenband) */
static const uint8_t cur_edge[CURSOR_H] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFC, 0xFC, 0xDC, 0x8C, 0x0C
};
static const uint8_t cur_body[CURSOR_H] = {
    0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xF8, 0xD8, 0x88, 0x08, 0x00
};
/* Gewuenschte Cursor-Position: vom Timer-IRQ (Kern 0, gui_fb_move_cursor) geschrieben, von
 * draw_cursor im Flush-Syscall (beliebiger Kern) gelesen -> volatile (single-copy-atomic, kein
 * Tearing pro Wort). g_drawn_y/on merken, WO das Sprite zuletzt in den FB komponiert wurde (fuer
 * das Loeschen der alten Position). g_cursor_lock serialisiert FB-Zugriff + Cursor-Zustand:
 * der Flush-Syscall nimmt es blockierend, der Mover (Timer-IRQ) NUR per trylock -> der IRQ
 * blockiert nie und es gibt keine IRQ-vs-Syscall-Deadlock-Klasse. */
static volatile int g_cursor_x, g_cursor_y, g_cursor_on;
static int          g_drawn_y, g_drawn_on;
static spinlock_t   g_cursor_lock = SPINLOCK_INIT;

uint64_t gui_fb_phys(void) { return (uint64_t)(uintptr_t)g_gui_bb; }

/* Backbuffer-Zeilen [y, y+nrows) -> echter FB kopieren (ohne Cursor, ohne Cache-Pflege). */
static void copy_rows(const fb_t *fb, uint32_t y, uint32_t nrows)
{
    uint32_t wpr = fb->pitch / 4;                            /* 32 bpp -> pitch durch 4 teilbar */
    for (uint32_t r = 0; r < nrows; r++) {
        uint32_t row = y + r;
        volatile uint32_t *dst = (volatile uint32_t *)(fb->base + (uint64_t)row * fb->pitch);
        const uint32_t    *src = (const uint32_t *)(const void *)(g_gui_bb + (uint64_t)row * fb->pitch);
        for (uint32_t x = 0; x < wpr; x++) { dst[x] = src[x]; }
    }
}

/* Cursor in fb->base fuer die Zeilen [y0, y0+n) zeichnen. Aufrufer haelt g_cursor_lock. */
static void draw_cursor(const fb_t *fb, uint32_t y0, uint32_t n)
{
    if (!g_cursor_on) { return; }
    int cx = g_cursor_x, cy = g_cursor_y;                   /* konsistenter Snapshot */
#if CURSOR_SCALE == 1
    /* 1:1 (640x480) -- unveraendert, damit -Vk/GUI-640-Builds byte-identisch bleiben. */
    for (int r = 0; r < CURSOR_H; r++) {
        int py = cy + r;
        if (py < (int)y0 || py >= (int)(y0 + n) || py < 0 || py >= (int)fb->height) { continue; }
        volatile uint32_t *row = (volatile uint32_t *)(fb->base + (uint64_t)py * fb->pitch);
        uint8_t eb = cur_edge[r], bo = cur_body[r];
        for (int c = 0; c < 8; c++) {
            int px = cx + c;
            if (px < 0 || px >= (int)fb->width) { continue; }
            uint8_t m = (uint8_t)(0x80u >> c);
            if (bo & m)      { row[px] = 0x00FFFFFFu; }   /* Fuellung weiss */
            else if (eb & m) { row[px] = 0x00000000u; }   /* Kontur schwarz */
        }
    }
#else
    /* Full-HD: jeden Cursor-Pixel als CURSOR_SCALE x CURSOR_SCALE-Block (gut sichtbar auf 1080p). */
    for (int r = 0; r < CURSOR_H; r++) {
        uint8_t eb = cur_edge[r], bo = cur_body[r];
        for (int sr = 0; sr < CURSOR_SCALE; sr++) {
            int py = cy + r * CURSOR_SCALE + sr;
            if (py < (int)y0 || py >= (int)(y0 + n) || py < 0 || py >= (int)fb->height) { continue; }
            volatile uint32_t *row = (volatile uint32_t *)(fb->base + (uint64_t)py * fb->pitch);
            for (int c = 0; c < 8; c++) {
                uint8_t m = (uint8_t)(0x80u >> c);
                uint32_t col; int draw = 0;
                if (bo & m)      { col = 0x00FFFFFFu; draw = 1; }
                else if (eb & m) { col = 0x00000000u; draw = 1; }
                if (draw) {
                    for (int sc = 0; sc < CURSOR_SCALE; sc++) {
                        int px = cx + c * CURSOR_SCALE + sc;
                        if (px >= 0 && px < (int)fb->width) { row[px] = col; }
                    }
                }
            }
        }
    }
#endif
}

/* Ein Cursor-Zeilenband [y, y+CURSOR_H) neu aus dem Backbuffer aufbauen + Cursor drueber + Cache.
 * Wird mit gehaltenem g_cursor_lock gerufen (Mover im Timer-IRQ / Flush). Klemmt an die FB-Hoehe. */
static void refresh_band(const fb_t *fb, int y)
{
    if (y < 0 || (uint32_t)y >= fb->height) { return; }
    uint32_t n = CURSOR_PH;
    if (n > fb->height - (uint32_t)y) { n = fb->height - (uint32_t)y; }
    copy_rows(fb, (uint32_t)y, n);
    draw_cursor(fb, (uint32_t)y, n);
    fb_flush_rows((uint32_t)y, n);
}

/* Cursor auf (x,y) bewegen -- aus dem Timer-IRQ (Kern 0) bei Mausbewegung. Loescht den zuletzt
 * gezeichneten Cursor (Band aus dem Backbuffer restaurieren) und zeichnet ihn am neuen Ort.
 * Nimmt g_cursor_lock NUR per trylock: flusht die App gerade (Lock besetzt), wird die neue
 * Position bloss gemerkt (der App-Flush bzw. der naechste Tick komponiert sie) -- der IRQ
 * blockiert nie. */
void gui_fb_move_cursor(int x, int y)
{
    const fb_t *fb = fb_get();
    if (!s_ready || !fb || !fb->base) { return; }
    if (!spin_trylock(&g_cursor_lock)) {
        g_cursor_x = x; g_cursor_y = y; g_cursor_on = 1;    /* FB busy -> nur Wunschposition merken */
        return;
    }
    int old_y = g_drawn_y, old_on = g_drawn_on;
    g_cursor_x = x; g_cursor_y = y; g_cursor_on = 1;
    if (old_on && old_y != y) { refresh_band(fb, old_y); }  /* alten Cursor loeschen (getrenntes Band) */
    refresh_band(fb, y);                                     /* neuen Cursor zeichnen (deckt auch old_y==y) */
    g_drawn_y = y; g_drawn_on = 1;
    spin_unlock(&g_cursor_lock);
}

int gui_fb_init(void)
{
    const fb_t *fb = fb_get();
    if (!fb || !fb->base) { return -1; }
    if ((uint64_t)fb->height * fb->pitch > GUI_BB_SIZE) { return -1; }  /* FB groesser als Backbuffer */

    /* gui_fb_flush schreibt fb->base ueber die Identity-VA -- die im GUI-Prozess (aktives TTBR0)
     * auf den Backbuffer umgebogen ist. Faellt die (firmware-gewaehlte) FB-Basis in das gemappte
     * Fenster [GUI_FB_USER_VA, +2MiB), traefe der Flush den Backbuffer statt den echten FB (stiller
     * Fehlschlag). Bei GUI_FB_USER_VA=0x18000000 kollidiert der VC-Carveout nie; Guard fuer HW. */
    uint64_t fb_base = (uint64_t)(uintptr_t)fb->base;
    if (fb_base < GUI_FB_USER_VA + GUI_BB_SIZE && fb_base + fb->size > GUI_FB_USER_VA) { return -1; }

    s_ready = 1;
    return 0;
}

int gui_fb_info(gui_fb_info_t *out)
{
    const fb_t *fb = fb_get();
    if (!s_ready || !fb || !fb->base || !out) { return -1; }
    out->width    = fb->width;
    out->height   = fb->height;
    out->pitch    = fb->pitch;
    out->bpp      = 32;
    out->fb_va    = GUI_FB_USER_VA;
    out->fb_bytes = (uint64_t)fb->height * fb->pitch;
    return 0;
}

int gui_fb_flush(uint32_t y, uint32_t nrows)
{
    const fb_t *fb = fb_get();
    if (!s_ready || !fb || !fb->base) { return -1; }
    if (y >= fb->height) { return 0; }
    if (nrows > fb->height - y) { nrows = fb->height - y; }

    copy_rows(fb, y, nrows);                                 /* lange BB->FB-Kopie OHNE Sperre (IRQs frei -> RT) */

    /* Nur das Cursor-Sprite unter g_cursor_lock -> gegen den Mover serialisiert (kurz). Der Mover
     * nimmt es per trylock, blockiert also nie im IRQ; kein Selbst-Deadlock, falls der Timer waehrend
     * des gehaltenen Locks feuert. Die BG-Kopie oben teilt sich mit dem Mover nur identische
     * Backbuffer-Pixel -> ein Ueberlappungs-Wettlauf ist optisch belanglos und heilt beim naechsten Tick. */
    spin_lock(&g_cursor_lock);
    draw_cursor(fb, y, nrows);
    spin_unlock(&g_cursor_lock);

    fb_flush_rows(y, nrows);                                 /* dc cvac auf den FB -> GPU sieht es */
    return 0;
}

#ifdef RTOS_SELFTEST
int gui_fb_selftest(void)
{
    const fb_t *fb = fb_get();
    if (!s_ready || !fb || !fb->base) { return 0; }
    uint32_t          *bb = (uint32_t *)(void *)g_gui_bb;
    volatile uint32_t *fp = (volatile uint32_t *)fb->base;
    uint32_t px = fb->pitch / 4;

    /* Aktuelle FB-Zeile 0 in den Backbuffer spiegeln, damit der Test-Flush nur die 4 Marker-
     * Pixel aendert (statt die Banner-Zeile zu schwaerzen). */
    for (uint32_t x = 0; x < px; x++) { bb[x] = fp[x]; }
    bb[0] = 0x123456; bb[1] = 0xABCDEF; bb[2] = 0x00FF00; bb[3] = 0xFFFFFF;

    gui_fb_flush(0, 1);                                       /* Backbuffer-Zeile 0 -> echter FB */

    return (fp[0] == 0x123456 && fp[1] == 0xABCDEF &&
            fp[2] == 0x00FF00 && fp[3] == 0xFFFFFF) ? 1 : 0;  /* echte FB-Pixel zurueckgelesen */
}

int gui_fb_cursor_selftest(void)
{
    const fb_t *fb = fb_get();
    if (!s_ready || !fb || !fb->base) { return 0; }
    uint32_t          *bb  = (uint32_t *)(void *)g_gui_bb;
    volatile uint32_t *fp  = (volatile uint32_t *)fb->base;
    uint32_t           wpr = fb->pitch / 4;

    /* Zwei getrennte Testbaender (50..61 und 100..111) mit BG fuellen -- der Backbuffer bleibt
     * cursor-frei. Der Cursor wird via move_cursor DIREKT in den FB komponiert (copy_rows aus dem
     * frischen BB-Band + Overlay); der zweite Move beweist zusaetzlich das Loeschen der alten
     * Position (Geist-Test -- genau der Partial-Flush-Fehler aus dem T2.3-Review). */
    for (int r = 0; r < CURSOR_H; r++) {
        uint32_t *r1 = bb + (uint32_t)(50  + r) * wpr;
        uint32_t *r2 = bb + (uint32_t)(100 + r) * wpr;
        for (uint32_t x = 0; x < fb->width; x++) { r1[x] = 0x001830; r2[x] = 0x001830; }
    }
    g_drawn_on = 0;                           /* frischer Start: keine alte Position zu loeschen */
    gui_fb_move_cursor(50, 50);

    uint32_t a_edge = fp[50u * wpr + 50u];    /* (50,50): Kontur (edge row0 col0) -> schwarz  */
    uint32_t a_body = fp[51u * wpr + 50u];    /* (50,51): Fuellung (body row1 col0) -> weiss   */
    uint32_t a_bg   = fp[50u * wpr + 56u];    /* (56,50): ausserhalb des Cursors -> BG bleibt  */

    gui_fb_move_cursor(50, 100);              /* Cursor nach (50,100) -> altes Band (Zeile 50) loeschen */
    uint32_t ghost  = fp[50u * wpr + 50u];    /* GEIST-Test: alte Cursor-Pos muss wieder reines BG sein */
    uint32_t b_edge = fp[100u * wpr + 50u];   /* neue Pos: Kontur -> schwarz */
    uint32_t b_body = fp[101u * wpr + 50u];   /* neue Pos: Fuellung -> weiss */

    g_cursor_on = 0; g_drawn_on = 0;          /* bis die echte Maus ihn wieder aktiviert */

    return (a_edge == 0x00000000u && a_body == 0x00FFFFFFu && a_bg == 0x00001830u &&
            ghost  == 0x00001830u &&                              /* kein Geist an alter Position */
            b_edge == 0x00000000u && b_body == 0x00FFFFFFu) ? 1 : 0;
}
#endif
