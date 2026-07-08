/*
 * user/lib/winforms.c  --  WinForms-artiger Retained-Mode-Kern + Standard-Controls 
 * Siehe winforms.h.  Reine EL0-Schicht: Zeichnen ueber libgui, Eingabe ueber SYS_POLL/WAIT_EVENT.
 * Controls liegen in einem statischen Pool (kein Heap/libc). Click wie WinForms (linke Taste
 * Down+Up ueber demselben Control); Tasten gehen an die fokussierte TextBox.
 */
#include "winforms.h"
#include "ulib.h"
#include "abi.h"

/* --- Klassische WinForms-3D-Palette (System-Farben) --- */
#define WF_FACE      0xD4D0C8u   /* ButtonFace / Steuerelement-Grau (Buttons, Form-Hintergrund) */
#define WF_LIGHT     0xE8E5DEu   /* 3DLight (innere helle Kante) */
#define WF_HILIGHT   0xFFFFFFu   /* ButtonHighlight (aeussere helle Kante) */
#define WF_SHADOW    0x808080u   /* ButtonShadow (innere dunkle Kante) */
#define WF_DKSHADOW  0x404040u   /* ButtonDarkShadow (aeussere dunkle Kante) */
#define WF_WINTEXT   0x000000u   /* Fenstertext (schwarz) */
#define WF_TEXT_INSET    3           /* linksbuendiger Text: Pixel-Einrueckung je Seite */

/* --- statischer Control-Pool (fuer alle Forms dieser App) --- */
static wf_control_t g_pool[WF_MAX_CONTROLS];
static int          g_pool_n;

/* Optionaler proportionaler TTF-Font (0 = fester 8x8-Font). */
static const gui_font_t *g_wf_font;
void wf_set_font(const gui_font_t *font) { g_wf_font = font; }

static int wf_strlen(const char *s)
{
    int n = 0;
    if (s) { while (s[n]) { n++; } }
    return n;
}

/* Ein 2px-3D-Bevel um [x,y,w,h] zeichnen: aeussere + innere Kante, oben/links (tl) vs unten/rechts
 * (br). Reihenfolge oben,links,unten,rechts -> unten/rechts besitzen ihre Ecken (klassischer Look). */
static void wf_bevel(const gui_t *g, int x, int y, int w, int h,
                     unsigned tl_o, unsigned br_o, unsigned tl_i, unsigned br_i)
{
    if (w < 2 || h < 2) { return; }
    int x2 = x + w - 1, y2 = y + h - 1;
    gui_hline(g, x, y, w, tl_o);                 /* aussen oben  */
    gui_vline(g, x, y, h, tl_o);                 /* aussen links */
    gui_hline(g, x, y2, w, br_o);                /* aussen unten */
    gui_vline(g, x2, y, h, br_o);                /* aussen rechts*/
    if (w < 4 || h < 4) { return; }
    gui_hline(g, x + 1, y + 1, w - 2, tl_i);     /* innen oben  */
    gui_vline(g, x + 1, y + 1, h - 2, tl_i);     /* innen links */
    gui_hline(g, x + 1, y2 - 1, w - 2, br_i);    /* innen unten */
    gui_vline(g, x2 - 1, y + 1, h - 2, br_i);    /* innen rechts*/
}

/* Gepunktetes Fokus-Rechteck (klassischer WinForms-Fokusrahmen), jeder 2. Pixel gesetzt. */
static void wf_focus_rect(const gui_t *g, int x, int y, int w, int h, unsigned col)
{
    if (w < 2 || h < 2) { return; }
    int x2 = x + w - 1, y2 = y + h - 1;
    for (int i = x; i <= x2; i += 2) { gui_pixel(g, i, y, col); gui_pixel(g, i, y2, col); }
    for (int i = y; i <= y2; i += 2) { gui_pixel(g, x, i, col); gui_pixel(g, x2, i, col); }
}

/* Text in [x0..x0+boxw) zeichnen, auf die Control-Rect gekappt (libgui clippt nur gegen den Screen).
 * center: horizontal zentrieren. bg: Hintergrundfarbe (fuer das Anti-Aliasing des TTF-Fonts). Ohne
 * gesetzten TTF-Font faellt es auf den festen 8x8-Font zurueck (transparenter Hintergrund). */
