/*
 * kernel/syscall.c  --  Syscall-Dispatch fuer EL0-User-Programme
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "usbkbd.h"
#include "sched.h"
#include "proc.h"
#include "vfs.h"
#include "user.h"
#include "spinlock.h"
#include "abi.h"
#include "syscall.h"
#include "gui_fb.h"
#include "gui_input.h"
#include "vi_parallel.h"   /* Kernel-Parallel-For (Fns nur #ifdef VISION vorhanden) */

/* Prueft via Hardware-Adressuebersetzung (AT S1E0R/W), ob die VA 'va' als EL0 les- bzw.
 * schreibbar ist -- Mapping UND Permission, OHNE einen Fault auszuloesen. 1 = ok, 0 = nicht.
 * Nutzt die aktuelle TTBR0 (waehrend eines Syscalls = Adressraum des aufrufenden Prozesses).
 * Defense-in-Depth ueber den festen Kachel-Bereichs-Check hinaus: selbst ein in-range-aber-
 * unmapped Zeiger (z.B. bei kuenftigem Demand-Paging/Guard-Pages) kann so den Kernel nicht
 * mehr per EL1-Data-Abort haengen -- der Zugriff wird vorab abgelehnt (-1/-EFAULT). */
static int uaccess_page_ok(uint64_t va, int write)
{
    if (write) { __asm__ volatile("at s1e0w, %0" :: "r"(va) : "memory"); }
    else       { __asm__ volatile("at s1e0r, %0" :: "r"(va) : "memory"); }
    isb();
    return (READ_SYSREG(par_el1) & 1ULL) == 0;   /* PAR.F==0 -> uebersetzbar + EL0-Zugriff ok */
}

/* Kopiert einen nullterminierten String aus der User-Region in einen Kernel-Puffer.
 * Grobbereich [USER_BASE,USER_STACK_TOP) + je Seite AT-geprueft (lesbar). 0 = ok, -1 = ungueltig. */
static int copy_user_string(char *dst, uint64_t uptr, int max)
{
    for (int i = 0; i < max; i++) {
        uint64_t a = uptr + (uint64_t)i;
        if (a < USER_BASE || a >= USER_STACK_TOP) {
            return -1;
        }
        if (i == 0 || (a & 0xFFF) == 0) {          /* Seiteneintritt -> EL0-Lesbarkeit pruefen */
            if (!uaccess_page_ok(a, 0)) {
                return -1;
            }
        }
        char c = *(const char *)(uintptr_t)a;
        dst[i] = c;
        if (c == 0) {
            return 0;
        }
    }
    return -1;
}

/* Prueft, dass [buf, buf+len) in der User-Kachel liegt (Overflow-sicher) UND jede beruehrte
 * Seite als EL0 in Richtung 'write' (0=lesen/1=schreiben) zugreifbar ist. 1 = ok, 0 = nicht. */
static int user_range_ok(uint64_t buf, uint64_t len, int write)
{
    if (buf < USER_BASE || buf > USER_STACK_TOP || len > USER_STACK_TOP - buf) {
        return 0;
    }
    for (uint64_t a = buf; a < buf + len; a = (a & ~0xFFFULL) + 0x1000) {
        if (!uaccess_page_ok(a, write)) {
            return 0;
        }
    }
    return 1;
}

#ifdef RTOS_SELFTEST
/* Sanity-Check der AT-Mechanik: im Kernel-Adressraum (kein User-Tile) darf NICHTS als EL0
 * zugreifbar sein -> AT muss Adresse 0 (unmapped) UND eine Kernel-.text-Adresse (EL1-only)
 * als nicht-zugreifbar melden. Der Positiv-Fall (gueltiger User-Zeiger -> ok) wird implizit
 * von jedem erfolgreichen EL0-Datei-Syscall abgedeckt. 1 = AT lehnt korrekt ab. */
