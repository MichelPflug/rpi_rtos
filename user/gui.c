/*
 * user/gui.c  --  GUI.ELF: die WinForms-artige GUI-Sitzung nach dem Login
 *
 * Eine "schoene" Demo, die ALLE verfuegbaren Controls zeigt und interaktiv verdrahtet:
 *   - Panel   : Kopfleiste + drei Gruppen-Container + eine Farb-Vorschau
 *   - Label   : Titel, Abschnitts-Ueberschriften, Feldbeschriftungen, dynamischer Zaehler + Status
 *   - TextBox : zwei Eingabefelder (Name/Ort), per Tastatur editierbar (Caret)
 *   - Button  : Zaehler +/-/Reset, Farbe Rot/Gruen/Blau, Eingaben loeschen, Beenden
 * Zaehler/Status/Vorschaufarbe werden von den Click-Handlern zur Laufzeit aktualisiert (Retained-
 * Mode: Handler aendern den Zustand + markieren dirty -> die Message-Loop zeichnet neu).
 *
 * Ein deterministischer Selbsttest (konstruierte Events) beweist die Bedienung headless; danach
 * uebernimmt wf_run die echte Message-Loop (Maus + Tastatur ueber die Kernel-Event-Queue). Der
 * Beenden-Button schliesst die Sitzung.
 */
#include "abi.h"
#include "ulib.h"
#include "gui.h"
#include "winforms.h"

/* --- kleine Ausgabe-Helfer (Serial-Marker) --- */
static char lbuf[160];
static int  lpos;
static void lflush(void) { if (lpos > 0) { sys3(SYS_WRITE, 1, (long)lbuf, lpos); lpos = 0; } }
static void uwrite(const char *s)
{
    for (; *s; ++s) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = *s; } if (*s == '\n') { lflush(); } }
}

/* Integer -> Dezimalstring (kein libc). */
static void fmt_int(char *buf, int v)
{
    char tmp[12]; int i = 0, p = 0, neg = (v < 0);
    if (neg) { v = -v; }
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    if (neg) { buf[p++] = '-'; }
    while (i > 0) { buf[p++] = tmp[--i]; }
    buf[p] = 0;
}

/* --- gemeinsamer Sitzungszustand (die Click-Handler greifen darauf zu) --- */
static wf_form_t    *g_form;
static wf_control_t *g_lblCount;     /* zeigt g_countStr */
static wf_control_t *g_lblStatus;    /* zeigt g_status   */
static wf_control_t *g_preview;      /* Farb-Vorschau-Panel */
static wf_control_t *g_tbName, *g_tbCity;
static char          g_countStr[12] = "0";
static int           g_count;
static const char   *g_status = "Bereit.";

/* Eingebetteter, aus TrueType gerasterter Font (tools/gen_font.py -> user/lib/font_dejavu_sans.c). */
extern const unsigned char font_dejavu_sans[];
extern const unsigned int  font_dejavu_sans_len;
static gui_font_t g_font;

static void refresh_count(void) { fmt_int(g_countStr, g_count); }
static void set_status(const char *s) { g_status = s; if (g_lblStatus) { g_lblStatus->text = s; } }
static void mark(void) { if (g_form) { g_form->dirty = 1; } }

static void on_plus(wf_control_t *c)  { (void)c; g_count++; refresh_count(); set_status("Zaehler erhoeht.");        mark(); }
static void on_minus(wf_control_t *c) { (void)c; g_count--; refresh_count(); set_status("Zaehler verringert.");     mark(); }
static void on_reset(wf_control_t *c) { (void)c; g_count = 0; refresh_count(); set_status("Zaehler zurueckgesetzt."); mark(); }
static void on_red(wf_control_t *c)   { (void)c; if (g_preview) { g_preview->back = 0xC03030; } set_status("Farbe: Rot.");   mark(); }
static void on_green(wf_control_t *c) { (void)c; if (g_preview) { g_preview->back = 0x30A050; } set_status("Farbe: Gruen."); mark(); }
static void on_blue(wf_control_t *c)  { (void)c; if (g_preview) { g_preview->back = 0x3060C0; } set_status("Farbe: Blau.");  mark(); }
static void on_clear(wf_control_t *c)
{
    (void)c;
    if (g_tbName && g_tbName->buf) { g_tbName->buf[0] = 0; g_tbName->buflen = 0; }
    if (g_tbCity && g_tbCity->buf) { g_tbCity->buf[0] = 0; g_tbCity->buflen = 0; }
    set_status("Eingaben geloescht.");
    mark();
}
static void on_exit(wf_control_t *c)  { (void)c; if (g_form) { wf_close(g_form); } }

