/*
 * user/guitest.c  --  EL0-Test der GUI-Grafik-Bruecke + libgui 
 *
 * Zeichnet mit der geteilten libgui (Rechteck/Rahmen/Text, mit Clipping) in den Backbuffer,
 * prueft charakteristische Pixel per gui_get zurueck (headless verifizierbar) und flusht.
 */
#include "abi.h"
#include "ulib.h"
#include "gui.h"
#include "winforms.h"

static char lbuf[160];
static int  lpos;
static void lflush(void) { if (lpos > 0) { sys3(SYS_WRITE, 1, (long)lbuf, lpos); lpos = 0; } }
static void uwrite(const char *s)
{
    for (; *s; ++s) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = *s; } if (*s == '\n') { lflush(); } }
}
static void uputn(int n)
{
    char tmp[12]; int i = 0;
    if (n < 0) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = '-'; } n = -n; }
    if (n == 0) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = '0'; } return; }
    while (n > 0 && i < 12) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = tmp[--i]; } else { i--; } }
}

#define BG 0x001830u

/* --- WinForms-Kern: Klick-Handler zaehlen Aufrufe (Dispatch-Beweis) --- */
static int g_clicks_A, g_clicks_B, g_nav_clicks;
static void on_click_A(wf_control_t *c)   { (void)c; g_clicks_A++; }
static void on_click_B(wf_control_t *c)   { (void)c; g_clicks_B++; }
static void on_click_nav(wf_control_t *c) { (void)c; g_nav_clicks++; }

/* Ein Maus-Event bauen (absolute Position + Button-Maske). */
static gui_event_t mk_mouse(int x, int y, unsigned buttons)
{
    gui_event_t e = { 0, 0, 0, 0, 0, 0 };
    e.type = GUI_EV_MOUSE; e.buttons = (unsigned char)buttons;
    e.x = (short)x; e.y = (short)y;
    return e;
}

/* Ein Tasten-Event bauen (ASCII). */
static gui_event_t mk_key(char k)
{
    gui_event_t e = { 0, 0, 0, 0, 0, 0 };
    e.type = GUI_EV_KEY; e.key = (unsigned char)k;
    return e;
}