int uaccess_selftest(void)
{
    int z = uaccess_page_ok(0, 0);                                   /* unmapped -> 0 */
    int k = uaccess_page_ok((uint64_t)(uintptr_t)&uaccess_selftest, 0); /* Kernel .text, EL1-only -> 0 */
    return (z == 0) && (k == 0);
}
#endif

/* Userland darf per Datei-Syscalls auf die User-Partition (hdd1) und den USB-Stick
 * (hdd2) zugreifen. Die System-Partition hdd0 -- inkl. der Credential-DB USERS.DB --
 * bleibt fuer EL0-Prozesse weder les- noch schreibbar (kernel-only). */
static int user_path_allowed(const char *p)
{
    return p[0] == 'h' && p[1] == 'd' && p[2] == 'd' &&
           (p[3] == '1' || p[3] == '2') && p[4] == ':';
}

/* Fuer ls: ein Mount-Name "hdd1"/"hdd2" optional mit Unterpfad ("hdd1:DOCS").
 * hdd0 (System) bleibt fuer EL0 unsichtbar -- auch beim Verzeichnislisting. */
static int user_listpath_allowed(const char *p)
{
    if (!(p[0] == 'h' && p[1] == 'd' && p[2] == 'd' && (p[3] == '1' || p[3] == '2'))) {
        return 0;
    }
    return p[4] == '\0' || p[4] == ':';     /* Wurzel oder ':<unterpfad>' */
}

/* --- Kommando-Historie fuer die Konsolen-Zeileneingabe (Pfeil hoch/runter) ---
 * Ringpuffer der zuletzt eingegebenen Zeilen. Global (es gibt effektiv eine interaktive
 * Shell); der Login (read_line in user.c) nutzt das NICHT -> Passwoerter landen nie hier. */
#define HIST_MAX 8
#define HIST_LEN 128
static char s_hist[HIST_MAX][HIST_LEN];
static int  s_hist_n;       /* Anzahl gespeicherter Eintraege (0..HIST_MAX) */
static int  s_hist_head;    /* Ring-Schreibindex (naechster Eintrag) */
/* console_readline laeuft preemptierbar (irq_enable) und kann via SYS_READ von einem
 * EL0-Prozess auf einem beliebigen Kern kommen -> der globale Ring wird unter diesem Lock
 * (IRQ-maskiert) angefasst, damit zwei gleichzeitige Leser ihn nicht korrumpieren. */
static spinlock_t s_histlock = SPINLOCK_INIT;

static int sc_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* k-ter juengster Eintrag (k=1 = neuester). 0, wenn k ausserhalb des Bereichs.
 * Nur mit gehaltenem s_histlock aufrufen (liest s_hist_head/s_hist_n). */
static const char *hist_get(int k)
{
    if (k < 1 || k > s_hist_n) {
        return 0;
    }
    int idx = (s_hist_head - k + HIST_MAX) % HIST_MAX;
    return s_hist[idx];
}

static void hist_push(const char *line)
{
    if (!line[0]) {
        return;                          /* leere Zeile nicht aufnehmen */
    }
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_histlock);
    const char *newest = hist_get(1);
    if (!(newest && sc_streq(newest, line))) {   /* aufeinanderfolgende Dublette ueberspringen */
        int j = 0;
        while (line[j] && j < HIST_LEN - 1) { s_hist[s_hist_head][j] = line[j]; j++; }
        s_hist[s_hist_head][j] = '\0';
        s_hist_head = (s_hist_head + 1) % HIST_MAX;
        if (s_hist_n < HIST_MAX) { s_hist_n++; }
    }
    spin_unlock(&s_histlock);
    WRITE_SYSREG(daif, f);
}

/* Den k-ten juengsten History-Eintrag in buf (max Bytes) kopieren, NUL-terminiert.
 * Liefert die Laenge. Kopiert unter s_histlock (konsistent gegen ein paralleles push);
 * das Echo erfolgt danach im Aufrufer (ausserhalb des Locks). */
