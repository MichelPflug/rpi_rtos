/*
 * drivers/usb/usbkbd.c  --  HID-Boot-Tastatur -> ASCII, plus Konsolen-Eingabe-Mux
 *
 * Holt fertige Interrupt-IN-Reports IRQ-getrieben aus dem DWC2-Ring
 * (dwc2_kbd_irq_getreport), erkennt NEU gedrueckte Tasten per Vergleich mit dem
 * vorigen 8-Byte-Boot-Report (Modifier + bis zu 6 Keycodes) und uebersetzt den
 * HID-Usage-Code unter Beachtung von Shift in ASCII.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "usb_hc.h"
#include "usbkbd.h"
#include "sched.h"

/* NULL-sichere Wrapper um die aktive HCD-vtable. Ein HC ohne HID-Interrupt-Support
 * (z.B. der xHCI-Kern) laesst kbd_* auf NULL -> hier -1 (kein Report). */
static int hc_kbd_irq_getreport(uint8_t report[8])
{
    const usb_hc_ops_t *h = usb_hc();
    return (h && h->kbd_irq_getreport) ? h->kbd_irq_getreport(report) : -1;
}
static int hc_kbd_poll(uint8_t report[8])
{
    const usb_hc_ops_t *h = usb_hc();
    return (h && h->kbd_poll) ? h->kbd_poll(report) : -1;
}

static int     s_enabled;
static uint8_t s_prev[8];          /* letzter Report (fuer Flankenerkennung) */

/* Laufende Escape-Sequenz (Pfeiltaste -> mehrbytiges CSI): s_seq zeigt auf "\x1b[A" o.ae.,
 * s_seq_pos ist das naechste auszugebende Byte. So liefert eine Pfeiltaste die 3 Bytes
 * ESC '[' 'A' nacheinander an console_readline (das CSI versteht). */
static const char *s_seq;
static int         s_seq_pos;

void usbkbd_enable(void) { s_enabled = 1; }

/* Pfeiltaste (HID-Usage) -> CSI-Sequenz, oder 0. Hoch/Runter = Historie, Links/Rechts = Cursor. */
static const char *arrow_seq(uint8_t code)
{
    switch (code) {
    case 0x52: return "\x1b[A";   /* Pfeil hoch    */
    case 0x51: return "\x1b[B";   /* Pfeil runter  */
    case 0x4F: return "\x1b[C";   /* Pfeil rechts  */
    case 0x50: return "\x1b[D";   /* Pfeil links   */
    default:   return 0;
    }
}

/* HID-Usage-Code -> ASCII. shift = LeftShift|RightShift gedrueckt. 0 = kein Zeichen. */
static char hid_to_ascii(uint8_t code, int shift)
{
    if (code >= 0x04 && code <= 0x1D) {            /* a..z */
        return (char)((shift ? 'A' : 'a') + (code - 0x04));
    }
    if (code >= 0x1E && code <= 0x26) {            /* 1..9 */
        static const char sh[] = "!@#$%^&*(";
        return shift ? sh[code - 0x1E] : (char)('1' + (code - 0x1E));
    }
    switch (code) {
    case 0x27: return shift ? ')' : '0';
    case 0x28: return '\n';                         /* Enter */
    case 0x2A: return '\b';                         /* Backspace */
    case 0x2B: return '\t';                         /* Tab */
    case 0x2C: return ' ';                          /* Space */
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default:   return 0;
    }
}

static int in_prev(uint8_t code)
{
    for (int i = 2; i < 8; i++) {
        if (s_prev[i] == code) {
            return 1;
        }
    }
    return 0;
}

/* Einen 8-Byte-Boot-Report in ein ASCII-Zeichen uebersetzen (Flankenerkennung). allow_seq=1:
 * Pfeiltasten werden als CSI-Sequenz emittiert (Shell-Phase); =0: Pfeiltasten ignoriert
 * (Login-Phase -- read_line kennt keine Escape-Sequenzen, sonst landete '[' im Eingabefeld). */
