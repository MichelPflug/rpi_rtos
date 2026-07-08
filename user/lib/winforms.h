/*
 * user/lib/winforms.h  --  WinForms-artiger Retained-Mode-Kern + Standard-Controls.
 *
 * Setzt rein in EL0 auf libgui + die Eingabe-Events 
 * auf. Modell wie .NET-4.8-WinForms: eine Form haelt Controls (aus einem statischen Pool, kein
 * Heap) mit Location/Size/Text/Farben + einem Click-Delegaten. Die Message-Loop (Application.Run)
 * holt Events, macht Hit-Testing, verwaltet Fokus + Maus-Capture, feuert Click (Down+Up ueber
 * demselben Control) und leitet Tasten an die fokussierte TextBox. Alles integer-only.
 */
#ifndef RPI_RTOS_WINFORMS_H
#define RPI_RTOS_WINFORMS_H

#include "gui.h"
#include "gui_abi.h"

typedef struct wf_control wf_control_t;
typedef struct wf_form    wf_form_t;

typedef void (*wf_click_fn)(wf_control_t *c);

/* Control-Arten (WinForms-Klassen als Tag statt Vererbung). */
#define WF_LABEL    0
#define WF_BUTTON   1
#define WF_TEXTBOX  2
#define WF_PANEL    3

/* Rahmen-Stil (3D-Bevel, klassischer WinForms-Look). Buttons/TextBoxen setzen ihren Stil selbst;
 * fuer Panels waehlbar (Titelleiste flach, GroupBox geaetzt, Bild-/Vorschaufeld versenkt). */
#define WF_STYLE_FLAT    0   /* nur Fuellung, kein Rahmen */
#define WF_STYLE_RAISED  1   /* erhaben (hell oben/links, dunkel unten/rechts) */
#define WF_STYLE_SUNKEN  2   /* versenkt (dunkel oben/links, hell unten/rechts) */
#define WF_STYLE_ETCHED  3   /* geaetzte Nut (GroupBox-Rahmen) */

struct wf_control {
    int          kind;          /* WF_* */
    int          x, y, w, h;     /* Location/Size relativ zur Form (Pixel) */
    unsigned     back, fore;     /* Fuell- + Textfarbe (0xRRGGBB; back==GUI_TRANSPARENT -> nicht fuellen) */
    const char  *text;           /* Label-/Button-Beschriftung (statisch) */
    int          visible, enabled, focusable;
    int          pressed;        /* laufender linker Maus-Druck (Button-Feedback) */
    int          style;          /* WF_STYLE_* (Rahmen-Bevel; v.a. fuer Panels waehlbar) */
    wf_click_fn  on_click;       /* WinForms-Click-Delegat (Down+Up ueber demselben Control) */
    /* TextBox: app-eigener, editierbarer Puffer (kein Heap). */
    char        *buf;            /* editierbarer Text (0-terminiert), sonst 0 */
    int          buflen;         /* aktuelle Laenge */
    int          bufcap;         /* Kapazitaet inkl. 0-Byte (nutzbar: bufcap-1) */
    void        *tag;            /* frei fuer die App */
};

#define WF_MAX_CONTROLS 32

struct wf_form {
    const gui_t  *g;
    unsigned      back;          /* Form-Hintergrund */
    const char   *title;
    wf_control_t *ctrl[WF_MAX_CONTROLS];
    int           n;
    wf_control_t *focused;       /* fokussiertes Control (oder 0) */
    wf_control_t *capture;       /* Control unter der Maus beim Druck (fuer Click-Erkennung) */
    unsigned      prev_buttons;  /* Button-Maske des letzten Events (Flankenerkennung) */
    int           running;       /* wf_run laeuft solange != 0 */
    int           dirty;         /* Neuzeichnen faellig */
};

/* Proportionalen TTF-Font fuer ALLE Control-Beschriftungen setzen (0 = zurueck zum 8x8-Font).
 * Global; Text wird dann anti-aliased ueber die jeweilige Hintergrundfarbe gemischt. */
void          wf_set_font(const gui_font_t *font);

/* Control-Pool leeren (Test/Neustart einer Form). */
void          wf_reset(void);

/* Form initialisieren (Hintergrund + Titel), Control-Liste leer. */
void          wf_form_init(wf_form_t *f, const gui_t *g, unsigned back, const char *title);

/* --- Standard-Controls: legen aus dem Pool an + haengen an die Form (0 bei vollem Pool). --- */
/* Label: rahmenlos, transparenter Hintergrund, linksbuendiger Text, nicht fokussierbar/klickbar. */
wf_control_t *wf_add_label(wf_form_t *f, int x, int y, int w, int h, const char *text, unsigned fore);
/* Button: gefuellt + gerahmt + zentrierter Text, klickbar + fokussierbar. */
wf_control_t *wf_add_button(wf_form_t *f, int x, int y, int w, int h,
                            const char *text, wf_click_fn on_click);
/* TextBox: editierbar (Tastatur an die fokussierte Box), Caret, app-eigener Puffer buf[cap]. */
wf_control_t *wf_add_textbox(wf_form_t *f, int x, int y, int w, int h, char *buf, int cap);
/* Panel: gefuellter + gerahmter Hintergrund-Container (nicht fokussierbar/klickbar). */
wf_control_t *wf_add_panel(wf_form_t *f, int x, int y, int w, int h, unsigned back);

/* Oberstes sichtbares+aktives Control unter (x,y), sonst 0 (Form-Hintergrund). */
wf_control_t *wf_hit_test(wf_form_t *f, int x, int y);

/* Die ganze Form neu zeichnen (Hintergrund + alle Controls) und zum FB flushen. */
void          wf_paint(wf_form_t *f);

/* EIN Eingabe-Event dispatchen: Maus (Hit-Testing/Fokus/Capture/Click) ODER Taste (an die
 * fokussierte TextBox). Reine Funktion des Form-Zustands (aus konstruierten Events testbar). */
void          wf_handle(wf_form_t *f, const gui_event_t *ev);

/* Non-blocking: ein Event aus der Kernel-Queue holen (SYS_POLL_EVENT) und dispatchen. 1/0. */
int           wf_pump(wf_form_t *f);

/* Application.Run: initial zeichnen, dann SYS_WAIT_EVENT-Schleife bis wf_close (running=0). */
void          wf_run(wf_form_t *f);

/* Message-Loop beenden (running=0) -> wf_run kehrt zurueck. Aus einem Click-Handler nutzbar. */
void          wf_close(wf_form_t *f);

#endif /* RPI_RTOS_WINFORMS_H */