static uint64_t hist_copy_into(int k, char *buf, uint64_t max)
{
    uint64_t j = 0;
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_histlock);
    const char *h = hist_get(k);
    while (h && h[j] && j < max - 1) { buf[j] = h[j]; j++; }
    buf[j] = '\0';
    spin_unlock(&s_histlock);
    WRITE_SYSREG(daif, f);
    return j;
}

/* Liest eine Zeile von der Serial-Konsole/USB-Tastatur in den (geprueften) User-Puffer;
 * echot die Eingabe. CR/LF-robust. Pfeil-hoch/runter (CSI 'A'/'B') blaettern durch die
 * Historie und zeichnen die Zeile neu. Liefert die Laenge (ohne Terminator). */
static long console_readline(char *buf, uint64_t max)
{
    if (max == 0) {
        return 0;
    }
    /* Syscalls laufen mit maskierten IRQs. Beim (potenziell langen) blockierenden
     * Lesen die IRQs freigeben, damit Timer/Preemption weiterlaufen und parallele
     * Tasks (z.B. per 'run' gestartete) nicht aushungern. Am Ende wiederherstellen. */
    uint64_t saved = READ_SYSREG(daif);
    irq_enable();

    uint64_t i = 0;        /* Zeilenlaenge */
    uint64_t pos = 0;      /* Cursor-Position (0..i) */
    int browse = 0;        /* 0 = frische Zeile; k>0 = k-ter juengster History-Eintrag */
    char c;
    do {
        c = console_getc();                 /* Serial ODER USB-Tastatur */
    } while (c == '\r' || c == '\n');

    for (;;) {
        if (c == '\r' || c == '\n') {
            while (pos < i) { uart_putc(buf[pos]); pos++; }   /* Cursor ans Zeilenende */
            uart_putc('\n');
            break;
        }
        if (c == 0x1b) {                    /* ESC -> evtl. CSI-Sequenz (ESC '[' ... Final) */
            char b1 = console_getc();
            if (b1 != '[') {
                /* Einzel-ESC + Folgebyte: dieses NICHT verschlucken, sondern normal
                 * verarbeiten (z.B. ESC dann Enter soll die Zeile abschicken). */
                c = b1;
                continue;
            }
            /* CSI: vollstaendig konsumieren -- Parameterbytes (0x30..0x3F) + Intermediates
             * (0x20..0x2F), dann das Final-Byte (0x40..0x7E). Sonst landeten die restlichen
             * Bytes mehrteiliger Sequenzen (Entf=ESC[3~, Bild-auf=ESC[5~, ...) in der Zeile.
             * 'A'/'B' = Historie hoch/runter, 'D'/'C' = Cursor links/rechts. */
            unsigned char fb = (unsigned char)console_getc();
            while (fb >= 0x30 && fb <= 0x3F) { fb = (unsigned char)console_getc(); }  /* Parameter */
            while (fb >= 0x20 && fb <= 0x2F) { fb = (unsigned char)console_getc(); }  /* Intermediate */
            if (fb == 'A' || fb == 'B') {
                int nb = browse + (fb == 'A' ? 1 : -1);   /* hoch=aelter, runter=neuer */
                if (nb < 0) { nb = 0; }
                if (nb > s_hist_n) { nb = s_hist_n; }
                if (nb != browse) {
                    while (pos < i) { uart_putc(buf[pos]); pos++; }   /* Cursor ans Ende */
                    while (i > 0) { uart_puts("\b \b"); i--; pos--; } /* ganze Zeile loeschen */
                    browse = nb;
                    if (browse == 0) {
                        buf[0] = '\0';                            /* zurueck zur leeren Zeile */
                    } else {
                        i = hist_copy_into(browse, buf, max);     /* unter s_histlock kopieren */
                        for (uint64_t j = 0; j < i; j++) { uart_putc(buf[j]); }
                    }
                    pos = i;                                      /* Cursor ans Ende der Recall-Zeile */
                }
            } else if (fb == 'D') {                  /* Cursor links */
                if (pos > 0) { pos--; uart_putc('\b'); }
            } else if (fb == 'C') {                  /* Cursor rechts */
                if (pos < i) { uart_putc(buf[pos]); pos++; }
            }
            /* andere Final-Bytes ('~' etc.) bewusst ignoriert */
            c = console_getc();             /* naechstes Zeichen holen */
            continue;
        }
        if (c == 8 || c == 127) {                    /* Backspace: Zeichen VOR dem Cursor loeschen */
            if (pos > 0) {
                for (uint64_t k = pos - 1; k + 1 < i; k++) { buf[k] = buf[k + 1]; }
                i--; pos--;
                uart_putc('\b');                              /* Cursor links */
                for (uint64_t k = pos; k < i; k++) { uart_putc(buf[k]); }  /* Rest neu zeichnen */
                uart_putc(' ');                               /* altes letztes Zeichen tilgen */
                for (uint64_t k = i + 1; k > pos; k--) { uart_putc('\b'); } /* Cursor zurueck */
            }
        } else if (i < max - 1 && c >= 32 && c < 127) {
            if (pos == i) {                          /* am Ende anhaengen (schneller Pfad) */
                buf[i++] = c; pos++;
                uart_putc(c);
            } else {                                 /* mitten in der Zeile einfuegen */
                for (uint64_t k = i; k > pos; k--) { buf[k] = buf[k - 1]; }
                buf[pos] = c; i++;
                for (uint64_t k = pos; k < i; k++) { uart_putc(buf[k]); }   /* ab Cursor neu */
                for (uint64_t k = i; k > pos + 1; k--) { uart_putc('\b'); } /* Cursor hinter c */
                pos++;
            }
        }
        c = console_getc();
    }
    buf[i] = '\0';
    hist_push(buf);                         /* Zeile in die Historie aufnehmen */
    WRITE_SYSREG(daif, saved);              /* maskierten Syscall-Zustand wiederherstellen */
    return (long)i;
}