/* --- Farben --- */
/* Klassische WinForms-Farben: graue Form + Steuerelemente, blaue Titelleiste, schwarzer Text. */
#define BG_FORM   0xD4D0C8u   /* Steuerelement-Grau (Form-Hintergrund) */
#define TITLE_BAR 0x0A246Au   /* klassische aktive Titelleiste (blau) */
#define COL_TITLE 0xFFFFFFu   /* Titelleisten-Text weiss */
#define COL_SECT  0x000080u   /* Abschnitts-Ueberschrift (dunkles Marineblau) */
#define COL_TEXT  0x000000u   /* Fenstertext (schwarz) */
#define PREVIEW0  0x4A6A8Au   /* Vorschau-Startfarbe (versenktes Feld) */

static gui_event_t mk_mouse(int x, int y, unsigned b)
{
    gui_event_t e = { 0, 0, 0, 0, 0, 0 };
    e.type = GUI_EV_MOUSE; e.buttons = (unsigned char)b; e.x = (short)x; e.y = (short)y;
    return e;
}
static gui_event_t mk_key(char k)
{
    gui_event_t e = { 0, 0, 0, 0, 0, 0 };
    e.type = GUI_EV_KEY; e.key = (unsigned char)k;
    return e;
}

static char g_bufName[32];
static char g_bufCity[32];
static unsigned char g_ttf_buf[16384];    /* geladene Subset-.ttf (DejaVu Sans ASCII, ~10 KB); bss, nicht im ELF */
static unsigned char g_font_arena[65536]; /* Ziel fuer die zur Laufzeit gerasterten Glyphen (.rfn) */