static void wf_draw_text(const gui_t *g, int x0, int y0, int boxw, int boxh,
                         const char *s, unsigned fg, unsigned bg, int inset, int center)
{
    if (g_wf_font) {                                   /* proportionaler TTF-Font (anti-aliased) */
        int avail = boxw - 2 * inset; if (avail < 0) { avail = 0; }
        int tw = gui_text_ttf_width(g_wf_font, s);
        if (tw > avail) { tw = avail; }                /* fuer die Zentrierung auf die Box begrenzen */
        int tx = center ? x0 + (boxw - tw) / 2 : x0 + inset;
        if (tx < x0 + inset) { tx = x0 + inset; }
        int ty = y0 + (boxh - g_wf_font->line_height) / 2;
        if (ty < y0) { ty = y0; }
        gui_text_ttf(g, tx, ty, s, g_wf_font, fg, bg, x0 + boxw - inset);
        return;
    }
    (void)bg;                                          /* 8x8-Font: transparenter Hintergrund */
    int avail = boxw - 2 * inset;
    if (avail < 0) { avail = 0; }
    int maxch = avail / GUI_GLYPH_W;
    int len   = wf_strlen(s);
    int nch   = (len < maxch) ? len : maxch;
    int tw    = nch * GUI_GLYPH_W;
    int tx    = center ? x0 + (boxw - tw) / 2 : x0 + inset;
    if (tx < x0 + inset) { tx = x0 + inset; }
    int ty = y0 + (boxh - GUI_GLYPH_H) / 2;
    if (ty < y0) { ty = y0; }
    for (int i = 0; i < nch; i++) {
        gui_char(g, tx + i * GUI_GLYPH_W, ty, s[i], fg, GUI_TRANSPARENT, 1);
    }
}

/* --- per-Typ Paint --- (surface_bg = Hintergrundfarbe hinter dem Control, fuers Text-Anti-Aliasing) */
static void wf_paint_label(const gui_t *g, const wf_control_t *c, int focused, unsigned surface_bg)
{
    (void)focused;
    unsigned bg = surface_bg;
    if (c->back != GUI_TRANSPARENT) { gui_fill_rect(g, c->x, c->y, c->w, c->h, c->back); bg = c->back; }
    if (c->text) { wf_draw_text(g, c->x, c->y, c->w, c->h, c->text, c->fore, bg, WF_TEXT_INSET, 0); }
}

static void wf_paint_button(const gui_t *g, const wf_control_t *c, int focused, unsigned surface_bg)
{
    (void)surface_bg;
    int off = c->pressed ? 1 : 0;                /* gedrueckt: Inhalt 1px nach unten/rechts */
    gui_fill_rect(g, c->x, c->y, c->w, c->h, c->back);
    if (c->pressed) {                            /* versenkt */
        wf_bevel(g, c->x, c->y, c->w, c->h, WF_DKSHADOW, WF_HILIGHT, WF_SHADOW, WF_LIGHT);
    } else {                                     /* erhaben */
        wf_bevel(g, c->x, c->y, c->w, c->h, WF_HILIGHT, WF_DKSHADOW, WF_LIGHT, WF_SHADOW);
    }
    if (c->text) { wf_draw_text(g, c->x + off, c->y + off, c->w, c->h, c->text, c->fore, c->back, WF_TEXT_INSET, 1); }
    if (focused && c->w > 12 && c->h > 12) {     /* gepunktetes Fokus-Rechteck (WinForms) */
        wf_focus_rect(g, c->x + 3 + off, c->y + 3 + off, c->w - 6, c->h - 6, WF_WINTEXT);
    }
}