/* Schreibt len Bytes ab buf (User-Zeiger) auf die Konsole. Mit Bereichspruefung. */
static long sys_write(uint64_t fd, uint64_t buf, uint64_t len)
{
    (void)fd;
    /* buf in der User-Kachel + jede Seite als EL0 LESBAR (AT). */
    if (!user_range_ok(buf, len, /*write=*/0)) {
        return -1;                       /* Zeiger/Laenge ungueltig oder nicht EL0-lesbar */
    }
    const char *p = (const char *)(uintptr_t)buf;
    /* Eine write()-Zeile string-atomar ausgeben: uart_write nimmt den UART-Lock (cross-core)
     * + maskiert IRQs -> die Ausgabe eines EL0-Prozesses verschraenkt sich weder mit einem
     * anderen Prozess auf demselben Kern noch mit der Ausgabe eines ANDEREN Kerns (wichtig,
     * sobald EL0-Prozesse auf Sekundaerkernen zur Laufzeit drucken). */
    uart_write(p, (uint32_t)len);
    return (long)len;
}

void syscall_dispatch(uint64_t *frame)
{
    uint64_t num = frame[8];               /* x8 = Syscall-Nummer */
    uint64_t a0  = frame[0];
    uint64_t a1  = frame[1];
    uint64_t a2  = frame[2];
    long     ret = 0;

    switch (num) {
    case SYS_WRITE:
        ret = sys_write(a0, a1, a2);
        break;
    case SYS_YIELD:
        sched_yield();
        break;
    case SYS_SLEEP_MS:
        task_sleep_ticks(a0 / 10);         /* 100 Hz -> 10 ms pro Tick */
        break;
    case SYS_GETPID:
        ret = (long)sched_current_pid();   /* monotone PID (nicht der recycelte Slot-Index) */
        break;
    case SYS_READ_FILE: {
        char kpath[64];
        if (copy_user_string(kpath, a0, sizeof(kpath)) != 0 || !user_range_ok(a1, a2, /*write=*/1) ||
            !user_path_allowed(kpath)) {
            ret = -1;
            break;
        }
        ret = vfs_read_file(kpath, (void *)(uintptr_t)a1, (uint32_t)a2);
        break;
    }
    case SYS_WRITE_FILE: {
        char kpath[64];
        if (copy_user_string(kpath, a0, sizeof(kpath)) != 0 || !user_range_ok(a1, a2, /*write=*/0) ||
            !user_path_allowed(kpath)) {
            ret = -1;
            break;
        }
        ret = vfs_write_file(kpath, (const void *)(uintptr_t)a1, (uint32_t)a2);
        break;
    }
    case SYS_WHOAMI:
        ret = (long)sched_current_uid();
        break;
    case SYS_USERADD: {
        /* Capability-gated: nutzt die Caps des AUFRUFENDEN Prozesses (cred). */
        char kname[32];
        char kpw[64];
        if (copy_user_string(kname, a0, sizeof(kname)) != 0 ||
            copy_user_string(kpw, a1, sizeof(kpw)) != 0) {
            ret = -1;
            break;
        }
        ret = user_add(kname, kpw, 0, sched_current_caps());
        break;
    }
    case SYS_READ: {
        if (!user_range_ok(a1, a2, /*write=*/1)) {   /* console_readline schreibt in a1 */
            ret = -1;
            break;
        }
        ret = console_readline((char *)(uintptr_t)a1, a2);   /* a0 (fd) ignoriert */
        break;
    }
    case SYS_READCHAR: {
        /* EIN rohes Byte, blockierend, ohne Echo/Zeilenpufferung -> der Shell-Editor (EL0)
         * macht Zeilenbearbeitung/History/Tab selbst. IRQs waehrend des (langen) Wartens frei,
         * damit Timer/Preemption laufen und parallele Tasks nicht aushungern. */
        uint64_t f = READ_SYSREG(daif);
        irq_enable();
        int ch = (unsigned char)console_getc();
        WRITE_SYSREG(daif, f);
        ret = ch;
        break;
    }
    case SYS_SPAWN: {
        /* Programm starten; das Kind ERBT den cred des Aufrufers (uid/caps).
         * Nur hdd1-Pfade (Userland-Programme auf der User-Partition). a1 = Kern-Affinitaet
         * (0..3): der Prozess laeuft mit eigenem TTBR0/ASID zur Laufzeit auf diesem Kern.
         * Ungueltige Kerne weist proc_exec_as_on ab (-1). */
        char kpath[64];
        /* a1 = Zielkern. VOR dem teuren ELF-Laden pruefen (fail-fast) + den vollen 64-Bit-Wert
         * gegen die Kernzahl pruefen, damit keine hohen Bits durch den uint32-Cast verloren
         * gehen. NCORES=4 (vgl. sched.c). */
        if (a1 >= 4 ||
            copy_user_string(kpath, a0, sizeof(kpath)) != 0 || !user_path_allowed(kpath)) {
            ret = -1;
            break;
        }
        /* Kind erbt cred; ppid = PID des Aufrufers (fuer wait/kill). Rueckgabe an EL0 = die
         * monotone PID des Kindes (nicht der interne Slot) -> Eltern-Handle fuer wait/kill. */
        uint64_t child_pid = 0;
        long tid = proc_exec_as_on((uint32_t)a1, kpath,
                                   sched_current_uid(), sched_current_caps(),
                                   sched_current_pid(), &child_pid);
        ret = (tid >= 0) ? (long)child_pid : -1;
        break;
    }
    case SYS_LISTDIR: {
        /* (path, buf, max) -> Verzeichnis als Text. "hdd1" oder "hdd1:DOCS"; nur hdd1/hdd2. */
        char kpath[64];
        if (copy_user_string(kpath, a0, sizeof(kpath)) != 0 || !user_range_ok(a1, a2, /*write=*/1) ||
            !user_listpath_allowed(kpath)) {
            ret = -1;
            break;
        }
        ret = vfs_listdir(kpath, (char *)(uintptr_t)a1, (uint32_t)a2);
        break;
    }
    case SYS_DELETE: {
        /* (path) -> Datei loeschen. Nur hdd1/hdd2 (hdd0 = System, geschuetzt). */
        char kpath[64];
        if (copy_user_string(kpath, a0, sizeof(kpath)) != 0 || !user_path_allowed(kpath)) {
            ret = -1;
            break;
        }
        ret = vfs_delete(kpath);
        break;
    }
    case SYS_PASSWD: {
        /* (newpw) -> eigenes Passwort aendern. Jeder Prozess nur fuer seine uid. */
        char kpw[64];
        if (copy_user_string(kpw, a0, sizeof(kpw)) != 0) {
            ret = -1;
            break;
        }
        ret = user_change_password(sched_current_uid(), kpw);
        break;
    }
    case SYS_MKDIR: {
        /* (path) -> Verzeichnis anlegen. Nur hdd1/hdd2 (hdd0 = System). */
        char kpath[64];
        if (copy_user_string(kpath, a0, sizeof(kpath)) != 0 || !user_path_allowed(kpath)) {
            ret = -1;
            break;
        }
        ret = vfs_mkdir(kpath);
        break;
    }
    case SYS_RMDIR: {
        /* (path) -> leeres Verzeichnis entfernen. Nur hdd1/hdd2. */
        char kpath[64];
        if (copy_user_string(kpath, a0, sizeof(kpath)) != 0 || !user_path_allowed(kpath)) {
            ret = -1;
            break;
        }
        ret = vfs_rmdir(kpath);
        break;
    }
    case SYS_GETCPU:
        ret = (long)cpu_id();              /* Kern, auf dem dieser EL0-Task gerade laeuft */
        break;
    case SYS_GETPPID:
        ret = (long)sched_current_ppid();  /* PID des Elternprozesses (0 = kernel-gestartet) */
        break;
    case SYS_WAIT:
        /* Blockiert bis das eigene Kind a0 endet; liefert dessen Exit-Code (>=0) / -1. */
        ret = sched_wait_pid((uint64_t)a0);
        break;
    case SYS_KILL:
        /* Kind (oder mit Admin-Cap beliebigen EL0-Prozess) a0 beenden. 0 / -1. */
        ret = sched_kill_pid((uint64_t)a0, sched_current_pid(),
                             (sched_current_caps() & USER_CAP_ADMIN) ? 1 : 0);
        break;
    case SYS_GUI_INFO: {
        /* Framebuffer-Geometrie + EL0-Backbuffer-VA in die User-Struktur schreiben (T2.1).
         * Nur mit USER_CAP_GUI -- sonst haette der Aufrufer das FB-Fenster gar nicht gemappt. */
        gui_fb_info_t info;
        if (!(sched_current_caps() & USER_CAP_GUI) ||
            gui_fb_info(&info) != 0 || !user_range_ok(a0, sizeof(info), /*write=*/1)) {
            ret = -1;
            break;
        }
        *(gui_fb_info_t *)(uintptr_t)a0 = info;   /* Seiten von user_range_ok EL0-schreibbar geprueft */
        break;
    }
    case SYS_POLL_EVENT: {
        /* Naechstes GUI-Eingabe-Event non-blocking (Maus/Taste). Nur mit USER_CAP_GUI. */
        if (!(sched_current_caps() & USER_CAP_GUI) ||
            !user_range_ok(a0, sizeof(gui_event_t), /*write=*/1)) {
            ret = -1;
            break;
        }
        gui_event_t ev;
        if (gui_input_pop(&ev)) {
            *(gui_event_t *)(uintptr_t)a0 = ev;
            ret = 1;
        } else {
            ret = 0;
        }
        break;
    }
    case SYS_WAIT_EVENT: {
        /* Blockiert bis ein GUI-Event vorliegt (schlaeft zwischen den Versuchen -> kein Busy-Poll,
         * SYS_KILL-Safe-Point). Nur mit USER_CAP_GUI. */
        if (!(sched_current_caps() & USER_CAP_GUI) ||
            !user_range_ok(a0, sizeof(gui_event_t), /*write=*/1)) {
            ret = -1;
            break;
        }
        gui_event_t ev;
        while (!gui_input_pop(&ev)) {
            sched_exit_if_killed();
            task_sleep_ticks(1);           /* ~10 ms schlafen, dann erneut versuchen */
        }
        *(gui_event_t *)(uintptr_t)a0 = ev;
        ret = 1;
        break;
    }
    case SYS_GUI_FLUSH: {
        /* Backbuffer-Zeilen [a0, a0+a1) -> echter FB + Cache-Flush (GPU). Nur mit USER_CAP_GUI. */
        if (!(sched_current_caps() & USER_CAP_GUI)) { ret = -1; break; }
        /* Die Kopie (~1,2 MiB + dc cvac) ist lang -> IRQs freigeben (wie console_readline/READCHAR),
         * damit Timer/Preemption laufen und parallele Tasks nicht bis zu ~1 ms aushungern. Gefahrlos:
         * es wird nur globaler Kernel-/Identity-Speicher beruehrt, kein Lock gehalten. */
        uint64_t f = READ_SYSREG(daif);
        irq_enable();
        ret = gui_fb_flush((uint32_t)a0, (uint32_t)a1);
        WRITE_SYSREG(daif, f);
        break;
    }
    case SYS_EXIT:
        uart_begin();                      /* Zeile cross-core atomar */
        uart_puts("[proc] User-Task beendet (exit ");
        uart_putdec(a0);
        uart_puts(").\n");
        uart_end();
        task_exit((int)a0);                /* Exit-Code an einen wartenden Elternprozess */
        break;
#ifdef VISION
    /* VISION (A1.5): Kernel-Parallel-For. PARSPAWN startet Worker fuer wid=1..n-1 (geteilter
     * ttbr0), der Aufrufer rechnet wid=0 selbst und JOIN'et; WORKER_DONE meldet + beendet den
     * Worker. Nur im -Vision-Build kompiliert -> ohne das Flag kein Dispatch-Zweig. */
    case SYS_VI_PARSPAWN:
        if (a0 < USER_BASE || a0 >= USER_STACK_TOP) { ret = -1; break; }   /* Worker-Fn muss EL0 sein */
        ret = vi_par_spawn(a0, a1, (int)a2, sched_current_uid(), sched_current_caps());
        break;
    case SYS_VI_PARJOIN:
        vi_par_join();
        break;
    case SYS_VI_WORKER_DONE:
        vi_par_worker_done();              /* sem_post + task_exit -> kehrt nie zurueck */
        break;
    case SYS_VI_CAM_GRAB:                  /* A4.1-Seam: Frame greifen (QEMU: kein Geraet -> -1) */
        if (a1 && !user_range_ok(a0, a1, /*write=*/1)) { ret = -1; break; }
        ret = vi_cam_grab(a0, a1);
        break;
    case SYS_VI_TICKS: {                   /* A5.2: a0!=0 -> CNTFRQ (Hz), sonst CNTPCT (monotoner Zaehler).
                                            * Beide an EL1 gelesen -> kein (evtl. trappender) EL0-Sysreg-Zugriff. */
        uint64_t t;
        if (a0) { __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(t)); }
        else    { __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t)); }
        ret = (long)t;
        break;
    }
#endif
    default:
        uart_puts("[syscall] unbekannte Nummer ");
        uart_putdec(num);
        uart_puts("\n");
        ret = -1;
        break;
    }

    frame[0] = (uint64_t)ret;              /* Rueckgabewert -> x0 */
}