int usbkbd_decode(const uint8_t *rep, int allow_seq)
{
    int shift = (rep[0] & 0x22) ? 1 : 0;           /* LeftShift(0x02) | RightShift(0x20) */
    int out = -1;
    for (int i = 2; i < 8; i++) {
        uint8_t code = rep[i];
        if (code != 0 && !in_prev(code)) {         /* neu gedrueckte Taste */
            if (allow_seq) {
                const char *seq = arrow_seq(code);
                if (seq) {                          /* Pfeiltaste: ESC zuerst, '[' + Final via s_seq */
                    s_seq = seq;
                    s_seq_pos = 1;
                    out = 0x1b;
                    break;
                }
            }
            char c = hid_to_ascii(code, shift);
            if (c) {
                out = (unsigned char)c;
                break;                              /* ein Zeichen pro Report-Aenderung */
            }
        }
    }
    for (int i = 0; i < 8; i++) {
        s_prev[i] = rep[i];
    }
    return out;
}

/* Naechstes Byte einer LAUFENDEN Pfeiltasten-Sequenz (ESC '[' Final), oder -1. console_getc
 * gibt dem Vorrang vor der UART, damit ein mehrbytiges CSI nicht von einem gleichzeitig
 * eintreffenden seriellen Byte zerrissen wird (sonst landeten '[' und 'A' als Literale in der
 * Zeile). */
int usbkbd_seq_pending(void)
{
    if (s_seq && s_seq[s_seq_pos]) {
        char c = s_seq[s_seq_pos++];
        if (!s_seq[s_seq_pos]) { s_seq = 0; }       /* Sequenz erschoepft */
        return (unsigned char)c;
    }
    return -1;
}

/* Aus dem IRQ-Ring (Shell-Phase: IRQs aktiv, Scheduler laeuft). */
int usbkbd_getchar(void)
{
    if (!s_enabled) {
        return -1;
    }
    int seq = usbkbd_seq_pending();                 /* laufende Pfeiltasten-Sequenz fortsetzen */
    if (seq >= 0) {
        return seq;
    }
    uint8_t rep[8];
    if (hc_kbd_irq_getreport(rep) <= 0) {
        return -1;
    }
    return usbkbd_decode(rep, /*allow_seq=*/1);
}

/* Synchroner HW-Poll (Login-Phase: vor dem Scheduler, IRQs maskiert -> kein Ring). */
static int usbkbd_getchar_polled(void)
{
    if (!s_enabled) {
        return -1;
    }
    uint8_t rep[8];
    if (hc_kbd_poll(rep) <= 0) {
        return -1;
    }
    return usbkbd_decode(rep, /*allow_seq=*/0);     /* keine Pfeiltasten im Login */
}

/* Wie usbkbd_getchar, aber OHNE Pfeiltasten-CSI-Zerlegung (allow_seq=0): eine Pfeiltaste liefert
 * KEINE synthetische ESC-'['-Sequenz, die sonst als Literale '[' 'A' in eine GUI-TextBox lecken
 * wuerde. Non-blocking, aus dem IRQ-Ring. */
static int usbkbd_getchar_raw(void)
{
    if (!s_enabled) {
        return -1;
    }
    uint8_t rep[8];
    if (hc_kbd_irq_getreport(rep) <= 0) {
        return -1;
    }
    return usbkbd_decode(rep, /*allow_seq=*/0);
}

/* Non-blocking: naechstes Konsolenzeichen (serielle UART oder USB-Tastatur-IRQ-Ring) oder -1,
 * wenn gerade nichts anliegt. Fuer Poller wie den GUI-Timer-Tick, die -- anders als console_getc --
 * NICHT blockieren duerfen. Post-Scheduler (IRQs an) liefert der USB-Ring; die UART geht immer.
 * Rohe Zeichen (allow_seq=0) -> keine Pfeiltasten-CSI-Bytes in die GUI-Event-Queue. */
#ifdef DEV_REMOTE
int dev_console_inject_get(void);   /* net/dev_agent.c: ferngesteuerte Taste (Dev-Remote) oder -1 */
#endif

#ifdef PCIE_PROBE
/* xHCI-Tastatur-Injektionsring (Pi4-USB-A hinter PCIe): der xHCI-Keyboard-Poll-Task (drivers/pci/
 * pcie.c) legt dekodierte ASCII-Zeichen hier ab; console_getc(_nb) liest sie -> die LOKALE USB-
 * Tastatur landet in der Shell. Ganz #ifdef PCIE_PROBE -> ohne das Flag byte-inert (RC/Vk/Release
 * unveraendert). SPSC: Producer = xHCI-Poll-Task, Consumer = console_getc(_nb). */