static void wf_paint_textbox(const gui_t *g, const wf_control_t *c, int focused, unsigned surface_bg)
{
    (void)surface_bg;
    gui_fill_rect(g, c->x, c->y, c->w, c->h, c->back);   /* weisses Eingabefeld */
    /* versenkter Client-Rand (klassisches EDIT-Control) */
    wf_bevel(g, c->x, c->y, c->w, c->h, WF_SHADOW, WF_HILIGHT, WF_DKSHADOW, WF_FACE);
    const char *s = c->buf ? c->buf : "";
    wf_draw_text(g, c->x, c->y, c->w, c->h, s, c->fore, c->back, WF_TEXT_INSET, 0);
    if (focused) {
        /* Caret hinter dem letzten Zeichen; Position aus der echten Textbreite (TTF proportional!),
         * vollstaendig auf die Control-Rect geklemmt (libgui clippt nur gg. Screen). */
        int cx, ch;
        if (g_wf_font) {
            cx = c->x + WF_TEXT_INSET + gui_text_ttf_width(g_wf_font, s);
            ch = g_wf_font->line_height;
        } else {
            int avail = c->w - 2 * WF_TEXT_INSET; if (avail < 0) { avail = 0; }
            int maxch = avail / GUI_GLYPH_W;
            int col   = (c->buflen < maxch) ? c->buflen : maxch;
            cx = c->x + WF_TEXT_INSET + col * GUI_GLYPH_W;
            ch = GUI_GLYPH_H;
        }
        if (cx > c->x + c->w - 1) { cx = c->x + c->w - 1; }
        if (ch > c->h) { ch = c->h; }
        int cy = c->y + (c->h - ch) / 2; if (cy < c->y) { cy = c->y; }
        if (ch > 0) { gui_vline(g, cx, cy, ch, c->fore); }
    }
}

static void wf_paint_panel(const gui_t *g, const wf_control_t *c, int focused, unsigned surface_bg)
{
    (void)focused; (void)surface_bg;
    if (c->back != GUI_TRANSPARENT) { gui_fill_rect(g, c->x, c->y, c->w, c->h, c->back); }
    switch (c->style) {
        case WF_STYLE_RAISED:                    /* erhaben */
            wf_bevel(g, c->x, c->y, c->w, c->h, WF_HILIGHT, WF_DKSHADOW, WF_LIGHT, WF_SHADOW); break;
        case WF_STYLE_SUNKEN:                    /* versenkt (Bild-/Vorschaufeld) */
            wf_bevel(g, c->x, c->y, c->w, c->h, WF_SHADOW, WF_HILIGHT, WF_DKSHADOW, WF_FACE); break;
        case WF_STYLE_ETCHED:                    /* geaetzte Nut (GroupBox) */
            wf_bevel(g, c->x, c->y, c->w, c->h, WF_SHADOW, WF_HILIGHT, WF_HILIGHT, WF_SHADOW); break;
        default: break;                          /* WF_STYLE_FLAT: nur Fuellung */
    }
}

static void wf_paint_control(const gui_t *g, const wf_control_t *c, int focused, unsigned surface_bg)
{
    switch (c->kind) {
        case WF_LABEL:   wf_paint_label(g, c, focused, surface_bg);   break;
        case WF_TEXTBOX: wf_paint_textbox(g, c, focused, surface_bg); break;
        case WF_PANEL:   wf_paint_panel(g, c, focused, surface_bg);   break;
        default:         wf_paint_button(g, c, focused, surface_bg);  break;   /* WF_BUTTON */
    }
}

/* --- Pool/Form --- */
void wf_reset(void)
{
    g_pool_n = 0;
}

void wf_form_init(wf_form_t *f, const gui_t *g, unsigned back, const char *title)
{
    f->g            = g;
    f->back         = back;
    f->title        = title;
    f->n            = 0;
    f->focused      = 0;
    f->capture      = 0;
    f->prev_buttons = 0;
    f->running      = 0;
    f->dirty        = 1;
}

/* Gemeinsamer Allokator: Slot aus dem Pool ziehen, Defaults setzen, an die Form haengen. */
static wf_control_t *wf_new(wf_form_t *f, int kind, int x, int y, int w, int h)
{
    if (g_pool_n >= WF_MAX_CONTROLS || f->n >= WF_MAX_CONTROLS) { return 0; }
    wf_control_t *c = &g_pool[g_pool_n++];
    c->kind = kind;
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->back = 0; c->fore = WF_WINTEXT;
    c->text = 0;
    c->visible = 1; c->enabled = 1; c->focusable = 0;
    c->pressed = 0;
    c->style = WF_STYLE_FLAT;
    c->on_click = 0;
    c->buf = 0; c->buflen = 0; c->bufcap = 0;
    c->tag = 0;
    f->ctrl[f->n++] = c;
    return c;
}