void _start(void)
{
    gui_t g;
    if (gui_init(&g) != 0) {
        uwrite("[guitest] gui_init fehlgeschlagen (keine Bruecke/GUI-Cap)\n");
        sys3(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    gui_clear(&g, BG);

    /* (1) Gefuelltes Rechteck. */
    gui_fill_rect(&g, 100, 100, 60, 40, 0xFF0000);
    int fill_ok = (gui_get(&g, 120, 118) == 0xFF0000u) && (gui_get(&g, 200, 200) == BG);

    /* (2) Rahmen (nur Rand gesetzt, Inneres bleibt Hintergrund). */
    gui_rect(&g, 20, 20, 80, 60, 0x00FF00);
    int rect_ok = (gui_get(&g, 20, 20) == 0x00FF00u) &&      /* Ecke */
                  (gui_get(&g, 99, 79) == 0x00FF00u) &&      /* gegenueberliegende Ecke */
                  (gui_get(&g, 60, 50) == BG);               /* Inneres leer */

    /* (3) Text -- erstes Zeichen 'H' hat in Zeile 0 (0x66) col 1 gesetzt, col 0 leer (MSB-first). */
    gui_text(&g, 300, 100, "Hallo rpi_rtos!", 0xFFFFFF, GUI_TRANSPARENT, 2);
    int text_ok = (gui_get(&g, 300 + 1 * 2, 100) == 0xFFFFFFu) &&   /* 'H' col1 gesetzt (scale 2) */
                  (gui_get(&g, 300 + 0 * 2, 100) == BG);            /* 'H' col0 leer -> bleibt BG */

    /* (4) Clipping: negative + ueberstehende Rechtecke duerfen weder crashen noch OOB schreiben. */
    gui_fill_rect(&g, -10, -10, 25, 25, 0x123456);          /* nur [0,15)x[0,15) sichtbar */
    gui_fill_rect(&g, (int)g.width - 5, (int)g.height - 5, 100, 100, 0x123456);
    int clip_ok = (gui_get(&g, 5, 5) == 0x123456u) &&
                  (gui_get(&g, g.width - 1, g.height - 1) == 0x123456u);

    gui_flush_all(&g);

    uwrite("[guitest] libgui fill=");
    uwrite(fill_ok ? "ok" : "FEHLT");
    uwrite(" rect=");
    uwrite(rect_ok ? "ok" : "FEHLT");
    uwrite(" text=");
    uwrite(text_ok ? "ok" : "FEHLT");
    uwrite(" clip=");
    uwrite(clip_ok ? "ok" : "FEHLT");
    uwrite("\n");

    /* (5) Event-System: der Kernel hat 2 synthetische Maus-Events vorab eingereiht.
     * SYS_WAIT_EVENT liefert das erste (schon vorhanden -> blockiert nicht), SYS_POLL_EVENT den Rest. */
    gui_event_t ev;
    int n = 0, fx = -1, fy = -1, fb = -1;
    if (sys3(SYS_WAIT_EVENT, (long)&ev, 0, 0) == 1) {
        fx = ev.x; fy = ev.y; fb = (int)ev.buttons; n = 1;
    }
    while (n < 8 && sys3(SYS_POLL_EVENT, (long)&ev, 0, 0) == 1) { n++; }
    uwrite("[guitest] events wait+poll=");
    uputn(n);
    uwrite(" first=(");
    uputn(fx); uwrite(","); uputn(fy); uwrite(",btn"); uputn(fb);
    uwrite(")\n");

    /* (6) WinForms-Retained-Kern: Form mit 2 Buttons; Hit-Testing, Klick-Dispatch (Down+Up),
     * Fokus + Neuzeichnen. Deterministisch mit KONSTRUIERTEN Events (der Kernel<->App-Event-Pfad ist
     * separat in (5) bewiesen; wf_run/wf_pump sind duenne SYS_WAIT/POLL_EVENT-Huellen um wf_handle). */
    wf_reset();
    wf_form_t form;
    wf_form_init(&form, &g, 0x202020, "wftest");
    wf_control_t *bA = wf_add_button(&form,  40, 200, 100, 40, "A", on_click_A);
    wf_control_t *bB = wf_add_button(&form, 200, 200, 100, 40, "B", on_click_B);
    /* Schmales Control mit zu langem Text -> muss horizontal auf die Control-Breite gekappt werden
     * (kein Ueberhang nach links in den Formbereich). */
    wf_add_button(&form, 100, 300, 32, 40, "MMMMMMMM", 0);
    wf_paint(&form);

    /* Hit-Testing (rechte/untere Kante exklusiv). */
    int ht_ok = (wf_hit_test(&form,  90, 220) == bA) &&
                (wf_hit_test(&form, 250, 220) == bB) &&
                (wf_hit_test(&form, 160, 220) == 0) &&     /* Luecke zwischen A und B */
                (wf_hit_test(&form, 140, 220) == 0) &&     /* x == A.x+A.w -> exklusiv */
                (bA != 0 && bB != 0);

    /* Klick auf A: Down setzt Fokus+pressed, Up ueber A feuert on_click_A. */
    gui_event_t d = mk_mouse(90, 220, 0x01);   wf_handle(&form, &d);
    int press_ok = (bA->pressed == 1 && form.focused == bA);
    gui_event_t u = mk_mouse(90, 220, 0x00);   wf_handle(&form, &u);
    int click_ok = (g_clicks_A == 1 && g_clicks_B == 0 && bA->pressed == 0);

    /* Neuzeichnen: A traegt jetzt (WinForms) ein gepunktetes Fokus-Rechteck (3px eingerueckt).
     * Buttons sind grau mit erhabenem 3D-Bevel (weisse Kante oben/links) + schwarzem Text. */
    wf_paint(&form);
    /* Text-Clip: der SCHWARZE Text von C (schmal, x=100..131) darf WEDER links (x<100, Klemm-Fix)
     * NOCH rechts (x>=132, Truncation-Fix) ueber die Control-Grenze lecken; im Control selbst muss
     * aber Text stehen (sonst waere !leak trivial). Text vertikal zentriert -> ty=316; Band y=[316,324). */
    int leak = 0, cin = 0;
    for (int yy = 316; yy < 324; yy++) {
        for (int xx = 84;  xx < 100; xx++) { if (gui_get(&g, xx, yy) == 0x000000u) { leak = 1; } }  /* links */
        for (int xx = 132; xx < 164; xx++) { if (gui_get(&g, xx, yy) == 0x000000u) { leak = 1; } }  /* rechts */
        for (int xx = 100; xx < 132; xx++) { if (gui_get(&g, xx, yy) == 0x000000u) { cin = 1; } }
    }
    int paint_ok = (gui_get(&g, 43, 203) == 0x000000u) &&      /* A: gepunktetes Fokus-Rechteck */
                   (gui_get(&g, 40, 200) == 0xFFFFFFu) &&      /* A: erhabener Bevel (weisse Kante o/l) */
                   (gui_get(&g, 203, 203) != 0x000000u) &&     /* B: KEIN Fokus-Rechteck (nicht fokussiert) */
                   (!leak && cin);                             /* C: Text auf Control-Breite gekappt */

    /* Klick daneben (160,220): kein Handler, Fokus verlaesst A. */
    gui_event_t d2 = mk_mouse(160, 220, 0x01); wf_handle(&form, &d2);
    gui_event_t u2 = mk_mouse(160, 220, 0x00); wf_handle(&form, &u2);
    int defocus_ok = (form.focused == 0 && g_clicks_A == 1 && g_clicks_B == 0);

    /* Press auf A, aber Up ueber B (Maus rausgezogen): KEIN Click (WinForms-Semantik). */
    gui_event_t d3 = mk_mouse(90, 220, 0x01);  wf_handle(&form, &d3);
    gui_event_t u3 = mk_mouse(250, 220, 0x00); wf_handle(&form, &u3);
    int cancel_ok = (g_clicks_A == 1 && g_clicks_B == 0 && bA->pressed == 0);

    uwrite("[guitest] winforms hittest=");
    uwrite(ht_ok ? "ok" : "FEHLT");
    uwrite(" click=");
    uwrite((press_ok && click_ok) ? "ok" : "FEHLT");
    uwrite(" focus=");
    uwrite((defocus_ok && cancel_ok) ? "ok" : "FEHLT");
    uwrite(" paint=");
    uwrite(paint_ok ? "ok" : "FEHLT");
    uwrite("\n");

    /* (7) T2.5 Standard-Controls: Panel (Fuellung+Rahmen), Label (transparent, linksbuendig),
     * TextBox (Klick-Fokus -> Tastatur -> Caret). Deterministisch mit konstruierten Events. */
    wf_reset();
    static char tbuf[24];
    tbuf[0] = 0;
    wf_form_t f2;
    wf_form_init(&f2, &g, 0x202020, "controls");
    wf_control_t *pnl = wf_add_panel(&f2, 20, 60, 300, 120, 0x303840);
    if (pnl) { pnl->style = WF_STYLE_ETCHED; }     /* GroupBox-Rahmen (geaetzte Nut) */
    wf_add_label(&f2, 30, 70, 200, 12, "Name:", 0xC0C0C0);
    wf_control_t *tb = wf_add_textbox(&f2, 30, 100, 200, 24, tbuf, (int)sizeof(tbuf));
    wf_paint(&f2);

    /* Panel: Fuellung innen + geaetzter Rahmen (aeussere Kante = Schatten 0x808080) an der Ecke. */
    int panel_ok = (gui_get(&g, 25, 65) == 0x303840u) && (gui_get(&g, 20, 60) == 0x808080u);

    /* Label: transparent (an textfreier Stelle scheint die Panel-Fuellung durch) + Text vorhanden. */
    int lbl_bg = (gui_get(&g, 220, 75) == 0x303840u);           /* rechts, kein Glyph -> Panel-Fuellung */
    int lbl_fg = 0;
    for (int yy = 72; yy < 80 && !lbl_fg; yy++) {
        for (int xx = 30; xx < 78; xx++) { if (gui_get(&g, xx, yy) == 0xC0C0C0u) { lbl_fg = 1; break; } }
    }
    int label_ok = lbl_bg && lbl_fg;

    /* TextBox: Klick fokussiert, dann "Hi" -> Backspace -> "x" ergibt "Hx". */
    gui_event_t td = mk_mouse(40, 110, 0x01); wf_handle(&f2, &td);
    gui_event_t tu = mk_mouse(40, 110, 0x00); wf_handle(&f2, &tu);
    int tb_focus = (f2.focused == tb);
    gui_event_t k;
    k = mk_key('H'); wf_handle(&f2, &k);
    k = mk_key('i'); wf_handle(&f2, &k);
    k = mk_key((char)8); wf_handle(&f2, &k);   /* Backspace -> "H" */
    k = mk_key('x'); wf_handle(&f2, &k);       /* -> "Hx" */
    int type_ok = (tb->buflen == 2 && tbuf[0] == 'H' && tbuf[1] == 'x' && tbuf[2] == 0);

    /* Repaint: weisses (versenktes) Feld + Caret hinter "Hx" (Spalte 2 -> x = 30 + inset(3) + 16 = 49). */
    wf_paint(&f2);
    int white_ok = (gui_get(&g, 100, 110) == 0xFFFFFFu);       /* Feld-Fuellung rechts vom Text */
    int caret_ok = 0;
    for (int yy = 106; yy < 122 && !caret_ok; yy++) { if (gui_get(&g, 49, yy) == 0x000000u) { caret_ok = 1; } }

    /* Guardian: ein NICHT 0-terminierter Anfangspuffer muss sicher auf cap-1 gekappt
     * werden -- kein OOB-Read/Write. raw[3] darf danach = 0 sein (Terminator im Puffer), buflen==3. */
    static char raw[4] = { 'A', 'B', 'C', 'D' };               /* KEIN Terminator */
    wf_reset();
    wf_form_t f3;
    wf_form_init(&f3, &g, 0, 0);
    wf_control_t *tb2 = wf_add_textbox(&f3, 0, 0, 40, 12, raw, 4);
    int bound_ok = (tb2 != 0 && tb2->buflen == 3 && raw[3] == 0);

    int textbox_ok = tb_focus && type_ok && white_ok && caret_ok && bound_ok;

    uwrite("[guitest] controls panel=");
    uwrite(panel_ok ? "ok" : "FEHLT");
    uwrite(" label=");
    uwrite(label_ok ? "ok" : "FEHLT");
    uwrite(" textbox=");
    uwrite(textbox_ok ? "ok" : "FEHLT");
    uwrite("\n");

    /* (8) Tastaturnavigation: Tab wechselt den Fokus (mit Umlauf), Enter loest den fokussierten
     * Button aus, Enter in einer TextBox fuegt kein Zeichen ein. Deterministisch mit Key-Events. */
    wf_reset();
    static char nbuf[16];
    nbuf[0] = 0;
    wf_form_t f4;
    wf_form_init(&f4, &g, 0x202020, "nav");
    wf_control_t *bx  = wf_add_button(&f4, 10, 10, 60, 24, "X", on_click_nav);
    wf_control_t *ntb = wf_add_textbox(&f4, 10, 40, 100, 24, nbuf, (int)sizeof(nbuf));
    wf_control_t *by  = wf_add_button(&f4, 10, 70, 60, 24, "Y", on_click_nav);
    gui_event_t t;
    t = mk_key('\t'); wf_handle(&f4, &t); int nav1 = (f4.focused == bx);
    t = mk_key('\t'); wf_handle(&f4, &t); int nav2 = (f4.focused == ntb);
    t = mk_key('\t'); wf_handle(&f4, &t); int nav3 = (f4.focused == by);
    t = mk_key('\t'); wf_handle(&f4, &t); int nav4 = (f4.focused == bx);   /* Umlauf zurueck zu X */
    int tab_ok = nav1 && nav2 && nav3 && nav4;

    int before = g_nav_clicks;                          /* Enter auf fokussiertem Button X */
    t = mk_key('\r'); wf_handle(&f4, &t);
    int enter_ok = (g_nav_clicks == before + 1);

    t = mk_key('\t'); wf_handle(&f4, &t);               /* Fokus X -> TextBox */
    before = g_nav_clicks;
    t = mk_key('\r'); wf_handle(&f4, &t);               /* Enter in TextBox: kein Klick, kein Zeichen */
    int enter_tb_ok = (f4.focused == ntb && g_nav_clicks == before && ntb->buflen == 0);

    /* Guardian: ein unsichtbares, aber fokussiertes Control darf per Tastatur NICHT
     * ausgeloest werden (Symmetrie zum Maus-hit_test, der visible prueft). */
    f4.focused = bx; bx->visible = 0;
    before = g_nav_clicks;
    t = mk_key('\r'); wf_handle(&f4, &t);
    int hidden_ok = (g_nav_clicks == before);          /* kein Klick auf den unsichtbaren Button */
    bx->visible = 1;

    uwrite("[guitest] keynav tab=");
    uwrite(tab_ok ? "ok" : "FEHLT");
    uwrite(" enter=");
    uwrite((enter_ok && enter_tb_ok && hidden_ok) ? "ok" : "FEHLT");
    uwrite("\n");

    sys3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