void _start(void)
{
    gui_t g;
    if (gui_init(&g) != 0) {
        uwrite("[gui] gui_init fehlgeschlagen (keine Bruecke/GUI-Cap)\n");
        sys3(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    g_bufName[0] = 0; g_bufCity[0] = 0;

    {
        int fp = gui_ttf_fp_smoke();
        uwrite(fp == 778 ? "[gui] el0-fp: ok (Fliesskomma am EL0 laeuft, smoke=778)\n"
                         : "[gui] el0-fp: FALSCHES Ergebnis\n");
    }

    /* die ASCII-Subset-.ttf zur LAUFZEIT von hdd1 laden (die Originale sind ~14 MB; das Subset
     * ~9 KB passt in den Prozess), SFNT-Magic pruefen, dann mit FP zur Laufzeit rastern (jede Groesse).
     * Fallback auf den beim Build gerasterten .rfn, falls Laden/Parsen scheitert. */
    int n = (int)sys3(SYS_READ_FILE, (long)"hdd1:GUIFONT.TTF", (long)g_ttf_buf, (long)sizeof(g_ttf_buf));
    int magic_ok = (n > 100 && n <= (int)sizeof(g_ttf_buf) &&
                    g_ttf_buf[0] == 0x00 && g_ttf_buf[1] == 0x01 &&
                    g_ttf_buf[2] == 0x00 && g_ttf_buf[3] == 0x00);       /* sfntVersion 0x00010000 */
    { char nb[12]; fmt_int(nb, n);
      uwrite(magic_ok ? "[gui] ttf-datei: hdd1:GUIFONT.TTF geladen + SFNT-Magic ok (" : "[gui] ttf-datei: FEHLER (");
      uwrite(nb); uwrite(" B)\n"); }

    int runtime = (magic_ok &&
        gui_font_rasterize_ttf(&g_font, g_ttf_buf, (unsigned)n, 14, g_font_arena, (unsigned)sizeof(g_font_arena)) == 0 &&
        g_font.line_height > 0);
    if (!runtime) {   /* Fallback: eingebetteter, beim Build gerasterter .rfn */
        gui_font_from_mem(&g_font, font_dejavu_sans, font_dejavu_sans_len);
    }
    wf_set_font(&g_font);

    /* Selbsttest: "Wg" anti-aliased auf schwarzem Grund -> ein helles Pixel muss entstehen. */
    int ttf_ok = 0;
    gui_fill_rect(&g, 0, 460, 44, 20, 0x000000);
    gui_text_ttf(&g, 3, 461, "Wg", &g_font, 0xFFFFFF, 0x000000, 44);
    for (int yy = 460; yy < 480 && !ttf_ok; yy++) {
        for (int xx = 3; xx < 40; xx++) {
            unsigned p = gui_get(&g, xx, yy);
            if (((p >> 16) & 0xFF) > 0xC0 && (p & 0xFF) > 0xC0) { ttf_ok = 1; break; }
        }
    }
    uwrite(ttf_ok ? "[gui] ttf-font: DejaVu Sans geladen (ok)\n"
                  : "[gui] ttf-font: NICHT geladen\n");
    uwrite(runtime ? "[gui] ttf-modus: LAUFZEIT-Rasterung aus der .ttf (14px, FP)\n"
                   : "[gui] ttf-modus: Fallback auf eingebetteten .rfn\n");

    /* Formcheck: das gerasterte 'o' muss ein Loch (Counter) haben -> beweist korrekten Nonzero-Winding-
     * Fill (ein Fill-Bug ergaebe ein VOLLES 'o'). Ink-Bounding-Box finden, Mitte muss dunkel sein. */
    {
        gui_fill_rect(&g, 0, 438, 26, 26, 0x000000);
        gui_text_ttf(&g, 4, 439, "o", &g_font, 0xFFFFFF, 0x000000, 26);
        int minx = 99, miny = 99, maxx = -1, maxy = -1;
        for (int yy = 438; yy < 464; yy++) {
            for (int xx = 0; xx < 26; xx++) {
                if (((gui_get(&g, xx, yy) >> 16) & 0xFF) > 0x80) {
                    if (xx < minx) { minx = xx; } if (xx > maxx) { maxx = xx; }
                    if (yy < miny) { miny = yy; } if (yy > maxy) { maxy = yy; }
                }
            }
        }
        int hole = 0;
        if (maxx > minx + 3 && maxy > miny + 3) {
            unsigned pc = gui_get(&g, (minx + maxx) / 2, (miny + maxy) / 2);
            hole = (((pc >> 16) & 0xFF) < 0x60);         /* Mitte dunkel = Counter-Loch vorhanden */
        }
        uwrite(hole ? "[gui] ttf-form: 'o' mit Loch gerastert (Nonzero-Winding-Fill korrekt)\n"
                    : "[gui] ttf-form: 'o' OHNE Loch (Fill-Fehler)\n");
    }

    wf_form_t form;
    g_form = &form;
    wf_form_init(&form, &g, BG_FORM, 0);   /* eigener Titel-Balken statt Form-Titel */
    wf_control_t *pg, *tl;

    /* --- Titelleiste (flach, blau) --- */
    wf_add_panel(&form, 0, 0, 640, 34, TITLE_BAR);
    tl = wf_add_label(&form, 12, 8, 420, 18, "rpi_rtos  -  WinForms GUI-Demo", COL_TITLE);
    if (tl) { tl->back = TITLE_BAR; }      /* Text auf der blauen Leiste blenden (AA gegen Blau) */

    /* --- GroupBox 1: Eingabe (TextBoxen) --- */
    pg = wf_add_panel(&form, 16, 48, 300, 176, BG_FORM); if (pg) { pg->style = WF_STYLE_ETCHED; }
    wf_add_label(&form, 28, 58,  260, 12, "Eingabe (Tastatur, Tab wechselt):", COL_SECT);
    wf_add_label(&form, 28, 92,  56,  12, "Name:", COL_TEXT);
    g_tbName = wf_add_textbox(&form, 92, 86, 208, 22, g_bufName, (int)sizeof(g_bufName));
    wf_add_label(&form, 28, 128, 56,  12, "Ort:", COL_TEXT);
    g_tbCity = wf_add_textbox(&form, 92, 122, 208, 22, g_bufCity, (int)sizeof(g_bufCity));
    wf_add_button(&form, 28, 158, 140, 26, "Eingaben loeschen", on_clear);

    /* --- GroupBox 2: Zaehler + Aktionen (Buttons + dynamisches Label) --- */
    pg = wf_add_panel(&form, 324, 48, 300, 176, BG_FORM); if (pg) { pg->style = WF_STYLE_ETCHED; }
    wf_add_label(&form, 336, 58, 260, 12, "Zaehler & Aktionen (Buttons):", COL_SECT);
    wf_add_label(&form, 336, 94, 56,  12, "Wert:", COL_TEXT);
    g_lblCount = wf_add_label(&form, 396, 94, 80, 12, g_countStr, COL_TEXT);
    wf_add_button(&form, 336, 114, 56, 28, "+",     on_plus);
    wf_add_button(&form, 400, 114, 56, 28, "-",     on_minus);
    wf_add_button(&form, 464, 114, 96, 28, "Reset", on_reset);
    wf_add_label(&form, 336, 160, 260, 12, "Farbe der Vorschau unten:", COL_SECT);
    wf_add_button(&form, 336, 180, 72, 28, "Rot",   on_red);
    wf_add_button(&form, 414, 180, 72, 28, "Gruen", on_green);
    wf_add_button(&form, 492, 180, 72, 28, "Blau",  on_blue);

    /* --- GroupBox 3: Vorschau (versenktes Farbfeld) --- */
    pg = wf_add_panel(&form, 16, 236, 608, 150, BG_FORM); if (pg) { pg->style = WF_STYLE_ETCHED; }
    wf_add_label(&form, 28, 246, 460, 12, "Vorschau (die Farb-Buttons aendern dieses Feld):", COL_SECT);
    g_preview = wf_add_panel(&form, 28, 266, 584, 104, PREVIEW0); if (g_preview) { g_preview->style = WF_STYLE_SUNKEN; }

    /* --- Statuszeile + Beenden --- */
    g_lblStatus = wf_add_label(&form, 16, 400, 480, 12, g_status, COL_TEXT);
    wf_add_button(&form, 512, 396, 112, 28, "Beenden", on_exit);

    wf_paint(&form);
    uwrite("[gui] Sitzung gestartet: Form 'rpi_rtos GUI' (WinForms-Demo: Panel/Label/TextBox/Button)\n");

    /* --- deterministischer Bedienungs-Selbsttest (konstruierte Events) --- */
    gui_event_t d = mk_mouse(120, 98, 0x01); wf_handle(&form, &d);   /* Name-TextBox anklicken */
    gui_event_t u = mk_mouse(120, 98, 0x00); wf_handle(&form, &u);
    int focus_ok = (form.focused == g_tbName);
    gui_event_t k;
    k = mk_key('h'); wf_handle(&form, &k);
    k = mk_key('i'); wf_handle(&form, &k);
    int type_ok = (g_tbName->buflen == 2 && g_bufName[0] == 'h' && g_bufName[1] == 'i');
    k = mk_key('\t'); wf_handle(&form, &k);                          /* Tab -> naechstes fokussierbares Control */
    int nav_ok = (form.focused != g_tbName && form.focused != 0);
    d = mk_mouse(360, 132, 0x01); wf_handle(&form, &d);             /* "+"-Button anklicken (fokussieren) */
    u = mk_mouse(360, 132, 0x00); wf_handle(&form, &u);
    int base = g_count;
    k = mk_key('\r'); wf_handle(&form, &k);                          /* Enter auf "+" -> Zaehler++ */
    int click_ok = (g_count == base + 1);

    uwrite("[gui] demo selftest fokus=");
    uwrite(focus_ok ? "ok" : "FEHLT");
    uwrite(" tastatur=");
    uwrite(type_ok ? "ok" : "FEHLT");
    uwrite(" nav=");
    uwrite(nav_ok ? "ok" : "FEHLT");
    uwrite(" klick=");
    uwrite(click_ok ? "ok" : "FEHLT");
    uwrite("\n");

    /* --- sauberer Start fuer die echte Sitzung: Selbsttest-Spuren verwerfen --- */
    g_bufName[0] = 0; g_tbName->buflen = 0;
    g_bufCity[0] = 0; g_tbCity->buflen = 0;
    g_count = 0; refresh_count();
    set_status("Bereit -- Maus im Fenster, Tastatur im Terminal. 'Beenden' schliesst.");
    form.focused = g_tbName;
    form.dirty = 1;
    wf_paint(&form);
    uwrite("[gui] bereit fuer Eingaben (Maus + Tastatur ueber die Kernel-Event-Queue)\n");

    wf_run(&form);                                                   /* echte Message-Loop bis Beenden */

    uwrite("[gui] Sitzung beendet\n");
    sys3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