wf_control_t *wf_add_label(wf_form_t *f, int x, int y, int w, int h, const char *text, unsigned fore)
{
    wf_control_t *c = wf_new(f, WF_LABEL, x, y, w, h);
    if (!c) { return 0; }
    c->back = GUI_TRANSPARENT;     /* rahmenlos + transparent (WinForms-Label-Default) */
    c->fore = fore;
    c->text = text;
    return c;
}

wf_control_t *wf_add_button(wf_form_t *f, int x, int y, int w, int h,
                            const char *text, wf_click_fn on_click)
{
    wf_control_t *c = wf_new(f, WF_BUTTON, x, y, w, h);
    if (!c) { return 0; }
    c->back      = WF_FACE;        /* graue Steuerelement-Flaeche, schwarzer Text (WinForms) */
    c->fore      = WF_WINTEXT;
    c->text      = text;
    c->focusable = 1;
    c->style     = WF_STYLE_RAISED;
    c->on_click  = on_click;
    return c;
}

wf_control_t *wf_add_textbox(wf_form_t *f, int x, int y, int w, int h, char *buf, int cap)
{
    wf_control_t *c = wf_new(f, WF_TEXTBOX, x, y, w, h);
    if (!c) { return 0; }
    c->back      = 0xFFFFFF;       /* weisses Eingabefeld, schwarzer Text */
    c->fore      = 0x000000;
    c->focusable = 1;
    c->buf       = buf;
    c->bufcap    = cap;
    /* Anfangslaenge SICHER bestimmen: Scan HART auf cap-1 begrenzt. Der Puffer soll laut winforms.h
     * 0-terminiert sein; verletzt der Aufrufer das (nicht terminiert / laenger als cap), darf die Lib
     * trotzdem nicht ueber das Ende hinaus lesen (wf_strlen) oder schreiben (buf[buflen]=0) und nie
     * die Invariante buflen <= bufcap-1 verletzen (sonst schriebe ein spaeteres Backspace OOB). */
    int n = 0;
    if (buf && cap > 0) { while (n < cap - 1 && buf[n]) { n++; } buf[n] = 0; }
    c->buflen = n;
    return c;
}

wf_control_t *wf_add_panel(wf_form_t *f, int x, int y, int w, int h, unsigned back)
{
    wf_control_t *c = wf_new(f, WF_PANEL, x, y, w, h);
    if (!c) { return 0; }
    c->back = back;
    return c;
}

/* --- Hit-Testing / Paint --- */
static int wf_contains(const wf_control_t *c, int x, int y)
{
    /* Als Differenz statt Summe: x < c->x + c->w wuerde bei riesiger App-Breite (c->w nahe INT_MAX)
     * signed overflow (UB) ausloesen. x-c->x ist nach dem ersten Test >= 0 und klein (Event-x ist
     * int16). w/h <= 0 -> leeres Control, nie treffbar (0 < w falsch). */
    return (x >= c->x && x - c->x < c->w && y >= c->y && y - c->y < c->h);
}

wf_control_t *wf_hit_test(wf_form_t *f, int x, int y)
{
    /* Von oben (zuletzt hinzugefuegt = oberste Z-Ordnung) nach unten. */
    for (int i = f->n - 1; i >= 0; i--) {
        wf_control_t *c = f->ctrl[i];
        if (c->visible && c->enabled && wf_contains(c, x, y)) { return c; }
    }
    return 0;
}

void wf_paint(wf_form_t *f)
{
    const gui_t *g = f->g;
    gui_clear(g, f->back);
    if (f->title) {
        if (g_wf_font) { gui_text_ttf(g, 4, 2, f->title, g_wf_font, 0xFFFFFF, f->back, (int)g->width); }
        else           { gui_text(g, 4, 2, f->title, 0xFFFFFF, GUI_TRANSPARENT, 1); }
    }
    for (int i = 0; i < f->n; i++) {
        wf_control_t *c = f->ctrl[i];
        if (c->visible) { wf_paint_control(g, c, c == f->focused, f->back); }
    }
    gui_flush_all(g);
    f->dirty = 0;
}

/* --- Dispatch --- */
static void wf_set_focus(wf_form_t *f, wf_control_t *c)
{
    if (f->focused != c) { f->focused = c; f->dirty = 1; }
}

