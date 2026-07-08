/*
 * kernel/gui_input.c  --  Kernel-Eingabe-Event-Queue
 *
 * SPSC-Ring: EIN Producer (gui_input_tick, Timer-IRQ Kern 0) + EIN Consumer (der GUI-EL0-Task in
 * SYS_POLL_EVENT/SYS_WAIT_EVENT). head gehoert dem Producer, tail dem Consumer; `dmb ish` ordnet
 * die Nutzdaten vor der Index-Freigabe. Kein Lock noetig (Single-Producer/Single-Consumer).
 */
#include <stdint.h>
#include "gui_input.h"
#include "usbmouse.h"
#include "gui_fb.h"
#include "usbkbd.h"    /* console_getc_nb (Tastatur-Poll in der GUI-Sitzung) */

#ifdef DEV_REMOTE
void dev_input_drain(void);   /* net/dev_agent.c: gestagete Dev-Remote-KEY/MOUSE-Events einreihen */
#endif
#ifdef PCIE_PROBE
#include "pcie.h"             /* xhci_mouse_get: lokale USB-Maus (Pi4 hinter PCIe/VL805) */
#endif

#define EVQ_N 64u                          /* 2er-Potenz -> Maskierung statt Modulo */
static gui_event_t      g_evq[EVQ_N];
static volatile uint32_t g_head;           /* naechster Schreib-Index (Producer) */
static volatile uint32_t g_tail;           /* naechster Lese-Index (Consumer)    */
static int              s_enabled;
static int              s_kbd;             /* Tastatur-Poll aktiv (nur in der GUI-Sitzung T2.6) */

static void dmb(void) { __asm__ volatile("dmb ish" ::: "memory"); }

void gui_input_enable(void)     { s_enabled = 1; }
void gui_input_enable_kbd(void) { s_kbd = 1; }

int gui_input_push(const gui_event_t *ev)
{
    uint32_t h = g_head;
    uint32_t n = (h + 1u) & (EVQ_N - 1u);
    if (n == g_tail) { return 0; }         /* voll -> verwerfen */
    g_evq[h] = *ev;
    dmb();                                 /* Nutzdaten vor der head-Freigabe sichtbar machen */
    g_head = n;
    return 1;
}

int gui_input_pop(gui_event_t *out)
{
    uint32_t t = g_tail;
    if (t == g_head) { return 0; }         /* leer */
    dmb();
    *out = g_evq[t];
    g_tail = (t + 1u) & (EVQ_N - 1u);
    return 1;
}

void gui_input_tick(void)
{
    /* Maus- und Tastatur-Poll unabhaengig gaten: eine keyboard-only GUI-Sitzung (kein Maus-Enable)
     * muss den Tastatur-Poll trotzdem bekommen. */
    if (s_enabled && usbmouse_poll()) {    /* Cursor/Buttons haben sich geaendert (dwc2-Maus, QEMU) */
        gui_fb_move_cursor(usbmouse_x(), usbmouse_y());  /* Cursor autonom komponieren (loescht alte Pos) */
        gui_event_t ev = { 0, 0, 0, 0, 0, 0 };
        ev.type    = GUI_EV_MOUSE;
        ev.buttons = (uint8_t)usbmouse_buttons();
        ev.x       = (int16_t)usbmouse_x();
        ev.y       = (int16_t)usbmouse_y();
        gui_input_push(&ev);
    }
#ifdef PCIE_PROBE
    /* Lokale xHCI-USB-Maus (Pi4 USB-A hinter PCIe): der Maus-Poll-Task pflegt Cursor+Buttons, hier
     * (single-producer) auslesen -> Cursor bewegen + GUI_EV_MOUSE einreihen. */
    { int mx, my, mb;
      if (xhci_mouse_get(&mx, &my, &mb)) {
          gui_fb_move_cursor(mx, my);
          gui_event_t ev = { 0, 0, 0, 0, 0, 0 };
          ev.type = GUI_EV_MOUSE; ev.buttons = (uint8_t)mb;
          ev.x = (int16_t)mx; ev.y = (int16_t)my;
          gui_input_push(&ev);
      } }
#endif
    if (s_kbd) {                            /* Tastatur -> GUI_EV_KEY (Serial/USB), bounded je Tick */
        int c, n = 0;
        while (n++ < 8 && (c = console_getc_nb()) >= 0) {
            gui_event_t ev = { 0, 0, 0, 0, 0, 0 };
            ev.type = GUI_EV_KEY;
            ev.key  = (uint8_t)c;
            gui_input_push(&ev);
        }
    }
#ifdef DEV_REMOTE
    /* Dev-Remote (docs/architecture/20): ferngesteuerte KEY/MOUSE-Events aus dem Netz-Task hier
     * (Timer-IRQ, Kern 0) einreihen -> gui_input_push bleibt strikt single-producer. Ganz
     * #ifdef DEV_REMOTE -> ohne das Flag kein Byte. */
    dev_input_drain();
#endif
}

#ifdef RTOS_SELFTEST
int gui_input_selftest(void)
{
    g_head = 0; g_tail = 0;
    gui_event_t a = { GUI_EV_MOUSE, 0x01, 0, 0, 100, 200 };
    gui_event_t b = { GUI_EV_KEY,   0,    'X', 0, 0, 0 };
    gui_event_t o;
    int ok = gui_input_push(&a) && gui_input_push(&b);
    ok = ok && gui_input_pop(&o) && o.type == GUI_EV_MOUSE && o.x == 100 && o.y == 200 && o.buttons == 0x01;
    ok = ok && gui_input_pop(&o) && o.type == GUI_EV_KEY && o.key == 'X';
    ok = ok && (gui_input_pop(&o) == 0);   /* jetzt leer */
    return ok ? 1 : 0;
}
#endif