static volatile int s_xk_head, s_xk_tail;
static char s_xk_ring[64];
void usbkbd_xhci_push(int c)
{
    int h = s_xk_head, nh = (h + 1) & 63;
    if (nh != s_xk_tail) { s_xk_ring[h] = (char)c; s_xk_head = nh; }
}
static int xhci_kbd_get(void)
{
    if (s_xk_tail == s_xk_head) { return -1; }
    char c = s_xk_ring[s_xk_tail]; s_xk_tail = (s_xk_tail + 1) & 63;
    return (unsigned char)c;
}
#endif

int console_getc_nb(void)
{
#ifdef DEV_REMOTE
    int di = dev_console_inject_get();               /* ferngesteuerte Tasten zuerst (Dev-Remote) */
    if (di >= 0) { return di; }
#endif
#ifdef PCIE_PROBE
    { int xk = xhci_kbd_get(); if (xk >= 0) { return xk; } }   /* lokale xHCI-USB-Tastatur */
#endif
    int c = uart_getc_nb();                          /* serielle Konsole */
    if (c >= 0) { return c; }
    c = usbkbd_getchar_raw();                        /* USB-Tastatur (IRQ-Ring, rohe Zeichen) */
    if (c >= 0) { return c; }
    return -1;
}

char console_getc(void)
{
    for (;;) {
        /* Safe-Point fuer SYS_KILL: ein EL0-Leser parkt hier als RUNNING (busy-poll, kein
         * Kernel-Lock gehalten, in der Shell mit freigegebenen IRQs) und wird sonst von den
         * EL0-Grenz-Safe-Points nicht getroffen. Per Timer-Preemption getaktet beendet sich ein
         * gekillter Leser damit spaetestens beim naechsten Poll-Durchlauf -- auch ohne Eingabe.
         * Vor dem Scheduler-Start / ohne kill_pending ein No-Op (idx-/used-Guard). */
        sched_exit_if_killed();
#ifdef DEV_REMOTE
        int di = dev_console_inject_get();          /* ferngesteuerte Tasten (Dev-Remote) -> Shell */
        if (di >= 0) {
            return (char)di;
        }
#endif
#ifdef PCIE_PROBE
        { int xk = xhci_kbd_get(); if (xk >= 0) { return (char)xk; } }   /* lokale xHCI-USB-Tastatur */
#endif
        int c = usbkbd_seq_pending();               /* laufendes USB-Pfeiltasten-CSI ATOMAR
                                                     * zuende liefern, bevor die UART gepollt wird */
        if (c >= 0) {
            return (char)c;
        }
        c = uart_getc_nb();                          /* serielle Konsole */
        if (c >= 0) {
            return (char)c;
        }
        c = usbkbd_getchar();                       /* USB-Tastatur (IRQ-Ring) */
        if (c >= 0) {
            return (char)c;
        }
        /* Vor dem Scheduler (DAIF.I gesetzt -> IRQs maskiert) liefert der Ring nie
         * etwas, da der Tastatur-IRQ nicht feuert. Dann die HW direkt pollen
         * (waehrend des Login gibt es ohnehin keine anderen Tasks). In der Shell
         * (IRQs aktiv) bleibt es beim Ring -> kein Busy-Poll, Kinder laufen weiter. */
        if (READ_SYSREG(daif) & (1u << 7)) {
            c = usbkbd_getchar_polled();
            if (c >= 0) {
                return (char)c;
            }
        } else {
            /* Scheduler laeuft (IRQs aktiv, z.B. in der Shell): keine Eingabe -> CPU ABGEBEN statt
             * busy zu pollen. Sonst hungert ein EL0-Leser andere Kern-0-Tasks aus (der Dev-Remote-
             * Netz-Agent lief deshalb NIE an). Eingabe kommt ueber den IRQ-Ring bzw. die Fern-Injektion;
             * die naechste Poll-Runde folgt spaetestens in einem Tick. */
            task_sleep_ticks(1);
        }
    }
}