/* Eine Taste an die fokussierte TextBox anwenden. */
static void wf_textbox_key(wf_form_t *f, char k)
{
    wf_control_t *t = f->focused;
    if (!t || !t->visible || t->kind != WF_TEXTBOX || !t->enabled || !t->buf || t->bufcap <= 0) { return; }
    if (k == 8 || k == 127) {                       /* Backspace / DEL */
        if (t->buflen > 0) { t->buf[--t->buflen] = 0; f->dirty = 1; }
    } else if (k >= 0x20 && k < 0x7F) {             /* druckbares ASCII */
        if (t->buflen < t->bufcap - 1) { t->buf[t->buflen++] = k; t->buf[t->buflen] = 0; f->dirty = 1; }
    }
}

/* Fokus auf das naechste fokussierbare Control weiterschalten (Tab-Reihenfolge = Einfuege-
 * Reihenfolge, mit Umlauf). Ohne aktuellen Fokus beginnt die Suche bei Index 0. */
static void wf_focus_next(wf_form_t *f)
{
    int start = -1;
    for (int i = 0; i < f->n; i++) { if (f->ctrl[i] == f->focused) { start = i; break; } }
    for (int step = 1; step <= f->n; step++) {
        int i = (start + step) % f->n;
        wf_control_t *c = f->ctrl[i];
        if (c->visible && c->enabled && c->focusable) { wf_set_focus(f, c); return; }
    }
}

void wf_handle(wf_form_t *f, const gui_event_t *ev)
{
    if (ev->type == GUI_EV_KEY) {
        char k = (char)ev->key;
        if (k == '\t') {                                /* Tab: naechstes fokussierbares Control */
            if (f->n > 0) { wf_focus_next(f); }
        } else if (k == '\r' || k == '\n') {            /* Enter: fokussierten Button ausloesen */
            wf_control_t *fc = f->focused;
            if (fc && fc->visible && fc->kind == WF_BUTTON && fc->enabled && fc->on_click) { fc->on_click(fc); }
        } else {                                        /* sonst an die fokussierte TextBox */
            wf_textbox_key(f, k);
        }
        return;
    }
    if (ev->type != GUI_EV_MOUSE) { return; }
    int      x    = ev->x, y = ev->y;
    unsigned btn  = ev->buttons;
    unsigned prev = f->prev_buttons;
    unsigned down = btn & ~prev;                        /* neu gedrueckte Tasten */
    unsigned up   = prev & ~btn;                        /* neu losgelassene Tasten */
    wf_control_t *hit = wf_hit_test(f, x, y);

    if (down & 0x01u) {                                 /* linke Taste gedrueckt */
        /* Fokus nur an fokussierbare Controls; Klick ins Leere (Form) nimmt den Fokus weg;
         * Klick auf ein nicht-fokussierbares Control (Label/Panel) laesst den Fokus stehen. */
        if (hit == 0)            { wf_set_focus(f, 0); }
        else if (hit->focusable) { wf_set_focus(f, hit); }
        f->capture = hit;
        if (hit) { hit->pressed = 1; f->dirty = 1; }
    }
    if (up & 0x01u) {                                   /* linke Taste losgelassen */
        wf_control_t *cap = f->capture;
        if (cap) {
            if (cap->pressed) { cap->pressed = 0; f->dirty = 1; }
            /* WinForms-Click nur, wenn Up ueber demselben Control wie Down. */
            if (cap == hit && cap->enabled && cap->on_click) { cap->on_click(cap); }
        }
        f->capture = 0;
    }
    f->prev_buttons = btn;
}

int wf_pump(wf_form_t *f)
{
    gui_event_t ev;
    if (sys3(SYS_POLL_EVENT, (long)&ev, 0, 0) == 1) {
        wf_handle(f, &ev);
        return 1;
    }
    return 0;
}

void wf_close(wf_form_t *f)
{
    f->running = 0;
}

void wf_run(wf_form_t *f)
{
    wf_paint(f);
    f->running = 1;
    while (f->running) {
        gui_event_t ev;
        if (sys3(SYS_WAIT_EVENT, (long)&ev, 0, 0) != 1) { continue; }
        wf_handle(f, &ev);
        if (f->dirty) { wf_paint(f); }
    }
}
