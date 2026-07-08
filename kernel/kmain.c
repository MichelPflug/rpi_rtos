/*
 * kernel/kmain.c  --  rpi_rtos Kernel-Einstieg
 *
 * Bringt MMU + Caches, Exception-Vektoren, GIC-400, Generic-Timer, den Scheduler
 * und den Storage-Stack hoch. Der Storage-Stack mountet zwei Partitionen
 * (hdd0 = System, hdd1 = User) ueber eine VFS-Mount-Tabelle und laedt die
 * User-Applikation INIT.ELF von der System-Partition hdd0.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "mmu.h"
#include "fdt.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"
#include "ipc.h"
#include "syscall.h"
#include "vfs.h"
#include "fat32.h"
#include "proc.h"
#include "gpio.h"
#include "spi.h"
#include "i2c.h"
#include "mailbox.h"
#include "crypto.h"
#include "user.h"
#include "fb.h"
#include "fbcon.h"
#include "gui_fb.h"
#include "dwc2.h"
#include "usbkbd.h"
#include "usbmsc.h"
#include "usbmouse.h"
#include "gui_input.h"
#include "httpd_fs.h"
#include "smp.h"
#include "uvc.h"   /* UVC-Klassen-Layer-Selbsttest (nur #ifdef VISION genutzt) */
#include "v3d.h"   /* Vulkan V5: V3D-HW-Erkennung (nur #ifdef V3D_PROBE genutzt) */
#include "dev_remote.h"   /* docs/architecture/20: Dev-Remote-Protokoll-Kern (nur #ifdef DEV_REMOTE genutzt) */
#include "dev_agent.h"    /* docs/architecture/20: Dev-Remote-UDP-Agent (nur #ifdef DEV_REMOTE genutzt) */
#include "diag_blink.h"   /* Blind-Boot-Diagnose ueber GPIO21 (nur #ifdef DIAG_BLINK genutzt) */
#include "diag_log.h"     /* Boot-Log auf die SD (hdd1:BOOTLOG.TXT) (nur #ifdef DIAG_LOG genutzt) */
#include "pcie.h"         /* BCM2711 PCIe/VL805-Diagnose (nur #ifdef PCIE_PROBE genutzt) */

uint64_t g_entry_el;   /* von start.S gesetzt: EL beim Kernel-Eintritt */
uint64_t g_dtb_ptr;    /* von start.S gesetzt: DTB-Zeiger (x0 beim Eintritt), 0 falls keiner */

/* Fail-closed-Halt waehrend des Boots (vor dem Scheduler-Start): IRQs maskieren, Grund
 * ausgeben, Kern anhalten. Die Sekundaerkerne sind zu diesem Zeitpunkt noch nicht
 * freigegeben (smp_sched_release folgt erst in [8]) -> das Gesamtsystem steht still.
 * Ein globales, alle Kerne stoppendes panic() mit Registerdump folgt separat (Roadmap T1.4). */
static void kpanic(const char *why)
{
    irq_disable();
    uart_puts("\n*** KERNEL-HALT (fail-closed): ");
    uart_puts(why);
    uart_puts(" ***\n");
    for (;;) { wfe(); }
}

#ifdef RTOS_SELFTEST
static int kcontains(const char *hay, const char *needle)
{
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) { j++; }
        if (!needle[j]) { return 1; }
    }
    return 0;
}

/* Guardian: FDT-Parser gegen einen EINGEBETTETEN Test-DTB pruefen. QEMU raspi4b
 * uebergibt keinen echten DTB -> der DTB-Pfad (echte HW) waere sonst gar nicht getestet.
 * Der Blob hat #address-cells/#size-cells=2/2 (64-bit) und ein /memory mit ZWEI Regionen:
 * eine Low-Region [0, 1 GiB) und eine HIGH-Region [4 GiB, 8 GiB) -> deckt genau den
 * 8-GB-Board-Fall (RAM >4 GiB) ab, den fdt_get_memory korrekt lesen muss. */
static uint8_t s_tdtb[256] __attribute__((aligned(8)));
static void tput32(uint32_t off, uint32_t v)
{
    s_tdtb[off] = (uint8_t)(v >> 24); s_tdtb[off + 1] = (uint8_t)(v >> 16);
    s_tdtb[off + 2] = (uint8_t)(v >> 8); s_tdtb[off + 3] = (uint8_t)v;
}
static int fdt_memory_selftest(void)
{
    for (int i = 0; i < 256; i++) { s_tdtb[i] = 0; }

    /* Struct-Block ab Offset 56 (Header 40 + Reservierungs-Map 16 = eine (0,0)-Endmarke). */
    uint32_t struct_off = 56, c = struct_off;
    tput32(c, 0x1); c += 4; tput32(c, 0); c += 4;                 /* BEGIN_NODE "" (root) */
    tput32(c, 0x3); c += 4; tput32(c, 4); c += 4; tput32(c, 0);  c += 4; tput32(c, 2); c += 4; /* #address-cells=2 */
    tput32(c, 0x3); c += 4; tput32(c, 4); c += 4; tput32(c, 15); c += 4; tput32(c, 2); c += 4; /* #size-cells=2 */
    tput32(c, 0x1); c += 4;                                        /* BEGIN_NODE "memory@0" */
    const char *mn = "memory@0";
    for (int i = 0; i < 8; i++) { s_tdtb[c + i] = (uint8_t)mn[i]; }
    s_tdtb[c + 8] = 0; c += 12;                                   /* 9 Byte -> auf 12 gepolstert */
    tput32(c, 0x3); c += 4; tput32(c, 32); c += 4; tput32(c, 27); c += 4;   /* PROP reg len=32 */
    tput32(c, 0); c += 4; tput32(c, 0);          c += 4;          /* Region1 base=0 */
    tput32(c, 0); c += 4; tput32(c, 0x40000000); c += 4;          /* Region1 size=1 GiB */
    tput32(c, 1); c += 4; tput32(c, 0);          c += 4;          /* Region2 base=0x1_00000000 */
    tput32(c, 1); c += 4; tput32(c, 0);          c += 4;          /* Region2 size=0x1_00000000 (4 GiB) */
    tput32(c, 0x2); c += 4; tput32(c, 0x2); c += 4; tput32(c, 0x9); c += 4; /* END_NODE x2, END */
    uint32_t struct_size = c - struct_off;

    uint32_t str_off = c;                                         /* Strings-Block */
    const char sac[] = "#address-cells", ssc[] = "#size-cells", sreg[] = "reg";
    uint32_t s = str_off;
    for (uint32_t i = 0; i < sizeof(sac); i++)  { s_tdtb[s++] = (uint8_t)sac[i]; }   /* Offset 0  */
    for (uint32_t i = 0; i < sizeof(ssc); i++)  { s_tdtb[s++] = (uint8_t)ssc[i]; }   /* Offset 15 */
    for (uint32_t i = 0; i < sizeof(sreg); i++) { s_tdtb[s++] = (uint8_t)sreg[i]; }  /* Offset 27 */
    uint32_t total = s;

    tput32(0, 0xd00dfeed); tput32(4, total); tput32(8, struct_off); tput32(12, str_off);
    tput32(16, 40); tput32(20, 17); tput32(24, 16); tput32(28, 0);
    tput32(32, s - str_off); tput32(36, struct_size);
    /* Reservierungs-Map @40: eine (0,0)-Endmarke -- Puffer bereits genullt. */

    fdt_mem_region_t tr[4];
    int tn = fdt_get_memory((uint64_t)(uintptr_t)s_tdtb, tr, 4);
    return (tn == 2 &&
            tr[0].base == 0x0ULL          && tr[0].size == 0x40000000ULL &&
            tr[1].base == 0x100000000ULL  && tr[1].size == 0x100000000ULL);
}
#endif

static void print_file(const char *path)
{
    static char buf[256];
    int n = vfs_read_file(path, buf, sizeof(buf) - 1);
    if (n >= 0) {
        if (n > (int)sizeof(buf) - 1) { n = (int)sizeof(buf) - 1; }   /* n = wahre Groesse -> auf Puffer klemmen */
        buf[n] = '\0';
        uart_puts("    ");
        uart_puts(path);
        uart_puts(" -> ");
        uart_puts(buf);
    } else {
        uart_puts("    ");
        uart_puts(path);
        uart_puts(" : nicht gefunden\n");
    }
}

/* Selbsttest: IPC-Timeouts. Nur im NICHT-interaktiven Selbsttest-Build (raspi-Suite):
 * im -Login-Build wuerde die lange Shell-Sitzung Kern 0 belegen, sodass der geweckte Testtask
 * erst spaet zum Messen der Elapsed-Zeit CPU bekaeme (Mess-Artefakt, kein Timeout-Fehler). */
#if defined(RTOS_SELFTEST) && !defined(INTERACTIVE_LOGIN)
/* --- IPC-Timeouts (sem_wait_timeout / mutex_lock_timeout) --- */
static semaphore_t s_ipct_sem;    /* fuer den Semaphor-Timeout/-Erfolg-Test */
static semaphore_t s_ipct_hold;   /* haelt den Mutex-Holder dauerhaft blockiert */
static mutex_t     s_ipct_mtx;    /* vom Holder gehalten -> Ziel des Mutex-Timeout-Tests */

/* Haelt s_ipct_mtx dauerhaft (blockiert danach fuer immer), damit der Timeout-Test darauf
 * auflaufen kann. Hoehere Prio (2) + zuerst erzeugt -> lockt vor dem Testtask. */
static void ipc_mtx_holder(void *arg)
{
    (void)arg;
    mutex_lock(&s_ipct_mtx);
    sem_wait(&s_ipct_hold);       /* niemand postet -> blockiert unbegrenzt, Mutex bleibt gehalten */
    task_exit(0);                 /* nie erreicht */
}

/* Beweist, dass blockierende IPC ein Timeout respektiert (RT-Kernzusage: beschraenkte
 * Worst-Case-Wartezeit). Deckt Timeout- UND Erfolgspfad ab. */
static void ipc_timeout_test(void *arg)
{
    (void)arg;
    /* Die Zeilen werden per uart_begin/uart_end cross-core-ATOMAR ausgegeben -- sonst kann
     * die Ausgabe eines EL0-Prozesses auf einem anderen Kern zwischen den Teil-put()-Aufrufen
     * einschieben und die Assert-Zeile zerreissen (der Testtask laeuft nach sched_start). */

    /* (1) Semaphor-Timeout: leeres Semaphor, niemand postet -> nach ~30 Ticks -1. */
    sem_init(&s_ipct_sem, 0);
    uint64_t t0 = timer_ticks();
    int r1 = sem_wait_timeout(&s_ipct_sem, 30);
    uint64_t d1 = timer_ticks() - t0;
    uart_begin();
    uart_puts("[ipctest] sem_wait_timeout(30t) ohne post -> ");
    uart_puts((r1 < 0 && d1 >= 25 && d1 <= 90) ? "Timeout(ret<0) nach ~" : "FEHLER dt=");
    uart_putdec(d1);
    uart_puts(" Ticks\n");
    uart_end();

    /* (2) Semaphor-Erfolg: count=1 -> sofort erwerben (Timeout bricht den Normalpfad nicht). */
    sem_init(&s_ipct_sem, 1);
    int r2 = sem_wait_timeout(&s_ipct_sem, 30);
    uart_begin();
    uart_puts("[ipctest] sem_wait_timeout(30t) mit count=1 -> ");
    uart_puts(r2 == 0 ? "sofort erworben(ret=0)\n" : "FEHLER\n");
    uart_end();

    /* (3) Mutex-Timeout: der Holder haelt s_ipct_mtx -> mutex_lock_timeout muss -1 liefern. */
    uint64_t t3 = timer_ticks();
    int r3 = mutex_lock_timeout(&s_ipct_mtx, 30);
    uint64_t d3 = timer_ticks() - t3;
    uart_begin();
    uart_puts("[ipctest] mutex_lock_timeout(30t) auf gehaltenem Mutex -> ");
    uart_puts((r3 < 0 && d3 >= 25 && d3 <= 90) ? "Timeout(ret<0) nach ~" : "FEHLER dt=");
    uart_putdec(d3);
    uart_puts(" Ticks\n");
    uart_end();

    task_exit(0);
}

/* --- Guardian: Cross-Core Priority-Inheritance-Boost stoesst den owner-Kern an --- */
static mutex_t     s_pi_mtx;     /* vom Holder (Kern 1) gehalten, vom Booster (Kern 0) kontestiert */
static semaphore_t s_pi_ready;   /* Holder -> Booster: Mutex ist jetzt gehalten */
static semaphore_t s_pi_done;    /* Booster -> Holder: gemessen, Mutex freigeben */

/* Holder auf Kern 1 (Prio 3 > Demo-Worker Prio 5 -> laeuft dort zuerst): nimmt den Mutex,
 * signalisiert, wartet bis der Booster gemessen hat, gibt frei. */
static void pi_holder(void *arg)
{
    (void)arg;
    mutex_lock(&s_pi_mtx);
    sem_post(&s_pi_ready);
    sem_wait(&s_pi_done);
    mutex_unlock(&s_pi_mtx);
    task_exit(0);
}

/* Booster auf Kern 0 (Prio 2 > Holder-Prio 3 -> loest PI-Boost aus). Der Boost hebt den
 * Holder (owner = Kern 1) hoch -> sched_set_prio muss den FREMDEN Kern via need_resched+SGI
 * anstossen (Zaehler steigt). Frueher fehlte dieser Handshake -> Boost erst bei Slice-Ablauf. */
static void pi_booster(void *arg)
{
    (void)arg;
    sem_wait(&s_pi_ready);                        /* Holder haelt den Mutex jetzt */
    uint32_t before = sched_pi_remote_count();
    (void)mutex_lock_timeout(&s_pi_mtx, 20);      /* boostet den Kern-1-Holder cross-core -> Timeout */
    uint32_t after = sched_pi_remote_count();
    sem_post(&s_pi_done);                          /* Holder darf freigeben/enden */
    uart_begin();
    uart_puts("[pitest] cross-core PI-Boost -> Reschedule-IPI an owner-Kern: ");
    uart_puts((after > before) ? "ja\n" : "NEIN\n");
    uart_end();
    task_exit(0);
}

/* --- Guardian: Mailbox-Transaktions-Serialisierung ueber Kerne ---
 * Je ein Hammer-Task auf Kern 1 und Kern 2 feuert viele idempotente Firmware-Revision-Reads
 * gegen die GETEILTE VideoCore-Mailbox. MIT dem Transaktions-Lock ist immer nur EIN Kern
 * gleichzeitig in der Transaktion (max-Belegung==1); fehlte das Lock, ueberlappten sich die
 * Transaktionen (occ>1) und der Verletzungs-Latch schluege an. Der Checker (Kern 0) wartet
 * per Semaphor auf beide Hammer und meldet Belegung/Verletzung/Fehler cross-core-atomar. */
static semaphore_t     s_mbox_done;
static volatile uint32_t s_mbox_fail;   /* Transfers mit Rueckgabe != 0 (racy-inkrementiert, ok) */
#define MBOX_HAMMER_ITERS 120
#define MBOX_HAMMERS      2             /* Kerne 1 und 2 */

static void mbox_hammer(void *arg)
{
    (void)arg;
    for (int i = 0; i < MBOX_HAMMER_ITERS; i++) {
        if (mailbox_get_fw_rev(0) != 0) {
            s_mbox_fail++;
        }
    }
    sem_post(&s_mbox_done);
    task_exit(0);
}

static void mbox_checker(void *arg)
{
    (void)arg;
    for (int i = 0; i < MBOX_HAMMERS; i++) {
        sem_wait(&s_mbox_done);         /* auf beide Hammer warten */
    }
    uart_begin();
    uart_puts("[mboxtest] SMP-Mailbox-Serialisierung: Transfers-Fehler=");
    uart_putdec(s_mbox_fail);
    uart_puts(" max-Belegung=");
    uart_putdec(mailbox_occ_max());
    uart_puts(" Verletzung(occ>1)=");
    uart_puts(mailbox_occ_violation() ? "JA(FEHLER)\n" : "nein\n");
    uart_end();
    task_exit(0);
}

/* --- Guardian: secbuf-Serialisierung (vfs_list nimmt jetzt fs_lock) ---
 * Kern 1 ruft wiederholt vfs_list("hdd0") auf (die reparierte, jetzt verriegelte Funktion),
 * Kern 2 hammert still vfs_listdir("hdd0", buf) (bereits verriegelt) -- beide gehen durch
 * dir_iterate auf dem GETEILTEN secbuf. Mit fs_lock ist immer nur EIN Kern im Verzeichnis-Scan
 * (max-Belegung==1); fehlte vfs_list das Lock, ueberlappten die Scans (occ>1) und der
 * secbuf-Verletzungs-Latch schluege an. Der Checker (Kern 0) wertet nach beiden Hammern aus. */
static semaphore_t s_secbuf_done;
#define SECBUF_LIST_ITERS  6            /* Kern 1: wenige vfs_list (begrenzt die Konsolenausgabe) */
#define SECBUF_LDIR_ITERS  60           /* Kern 2: stiller listdir-Hammer haelt das Fenster praesent */

static void secbuf_list_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < SECBUF_LIST_ITERS; i++) {
        vfs_list("hdd0");               /* wenige Eintraege -> minimale Ausgabe */
    }
    sem_post(&s_secbuf_done);
    task_exit(0);
}

static void secbuf_ldir_task(void *arg)
{
    (void)arg;
    static char lb[512];
    for (int i = 0; i < SECBUF_LDIR_ITERS; i++) {
        (void)vfs_listdir("hdd0", lb, sizeof(lb));   /* still: in Puffer, keine Konsole */
    }
    sem_post(&s_secbuf_done);
    task_exit(0);
}

static void secbuf_checker(void *arg)
{
    (void)arg;
    sem_wait(&s_secbuf_done);           /* auf beide Hammer warten */
    sem_wait(&s_secbuf_done);
    fat32_secbuf_widen = 0;             /* Beobachtungsfenster wieder schliessen */
    uart_begin();
    uart_puts("[vfslocktest] secbuf-Serialisierung (vfs_list+listdir): max-Belegung=");
    uart_putdec(fat32_secbuf_occ_max());
    uart_puts(" Verletzung(occ>1)=");
    uart_puts(fat32_secbuf_occ_violation() ? "JA(FEHLER)\n" : "nein\n");
    uart_end();
    task_exit(0);
}
#endif /* RTOS_SELFTEST && !INTERACTIVE_LOGIN */

#ifdef PANIC_SELFTEST
/* Guardian: loest NACH dem Scheduler-Start (Sekundaerkerne laufen) absichtlich einen
 * EL1-Kernel-Fault aus -> muss in panic() muenden, das ALLE Kerne stoppt (nicht nur diesen).
 * Nur im dedizierten -PanicTest-Build (halt das System bewusst an). */
static void panic_test_task(void *arg)
{
    (void)arg;
    task_sleep_ticks(30);   /* Sekundaerkerne ihre Scheduler laufen lassen (Halt-SGI erreicht sie) */
    uart_puts("[paniktest] loese absichtlich einen EL1-Kernel-Fault aus (Lesen @4GiB unmapped)...\n");
    volatile uint64_t v = *(volatile uint64_t *)0x100000000ULL;   /* 4 GiB: unmapped -> Data Abort */
    (void)v;
    uart_puts("[paniktest] FEHLER: kein Fault ausgeloest\n");     /* nie erreicht */
    task_exit(0);
}
#endif

#ifdef RMUTEX_PANIC_TEST
/* Guardian: loest NACH dem Scheduler-Start ein REKURSIVES mutex_lock aus -> muss in
 * panic() muenden (fail-loud), statt still zu no-oppen. Nur im dedizierten -MutexPanicTest-Build. */
static mutex_t s_rmtx;
static void rmutex_panic_task(void *arg)
{
    (void)arg;
    task_sleep_ticks(30);
    uart_puts("[rmutextest] loese absichtlich ein rekursives mutex_lock aus...\n");
    mutex_init(&s_rmtx);
    mutex_lock(&s_rmtx);
    mutex_lock(&s_rmtx);   /* rekursiv auf einstufigem Mutex -> panic (fail-loud) */
    uart_puts("[rmutextest] FEHLER: kein panic ausgeloest\n");   /* nie erreicht */
    task_exit(0);
}
#endif

void kmain(void)
{
#ifdef DIAG_BLINK
    diag_latch(4);       /* Kernel-Eintritt erreicht (Pin 7) -- VOR uart_init, reines GPIO-Register */
#endif
    uart_init();
#ifdef DIAG_BLINK
    diag_latch(18);      /* uart_init ueberstanden (Pin 12) */
#endif

    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("   rpi_rtos  -  Storage: hdd0 + hdd1\n");
    uart_puts("========================================\n");
    uart_puts("Start auf EL");
    uart_putdec(current_el());
    uart_puts(" (Eintritt vom Bootloader auf EL");
    uart_putdec(g_entry_el);
    uart_puts(")\n");

    uart_puts("[1] Exception-Vektoren (VBAR_EL1)...\n");
#ifdef DIAG_BLINK
    diag_latch(12);      /* Banner + [1]-Ausgabe fertig, VOR exceptions_init (Pin 32) */
#endif
    exceptions_init();
#ifdef DIAG_BLINK
    diag_latch(23);      /* exceptions_init ueberstanden (Pin 16) */
#endif

    uart_puts("[2] MMU + Caches...\n");
    mmu_init();
    uart_lock_online();  /* MMU/Caches AN -> ab jetzt darf der UART-Spinlock LDAXR/STXR nutzen */
#ifdef DIAG_BLINK
    diag_latch(17);      /* MMU aktiviert + ueberlebt (Pin 11) */
#endif

    /* RAM-Ausbau melden. Quelle = DTB /memory (echte HW -> voller RAM inkl.
     * >4 GiB) oder Grobkarte (kein DTB, z.B. QEMU raspi4b = 2 GiB, 0..3 GiB Normal gemappt).
     * Mailbox ARM/VC-Split nur als Diagnose (32-bit, nur LOW-Region). */
    uart_puts("[2a] RAM-Ausbau (T1.12): Quelle=");
    uart_puts(mmu_ram_from_dtb() ? "DTB-/memory" : "Grobkarte-ohne-DTB");
    uart_puts(" Normal-RAM-gemappt=");
    uart_puthex(mmu_ram_mapped());
    uart_puts("\n");
    {
        uint32_t ab = 0, as = 0, vb = 0, vs = 0;
        (void)mailbox_get_arm_memory(&ab, &as);
        (void)mailbox_get_vc_memory(&vb, &vs);
        uart_puts("    [ram] Mailbox-Split (Diagnose): ARM=");
        uart_puthex(as);
        uart_puts(" VC=");
        uart_puthex(vs);
        uart_puts(" DTB-Zeiger=");
        uart_puthex(g_dtb_ptr);
        uart_puts("\n");
    }
#ifdef RTOS_SELFTEST
    uart_puts("    [ram] FDT-Parser-Selbsttest (/memory, 2 Regionen inkl. High-RAM >4GiB): ");
    uart_puts(fdt_memory_selftest() ? "ok\n" : "FEHLER\n");
#endif

#ifdef DIAG_BLINK
    diag_latch(27);      /* Mailbox-Aufrufe ueberstanden -> Mailbox/MMIO ok (Pin 13) */
#endif

    /* HDMI frueh hochziehen, damit Boot, Login und Shell auf dem Bildschirm
     * erscheinen: nach fbcon_init wird jede serielle Ausgabe zusaetzlich auf die
     * Framebuffer-Textkonsole gespiegelt (Eingabe weiterhin ueber Serial). */
    uart_puts("[2b] HDMI/Framebuffer-Konsole (VideoCore-Mailbox):\n");
    /* Aufloesung: Standard 640x480 (passt in den 2-MiB-gui_fb-Backbuffer -> GUI-Builds/Suites intakt).
     * NUR mit -DFULLHD (ShellImage): Full-HD 1920x1080 anfordern -- die Firmware klemmt bei
     * Nichtunterstuetzung auf einen kleineren Modus, fb_init liest die TATSAECHLiche Groesse aus der
     * GPU-Antwort zurueck. config.txt muss dann hdmi_safe=1 (erzwingt 640x480) losgeworden sein. Bei
     * 1080p faellt die gui_fb-Bruecke sauber weg (FB > Backbuffer); Shell/fbcon laeuft weiter. */
#ifdef FULLHD
    if (fb_init(1920, 1080) == 0) {
#else
    if (fb_init(640, 480) == 0) {
#endif
        const fb_t *fb = fb_get();
        /* Pixel-Selbsttest (4 Eckpixel) -> beweist Rendering im FB-RAM, bevor
         * fbcon_init den Schirm loescht. */
        fb_pixel(0, 0, 0xFF0000); fb_pixel(1, 0, 0x00FF00);
        fb_pixel(2, 0, 0x0000FF); fb_pixel(3, 0, 0xFFFFFF);
        volatile uint32_t *p0 = (volatile uint32_t *)fb->base;
        uart_puts("    [fb] "); uart_putdec(fb->width); uart_puts("x"); uart_putdec(fb->height);
        uart_puts(" @ base=");
        uart_puthex((uint64_t)(uintptr_t)fb->base);
        uart_puts(" pitch=");
        uart_putdec(fb->pitch);
        uart_puts(" size=");
        uart_putdec(fb->size);
        uart_puts("\n    [fb] testbild r=");
        uart_puthex(p0[0]); uart_puts(" g="); uart_puthex(p0[1]);
        uart_puts(" b="); uart_puthex(p0[2]); uart_puts(" w="); uart_puthex(p0[3]);
        uart_puts("\n");

        fbcon_init(0xFFFFFF, 0x001030);     /* weiss auf dunkelblau, loescht den Schirm */
        uart_set_mirror(fbcon_putc);        /* ab jetzt: serielle Ausgabe auch auf HDMI */
#ifdef DIAG_BLINK
        diag_latch(22);      /* fb_init + fbcon ERFOLG -> Framebuffer ok (Pin 15) */
#endif
        /* Banner in Gruen (ANSI-SGR) -> faerbt sowohl das Serial-Terminal als auch die
         * HDMI-Konsole (fbcon parst ESC[..m). */
        uart_puts("\x1b[32mrpi_rtos HDMI-Konsole aktiv -- Ausgabe gespiegelt.\x1b[0m\n");

        /* Nachweis, dass der gespiegelte Text gerendert wird UND die Farbe greift:
         * im Bannerbereich (Zeilen 0..7) muss es Nicht-Hintergrund- und gruene Pixel geben. */
        int painted = 0, colored = 0;
        for (uint32_t y = 0; y < 8; y++) {
            volatile uint32_t *row = (volatile uint32_t *)(fb->base + (uint64_t)y * fb->pitch);
            for (uint32_t x = 0; x < fb->width; x++) {
                if (row[x] != 0x001030u) { painted = 1; }
                if (row[x] == 0x00FF00u) { colored = 1; }
            }
        }
        uart_puts("    [fb] konsole-render: ");
        uart_puts(painted ? "ja\n" : "nein\n");
        uart_puts("    [fb] konsole-farbe: ");
        uart_puts(colored ? "ja\n" : "nein\n");

        /* GUI-Grafik-Bruecke: Backbuffer an den FB binden, damit EL0-GUI-Apps zeichnen
         * koennen. Der Kernel-Selbsttest beweist die Backbuffer->FB-Kopie + GPU-Cache-Flush. */
        if (gui_fb_init() == 0) {
            uart_puts("    [gui] Grafik-Bruecke bereit (Backbuffer @ EL0-VA 0x18000000, nur mit GUI-Cap)\n");
#ifdef RTOS_SELFTEST
            uart_puts("    [gui] bridge: bb->fb ");
            uart_puts(gui_fb_selftest() ? "ok\n" : "FEHLER\n");
            uart_puts("    [gui] cursor-overlay: ");
            uart_puts(gui_fb_cursor_selftest() ? "ok\n" : "FEHLER\n");
#endif
        } else {
            uart_puts("    [gui] Grafik-Bruecke nicht verfuegbar (FB zu gross/fehlt)\n");
        }
    } else {
        uart_puts("    [fb] fb_init fehlgeschlagen\n");
    }

    uart_puts("[3] GIC-400...\n");
    gic_init();
#ifdef DIAG_BLINK
    diag_latch(5);           /* GIC-400 initialisiert (Pin 29) */
#endif

    uart_puts("[4] Generic Timer (100 Hz)...\n");
    timer_init(100);
#ifdef DIAG_BLINK
    diag_latch(6);           /* Timer initialisiert (Pin 31) */
#endif

    /* SMP NACH GIC [3] + Timer [4]: der Distributor ist an (sonst keine PPI-Zustellung)
     * und das Tick-Intervall steht. Die Sekundaerkerne aktivieren Per-Core-MMU/GIC/Timer,
     * fuehren den Spinlock-Lasttest aus und nehmen danach ihre eigenen Timer-IRQs an. */
    uart_puts("[4c] SMP: Sekundaerkerne freigeben + Per-Core-IRQ + Spinlock-Lasttest...\n");
    smp_init_secondaries();

    uart_puts("[4b] USB (DWC2 Host-Controller):\n");
    if (dwc2_init() == 0) {
        if (dwc2_port_reset_detect() != USB_SPEED_NONE && dwc2_enumerate() == 0) {
            if (dwc2_dev_kind() == 1) {           /* HID-Tastatur */
                usbkbd_enable();                  /* Tastendruecke fuer console_getc bereitstellen */
                dwc2_kbd_irq_enable();            /* IRQ-getrieben (GIC-SPI 105) statt Polling */
                uart_puts("    [usb] HID-Tastatur bereit (IRQ-getrieben, Eingabe an Login/Shell)\n");
            } else if (dwc2_dev_kind() == 2) {    /* Massenspeicher */
#ifdef RTOS_SELFTEST
                /* usbmsc_probe schreibt zum Selbsttest ein Muster auf den LETZTEN Sektor und
                 * stellt ihn wieder her -- bei Power-Loss zwischen Schreiben und Restore geht
                 * der Nutzer-Sektor verloren. Daher NUR im Selbsttest-Build, nie im RC-Image.
                 * Produktiv wird der Stick unten regulaer als hdd2 gemountet (read/write ueber VFS). */
                usbmsc_probe();                   /* INQUIRY + READ CAPACITY + (destruktiver) Sektor-Schreibtest */
#endif
            } else if (dwc2_dev_kind() == 3) {    /* HID-Maus (T2.3) */
                const fb_t *fbm = fb_get();
                usbmouse_enable(fbm ? fbm->width : 640, fbm ? fbm->height : 480);
#ifdef RTOS_SELFTEST
                uart_puts("    [mouse] decode-selbsttest: ");
                uart_puts(usbmouse_selftest() ? "ok\n" : "FEHLER\n");
                uart_puts("    [event] queue-selbsttest: ");
                uart_puts(gui_input_selftest() ? "ok\n" : "FEHLER\n");
                /* 2 synthetische Maus-Events VOR gui_input_enable einreihen (dann ist der Timer-Tick
                 * noch KEIN zweiter Producer): guitest liest sie via SYS_POLL_EVENT (EL0-Event-Pfad). */
                {
                    gui_event_t e1 = { GUI_EV_MOUSE, 0x00, 0, 0, 111, 222 };
                    gui_event_t e2 = { GUI_EV_MOUSE, 0x01, 0, 0, 150, 260 };
                    gui_input_push(&e1); gui_input_push(&e2);
                }
#endif
                gui_input_enable();               /* ab jetzt pollt der Timer-Tick die Maus */
                dwc2_kbd_irq_enable();            /* generischer HID-Interrupt-IN (GIC-SPI 105) */
#ifdef RTOS_SELFTEST
                /* Beweist, dass der HID-IRQ fuer die MAUS (dev_kind==3) scharfgeschaltet wurde --
                 * genau der Pfad, der frueher am dev_kind==1-Guard scheiterte (Maus lieferte 0 Events). */
                uart_puts("    [mouse] irq-armed: ");
                uart_puts(dwc2_kbd_irq_active() ? "ok\n" : "FEHLER\n");
#endif
                uart_puts("    [usb] HID-Maus bereit (IRQ-getrieben, Cursor mittig)\n");
            }
        } else {
            dwc2_hotplug_enable();                /* kein Geraet -> auf Hot-Plug-IRQ warten */
        }
    }

    uart_puts("[5] Storage: EMMC2/SD + MBR + FAT32 (hdd0 + hdd1)...\n");
    int storage_ok = (vfs_init() == 0);
#ifdef DIAG_BLINK
    diag_latch(26);          /* vfs_init [5] zurueckgekehrt -> SD/Storage-Init ueberstanden (Pin 37) */
#endif
#ifdef DIAG_LOG
    /* Fruehester moeglicher SD-Schnitt: erfasst den gesamten Log bis hier ([1]-[5] inkl. fb_init-
     * Ergebnis). Erscheint hdd1:BOOTLOG.TXT NICHT, haengt der Kernel schon vor [5]. */
    if (storage_ok) { diag_log_to_sd(); }
#endif
    if (storage_ok) {
        /* Falls ein USB-Massenspeicher enumeriert wurde: als hdd2 einhaengen. */
        if (dwc2_dev_kind() == 2) {
            vfs_mount_usb();
        }
#ifdef RTOS_SELFTEST
        /* Ausfuehrliche Verzeichnis-Auflistung nur im Selbsttest-Build -- im schlanken Shell-/RC-Image
         * unerwuenschtes Boot-Rauschen. */
        uart_puts("    Mounts + Wurzelverzeichnisse:\n");
        vfs_list_all();
#endif
        print_file("hdd0:SYSTEM.TXT");
        print_file("hdd1:WELCOME.TXT");
        if (dwc2_dev_kind() == 2) {
            print_file("hdd2:USBINFO.TXT");      /* Datei vom USB-Stick */
        }

#ifdef RTOS_SELFTEST
        uart_puts("    Schreibtest (hdd1 = User schreibbar, hdd0 = read-only):\n");
        static const char wmsg[] = "Vom rpi_rtos-Kernel zur Laufzeit geschrieben.\r\n";
        int w = vfs_write_file("hdd1:KERNEL.TXT", wmsg, sizeof(wmsg) - 1);
        if (w >= 0) {
            uart_puts("      hdd1:KERNEL.TXT geschrieben (");
            uart_putdec(w);
            uart_puts(" Bytes), lese zurueck:\n");
            print_file("hdd1:KERNEL.TXT");
        } else {
            uart_puts("      Schreiben fehlgeschlagen\n");
        }
        vfs_write_file("hdd0:NOPE.TXT", wmsg, sizeof(wmsg) - 1);   /* -> read-only */
        uart_puts("    hdd1-Verzeichnis nach dem Schreiben:\n");
        vfs_list("hdd1");

        /* LFN-Schreibtest: langer Dateiname -> 8.3-Alias + LFN-Eintraege; per langem
         * Namen zurueckgelesen (deckt fat32_write_file LFN-Zweig + LFN-Lesen ab). */
        static const char lmsg[] = "LFN-Schreibtest vom Kernel (langer Dateiname).\r\n";
        if (vfs_write_file("hdd1:LangerDateiname.txt", lmsg, sizeof(lmsg) - 1) >= 0) {
            uart_puts("    hdd1:LangerDateiname.txt geschrieben (LFN), lese zurueck:\n");
            print_file("hdd1:LangerDateiname.txt");
        } else {
            uart_puts("    LFN-Schreiben fehlgeschlagen\n");
        }

        /* FS-Reife: (a) Trunkierungssignal (statt still abzuschneiden), (b) FSInfo-Free-Count. */
        uart_puts("[5f] FS-Reife (Trunkierung + FSInfo-Free-Count):\n");
        {
            /* (a) INDEX.HTM (88 B) mit 10-Byte-Puffer lesen -> return == WAHRE Groesse (88 > 10). */
            static char tb[16];
            int tn = vfs_read_file("hdd1:INDEX.HTM", tb, 10);
            uart_puts("    read(INDEX.HTM, buf=10) -> ");
            uart_putdec(tn >= 0 ? (unsigned)tn : 0);
            uart_puts(tn > 10 ? " (>10: Trunkierung signalisiert)\n" : " (KEINE Trunkierung)\n");

            /* (b) free_count muss bei Alloc (Schreiben) sinken und bei Free (Loeschen) steigen. */
            uint32_t f0 = vfs_free_count("hdd1");
            static const char fmsg[] = "FSInfo-Free-Count-Test.\r\n";
            vfs_write_file("hdd1:FSITEST.TXT", fmsg, sizeof(fmsg) - 1);
            uint32_t f1 = vfs_free_count("hdd1");
            vfs_delete("hdd1:FSITEST.TXT");
            uint32_t f2 = vfs_free_count("hdd1");
            uart_puts("    FSInfo free_count ");
            uart_putdec(f0); uart_puts(" -alloc-> "); uart_putdec(f1);
            uart_puts(" -free-> ");  uart_putdec(f2);
            uart_puts((f1 == f0 - 1 && f2 == f0) ? "  (gepflegt)\n" : "  (NICHT gepflegt)\n");

            /* (c) LFN-Loeschen ueber Sektorgrenze: CrossBoundary.txt (LFN-Lauf ueber die 512-B-
             *     Grenze im DOCS-Cluster, vom Image-Generator gelegt) loeschen -> Datei weg, der
             *     Folge-Eintrag MARKER.TXT bleibt intakt (kein Nachbar korrumpiert/Absturz beim
             *     Kreuzen der Sektorgrenze). Assertion auf dem LISTING (Lauf 2 laeuft auf dem
             *     bereits geaenderten Image -> Delete kann -1 liefern, Zustand ist aber gleich). */
            vfs_delete("hdd1:DOCS/CrossBoundary.txt");
            static char db[512];
            int dl = vfs_listdir("hdd1:DOCS", db, sizeof(db) - 1);
            db[(dl >= 0 && dl < (int)sizeof(db)) ? dl : 0] = '\0';
            int ok_marker = kcontains(db, "MARKER.TXT");
            int no_cross  = !kcontains(db, "CrossBoundary");
            uart_puts("    LFN-Loeschen ueber Sektorgrenze (CrossBoundary.txt): ");
            uart_puts((ok_marker && no_cross) ? "geloescht, MARKER.TXT intakt\n" : "FEHLER\n");
        }
#endif /* RTOS_SELFTEST */
    } else {
        uart_puts("    Storage-Init fehlgeschlagen.\n");
    }

#ifdef RTOS_SELFTEST
    uart_puts("[5b] Chipsatz-Funktionen (GPIO/SPI/I2C) demonstrieren:\n");
    {
        /* GPIO: Pin 21 als Ausgang konfigurieren, Richtung zuruecklesen (prueft
         * gpio_set_function), dann setzen/loeschen und Pegel zurueckmessen. */
        gpio_set_function(21, GPIO_FUNC_OUTPUT);
        int is_out = (gpio_get_function(21) == GPIO_FUNC_OUTPUT);
        gpio_set(21);
        int hi = gpio_level(21);
        gpio_clear(21);
        int lo = gpio_level(21);
        uart_puts("    GPIO21 dir=");
        uart_puts(is_out ? "output" : "FALSCH");
        uart_puts(" set->");
        uart_putdec(hi);
        uart_puts(" clr->");
        uart_putdec(lo);
        uart_puts("\n");

        /* SPI0: Vollduplex-Transfer (ohne Slave -> RX 0, aber DONE/Ablauf ok). */
        spi_config_t sc = { .clock_hz = 1000000, .mode = 0, .cs = 0 };
        spi_init(&sc);
        static const uint8_t tx[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        uint8_t rx[4] = { 0 };
        int sr = spi_transfer(tx, rx, 4);
        uart_puts("    SPI0 Transfer 4 Byte: ");
        uart_puts(sr == 0 ? "ok (DONE)\n" : "Timeout\n");

        /* I2C BSC1: Schreibversuch an 0x50 (ohne Slave -> kein ACK, sauber -1). */
        i2c_config_t ic = { .clock_hz = 100000 };
        i2c_init(&ic);
        uint8_t reg = 0x00;
        int ir = i2c_write(0x50, &reg, 1);
        uart_puts("    I2C BSC1 write(0x50): ");
        uart_puts(ir == 0 ? "ACK\n" : "kein ACK (ohne Slave erwartbar)\n");
    }
#endif /* RTOS_SELFTEST ([5b] Chipsatz-Demo) */

    /* Produktions-Sicherheitsgates: laufen in JEDEM Build und sind FAIL-CLOSED.
     * Ohne bestandenen Krypto-KAT und ladbare Benutzer-DB darf der Kernel nicht weiter
     * Richtung Login/Shell -- lieber definiert anhalten als mit kaputtem KDF Passwoerter
     * pruefen oder ohne Credential-Quelle booten. */
    uart_puts("[5c] Sicherheits-Init (Krypto-Selbsttest + Benutzer-DB, fail-closed):\n");
    if (!storage_ok) {
        kpanic("Storage nicht verfuegbar -- Benutzer-DB (hdd0) unerreichbar");
    }
    uart_puts("    Krypto-Selbsttest: ");
    if (crypto_selftest() != 0) {
        uart_puts("FEHLGESCHLAGEN\n");
        kpanic("crypto_selftest (SHA-256/PBKDF2 KAT) fehlgeschlagen -- KDF unbrauchbar");
    }
    uart_puts("ok (SHA-256/PBKDF2)\n");
    /* FP-Policy UNBEDINGT melden (T1.8/T3.1): das RC-Image muss FPEN=0b00 tragen (FP/SIMD
     * trappt), GUI_FP-Builds 0b11 (EL0-FP + per-Task-FP-Kontext). Der Release-Verify und
     * der HW-Poller asserten den 0b00-Marker (tools/rc_markers.ps1) -- vorher war die
     * Haertungs-Invariante auf dem Produktions-Image zur Laufzeit unbelegt. */
    {
        uint32_t fpen = ((uint32_t)(READ_SYSREG(cpacr_el1) >> 20)) & 3u;
        uart_puts("    CPACR_EL1.FPEN=");
        if (fpen == 0u)      { uart_puts("0b00 (FP/SIMD-Trap aktiv, T1.8-Haertung)\n"); }
        else if (fpen == 3u) { uart_puts("0b11 (EL0-FP frei, FP-Kontext je Task, T3.1)\n"); }
        else                 { uart_puts("UNERWARTET\n"); }
    }
#ifdef RTOS_SELFTEST
    /* Crash-Sicherheit der Benutzer-DB: korruptes Primaer -> Wiederherstellung aus USERS.BAK.
     * Self-contained (eigene Test-Dateien, setzt s_users zurueck) -> laeuft VOR dem echten
     * user_init, das danach die reale DB laedt. */
    uart_puts("    [selftest] DB-Recovery (korruptes Primaer -> Backup): ");
    uart_puts(user_selftest_db_recovery() ? "ja\n" : "NEIN\n");
    /* AT-S1E0-User-Access-Pruefung (T1.4): AT lehnt inaccessible Adressen korrekt ab. */
    uart_puts("    [selftest] AT-User-Access (lehnt inaccessible ab): ");
    uart_puts(uaccess_selftest() ? "ja\n" : "NEIN\n");
    /* CPACR_EL1.FPEN[21:20]: normalerweise 0b00 (FP trappt). In GUI-Builds (GUI_FP) 0b11
     * (EL0-FP fuer die Laufzeit-TTF-Rasterung freigeschaltet). */
    {
        uint32_t fpen = ((uint32_t)(READ_SYSREG(cpacr_el1) >> 20)) & 3u;
#ifdef GUI_FP
        uart_puts("    [selftest] CPACR_EL1.FPEN (EL0-FP fuer TTF freigeschaltet): ");
        uart_puts(fpen == 3u ? "0b11 (ja)\n" : "!=3 (NEIN)\n");
#else
        uart_puts("    [selftest] CPACR_EL1.FPEN (FP-Trap EL0+EL1): ");
        uart_puts(fpen == 0u ? "0b00 (ja)\n" : "!=0 (NEIN)\n");
#endif
    }
#endif
    if (user_init() != 0) {                          /* DB laden (mit Backup-Fallback) oder Default-admin */
        kpanic("user_init fehlgeschlagen -- Benutzer-DB unbrauchbar (kein sicherer Login)");
    }
#ifdef RTOS_SELFTEST
    user_list();
    /* Demo/Selbsttest: hartkodiertes Testkonto 'alice' + Rechte-/Negativ-Faelle. NUR im
     * Selbsttest-Build -- das RC-Image (ohne RTOS_SELFTEST) enthaelt KEINE hartkodierten
     * Credentials und keine Auth-Demos. */
    if (user_authenticate("alice", "geheim123") < 0) {
        int r = user_add("alice", "geheim123", 0, USER_CAP_ADMIN);
        uart_puts("    user_add(alice) als Admin: ");
        uart_puts(r == 0 ? "ok\n" : "fehlgeschlagen\n");
    }
    {
        int uid = user_authenticate("alice", "geheim123");
        uart_puts("    Login alice (richtiges Passwort): ");
        if (uid >= 0) { uart_puts("ok, uid="); uart_putdec((unsigned)uid); uart_puts("\n"); }
        else          { uart_puts("ABGELEHNT(!)\n"); }
    }
    uart_puts("    Login alice (falsches Passwort): ");
    uart_puts(user_authenticate("alice", "falsch") < 0 ? "abgelehnt (korrekt)\n"
                                                       : "AKZEPTIERT(!)\n");
    uart_puts("    user_add ohne Admin-Recht: ");
    uart_puts(user_add("mallory", "x", 0, 0) < 0 ? "verweigert (korrekt)\n"
                                                 : "ERLAUBT(!)\n");
#endif

#ifdef RTOS_SELFTEST
    if (storage_ok) {
        /* HTTP-Server-VFS-Resolver: in QEMU raspi4b gibt es kein NIC, daher wird der
         * (sicherheitskritische) Resolver hier DIREKT geprueft -- Datei-Aufloesung,
         * Content-Type und vor allem der Directory-Traversal-Schutz gegen hdd0. Die
         * HTTP-Protokollschicht ist separat auf der virt-Maschine verifiziert. */
        uart_puts("[5e] HTTP-Server-VFS-Resolver (Doc-Root hdd1, Traversal-Schutz):\n");
        httpd_fs_init("hdd1");
        const uint8_t *b; uint16_t l; const char *ct;

        if (httpd_fs_resolve("/INDEX.HTM", &b, &l, &ct) == 0) {
            uart_puts("    /INDEX.HTM -> ");
            uart_putdec(l);
            uart_puts(" Byte, ctype=");
            uart_puts(ct);
            uart_puts("\n      ");
            for (uint16_t i = 0; i < l && i < 120; i++) { uart_putc((char)b[i]); }
            uart_puts("\n");
        } else {
            uart_puts("    /INDEX.HTM : nicht gefunden(!)\n");
        }
        if (httpd_fs_resolve("/STYLE.CSS", &b, &l, &ct) == 0) {
            uart_puts("    /STYLE.CSS -> ctype=");
            uart_puts(ct);
            uart_puts("\n");
        }
        if (httpd_fs_resolve("/", &b, &l, &ct) == 0) {
            uart_puts("    / (Index) -> ");
            uart_putdec(l);
            uart_puts(" Byte Listing\n");
        }
        /* Directory-Traversal / Doc-Root-Escape MUSS abgewiesen werden (alle -> -1). */
        int esc1 = httpd_fs_resolve("/../hdd0:SYSTEM.TXT", &b, &l, &ct);   /* ".." + ":" */
        int esc2 = httpd_fs_resolve("/..%2f..", &b, &l, &ct);              /* ".." (literal) */
        int esc3 = httpd_fs_resolve("/hdd0:USERS.DB", &b, &l, &ct);        /* Mount-Injection */
        uart_puts("    Traversal blockiert: ../=");
        uart_puts(esc1 < 0 ? "ja" : "NEIN(!)");
        uart_puts(" %2f=");
        uart_puts(esc2 < 0 ? "ja" : "NEIN(!)");
        uart_puts(" mount-inject=");
        uart_puts(esc3 < 0 ? "ja\n" : "NEIN(!)\n");
        uart_puts("    /GIBTESNICHT.TXT -> ");
        uart_puts(httpd_fs_resolve("/GIBTESNICHT.TXT", &b, &l, &ct) < 0
                      ? "nicht gefunden (korrekt)\n" : "GEFUNDEN(?!)\n");
        /* Legitimer Dateiname mit zwei Punkten (LFN) darf NICHT als Traversal gelten. */
        uart_puts("    /release..notes.txt -> ");
        if (httpd_fs_resolve("/release..notes.txt", &b, &l, &ct) == 0) {
            uart_puts("gefunden (korrekt): ");
            for (uint16_t i = 0; i < l && i < 60; i++) { uart_putc((char)b[i]); }
        } else {
            uart_puts("FAELSCHLICH abgewiesen(!)\n");
        }
    }
#endif /* RTOS_SELFTEST (HTTP-VFS-Resolver) */

#ifdef INTERACTIVE_LOGIN
    uint32_t login_uid = 0, login_caps = 0;
    int logged_in = 0;
    if (storage_ok) {
#ifdef AUTO_LOGIN
        /* AUTOMATISCHER Admin-Login OHNE Prompt. Nur so kommen
         * der Scheduler und damit das Dev-Interface OHNE manuellen Konsolen-Login hoch -> autonome
         * Fernsteuerung (auch nach einem fern-ausgeloesten Neustart). AUSSCHLIESSLICH fuer Dev-
         * Images gedacht (steht mit DEV_REMOTE unter bewusster Sicherheitsreduktion), NIE im RC-/
         * Release-Produktions-Image. must_change wird bewusst ignoriert (kein Prompt moeglich). */
        (void)login_console;
        if (user_login("admin", "admin", &login_uid, &login_caps) == 0) {
            logged_in = 1;
            /* Dev-Image: der Auto-Login-Shell die GUI-Cap geben, damit `run hdd1:VKCUBE.ELF` /
             * `VKTEST.ELF` (und jede GUI-App) den Framebuffer bespielen koennen -- die Kinder erben
             * die Cap ueber SYS_SPAWN. Ausschliesslich #ifdef AUTO_LOGIN (Dev-Image, bewusste
             * Sicherheitsreduktion, nie im RC-/Release-Produktionsimage). */
            login_caps |= USER_CAP_GUI;
            uart_puts("RC-READY: rpi_rtos gebootet, Auto-Login (Dev-Image)\n");
            uart_puts("[5d] Auto-Login (Dev-Image, admin+GUI-Cap) -- KEIN interaktiver Login\n");
        } else {
            uart_puts("[5d] Auto-Login FEHLGESCHLAGEN (Benutzer-DB?)\n");
        }
#else
        /* Einzeiliger Bereitschafts-Marker fuer den HW-/QEMU-Serial-Poller (T1.17): alle
         * Fail-closed-Gates (Krypto-KAT + Benutzer-DB) bestanden, System bootet bis zum
         * interaktiven Login. Zentrale Marker-Taxonomie: tools/rc_markers.ps1. */
        uart_puts("RC-READY: rpi_rtos gebootet, Login bereit\n");
        uart_puts("[5d] Interaktiver Login (Serial-Konsole):\n");
        logged_in = (login_console(&login_uid, &login_caps) == 0);
#endif
    }
#endif

    uart_puts("[6] Scheduler initialisieren...\n");
    sched_init();

#if defined(RTOS_SELFTEST) && defined(GUI_FP)
    /* Guardian (white-box): fpctx-Zero-Init bei Slot-Vergabe -- HIER, direkt nach
     * sched_init und VOR allen weiteren task_creates (single-threaded, kein Allokator-Race).
     * Die EL0-Reuse-Probe (dritte FPTEST-Instanz) deckt den Pfad zusaetzlich real ab, ist
     * aber nicht slot-deterministisch; DIESER Test toetet die Mutation "Zero-Init entfernt". */
    uart_puts("    [fpctx] zero-init bei Slot-Vergabe (white-box, vergifteter Slot): ");
    uart_puts(sched_fpctx_zeroinit_selftest() ? "ok\n" : "FEHLER\n");
#endif

#if defined(RTOS_SELFTEST) && !defined(INTERACTIVE_LOGIN)
    /* IPC-Timeout-Selbsttest. Holder (Prio 2) haelt s_ipct_mtx, Testtask (Prio 3) laeuft
     * auf den Mutex und auf ein leeres Semaphor auf -> beweist, dass die Timeouts feuern.
     * Vor sched_start angelegt; laeuft nach dem Scheduler-Start auf Kern 0. */
    sem_init(&s_ipct_hold, 0);
    mutex_init(&s_ipct_mtx);
    task_create(ipc_mtx_holder, 0, 2, "ipchold");
    task_create(ipc_timeout_test, 0, 3, "ipctest");

    /* Cross-Core-PI-Boost-Guardian. Holder auf Kern 1 (Prio 3), Booster auf Kern 0 (Prio 2). */
    sem_init(&s_pi_ready, 0);
    sem_init(&s_pi_done, 0);
    mutex_init(&s_pi_mtx);
    task_create_on(1, pi_holder, 0, 3, "pihold");
    task_create_on(0, pi_booster, 0, 2, "piboost");

    /* Mailbox-Serialisierungs-Guardian. Zwei Hammer (Kern 1 + Kern 2) feuern gleichzeitig
     * Firmware-Revision-Reads; der Checker (Kern 0) wertet die Belegung/Verletzung aus. */
    sem_init(&s_mbox_done, 0);
    task_create_on(1, mbox_hammer, 0, 4, "mbxh1");
    task_create_on(2, mbox_hammer, 0, 4, "mbxh2");
    task_create(mbox_checker, 0, 3, "mbxchk");

    /* secbuf-Serialisierungs-Guardian. Fenster weiten (nur fuer diesen Test), dann
     * vfs_list (Kern 1) gegen stillen vfs_listdir-Hammer (Kern 2) laufen lassen; der Checker
     * (Kern 0) wertet aus und schliesst das Fenster wieder. */
    sem_init(&s_secbuf_done, 0);
    fat32_secbuf_widen = 1;
    task_create_on(1, secbuf_list_task, 0, 4, "vfsl1");
    task_create_on(2, secbuf_ldir_task, 0, 4, "vfsd2");
    task_create(secbuf_checker, 0, 3, "vfschk");
#endif

#ifdef PANIC_SELFTEST
    task_create(panic_test_task, 0, 3, "paniktest");   /* loest spaeter einen EL1-Fault aus -> panic() */
#endif
#ifdef RMUTEX_PANIC_TEST
    task_create(rmutex_panic_task, 0, 3, "rmutextest"); /* loest ein rekursives mutex_lock aus -> panic() */
#endif

    /* SMP-Scheduler-Demo: je einen Kernel-Worker mit Affinitaet zu Kern 1..3 anlegen
     * (die Sekundaerkerne fuehren sie nach der Freigabe aus). */
    smp_sched_demo_create();

#ifdef INTERACTIVE_LOGIN
#ifdef GUI_SESSION
    uart_puts("[7] GUI-Sitzung mit den Rechten des angemeldeten Benutzers starten:\n");
    if (storage_ok && logged_in) {
        uart_set_mirror(0);                 /* fbcon-Handoff: ab jetzt gehoert der Schirm der GUI */
        uart_puts("    [gui] fbcon-Mirror aus (GUI uebernimmt den Framebuffer)\n");
        gui_input_enable_kbd();             /* Tastatur (Serial/USB) -> GUI-Event-Queue */
        if (proc_exec_as("hdd0:GUI.ELF", login_uid, login_caps | USER_CAP_GUI) < 0) {
            uart_set_mirror(fbcon_putc);    /* GUI-Start fehlgeschlagen -> Schirm zurueckholen (nicht schwarz lassen) */
            uart_puts("    [gui] GUI.ELF-Start fehlgeschlagen -> fbcon-Mirror wieder an\n");
        }
    } else {
        uart_puts("    (kein erfolgreicher Login -> keine GUI)\n");
    }
#else
    uart_puts("[7] Shell mit den Rechten des angemeldeten Benutzers starten:\n");
    if (storage_ok && logged_in) {
        proc_exec_as("hdd0:SHELL.ELF", login_uid, login_caps);
    } else {
        uart_puts("    (kein erfolgreicher Login -> keine Shell)\n");
    }
#endif
#else
    uart_puts("[7] Login + Least Privilege: INIT.ELF als alice (non-admin) und admin:\n");
    if (storage_ok) {
        uint32_t uid, caps;
#if !defined(VK_TEST) && !defined(VISION)
        /* Im -Vk-/-Vision-Build entfallen die INIT.ELF-Demos: das MAX_USER_PROCS-Budget (4)
         * gehoert den Phase-3- bzw. Vision-Prozessen. */
        if (user_login("alice", "geheim123", &uid, &caps) == 0) {
            uart_puts("    Login alice ok -> Prozess (uid=");
            uart_putdec(uid);
            uart_puts(", caps ohne Admin)\n");
            proc_exec_as("hdd0:INIT.ELF", uid, caps);
        }
#endif
        if (user_login("admin", "admin", &uid, &caps) == 0) {
            uart_puts("    Login admin ok -> Prozess (uid=");
            uart_putdec(uid);
            uart_puts(", caps mit Admin)\n");
#if !defined(VK_TEST) && !defined(VISION)
            proc_exec_as("hdd0:INIT.ELF", uid, caps);
#endif
#if defined(VK_TEST)
            /* FP-Kontext-Guardian -- ZWEI FPTEST.ELF auf DEMSELBEN Kern (1).
             * Beide haemmern disjunkte V-Register-/FPCR-/FPSR-Muster ueber Schlaf-Praeemptionen;
             * ohne fpctx_save/restore saehe jeder die Muster des anderen (Leak + Korruption).
             * Least Privilege (Review T3.x): FPTEST braucht KEINE Caps (caps=0), VKTEST nur
             * die GUI-Cap -- nicht die Admin-Caps des Logins. Startfehler werden LAUT gemeldet
             * (der Verify wertet '[vk] FEHLER' als rot). */
            uart_puts("    [vk] T3.1 FP-Kontext-Guardian: 2x FPTEST.ELF auf Kern 1\n");
            if (proc_exec_as_on(1, "hdd0:FPTEST.ELF", uid, 0, 0, 0) < 0 ||
                proc_exec_as_on(1, "hdd0:FPTEST.ELF", uid, 0, 0, 0) < 0) {
                uart_puts("    [vk] FEHLER: FPTEST.ELF-Start fehlgeschlagen\n");
            }
            /* r3d-/Vulkan-Selbsttests + 3D-Demo (zeichnet ueber die GUI-Bruecke). */
            uart_puts("    [vk] T3.2 Rasterizer-Selbsttest + Demo: VKTEST.ELF (GUI-Cap)\n");
            if (proc_exec_as("hdd0:VKTEST.ELF", uid, USER_CAP_GUI) < 0) {
                uart_puts("    [vk] FEHLER: VKTEST.ELF-Start fehlgeschlagen\n");
            } else {
                /* fbcon-Handoff (wie GUI_APP): der Schirm gehoert ab jetzt der 3D-Ausgabe;
                 * alle Selbsttest-Marker laufen weiter ueber Serial (Asserts unveraendert). */
                uart_puts("    [vk] fbcon-Mirror aus (3D uebernimmt den Schirm)\n");
                uart_set_mirror(0);
            }
#elif defined(GUI_APP)
            /* die echte GUI-Sitzung (GUI.ELF) statt des Bruecke-Tests -- Demo-Form + wf_run.
             * fbcon-Handoff, damit die GUI den Schirm besitzt; Tastatur -> GUI-Event-Queue. */
            uart_set_mirror(0);
            uart_puts("    [gui] fbcon-Mirror aus (GUI uebernimmt den Framebuffer)\n");
            gui_input_enable_kbd();
            if (proc_exec_as("hdd0:GUI.ELF", uid, caps | USER_CAP_GUI) < 0) {
                uart_set_mirror(fbcon_putc);   /* GUI-Start fehlgeschlagen -> Schirm zurueckholen */
                uart_puts("    [gui] GUI.ELF-Start fehlgeschlagen -> fbcon-Mirror wieder an\n");
            }
#elif defined(VKGUI_APP)
            /* Vulkan-in-WinForms-Demo (VKGUI.ELF): eine GUI-App mit eingebettetem, live gerendertem
             * Vulkan-Wuerfel-Viewport. Wie GUI_APP: fbcon-Handoff (die GUI besitzt den Schirm) +
             * Tastatur/Maus an die GUI-Event-Queue. USER_CAP_GUI fuers Framebuffer-Fenster. */
            uart_set_mirror(0);
            uart_puts("    [vkgui] fbcon-Mirror aus (Vulkan-GUI uebernimmt den Framebuffer)\n");
            gui_input_enable_kbd();
            if (proc_exec_as("hdd0:VKGUI.ELF", uid, caps | USER_CAP_GUI) < 0) {
                uart_set_mirror(fbcon_putc);
                uart_puts("    [vkgui] VKGUI.ELF-Start fehlgeschlagen -> fbcon-Mirror wieder an\n");
            }
#elif defined(VISION)
            /* Vision-Track (docs/architecture/19): KI-Bildauswertung-Selbsttest (AIVISION.ELF).
             * Der EINZIGE Kernel-Kontakt des gekapselten Moduls -- praeprozessor-gegated, ohne
             * -DVISION byte-inert. USER_CAP_GUI fuer den spaeteren Framebuffer-Overlay (A2); A1.1
             * rechnet reine NEON-fp32-sgemm und braucht ihn noch nicht (Least Privilege spaeter). */
            uvc_selftest();   /* A4.1a: UVC-Klassen-Layer (synthetisch, QEMU-verifizierbar) */
            uart_puts("    [vision] AIVISION.ELF: KI-Bildauswertung-Selbsttest\n");
            if (proc_exec_as("hdd0:AIVISION.ELF", uid, USER_CAP_GUI) < 0) {
                uart_puts("    [vision] FEHLER: AIVISION.ELF-Start fehlgeschlagen\n");
            }
#else
            /* EL0-GUI-Bruecke-Test -- zeichnet via SYS_GUI_INFO/SYS_GUI_FLUSH in den
             * Backbuffer und beweist den EL0-Zugriff. Braucht USER_CAP_GUI (sonst kein FB-Fenster). */
            proc_exec_as("hdd1:GUITEST.ELF", uid, caps | USER_CAP_GUI);
#endif
        }
        /* SMP: Der EL0-Prozess auf Kern 1 wird NICHT mehr hier (vor dem Scheduler-Start)
         * angelegt, sondern zur LAUFZEIT vom Kernel-Task smp_runtime_spawner (smp.c) --
         * sobald der Sekundaer-Scheduler von Kern 1 laeuft. Das prueft die Publish-Ordering
         * (TASK_SETUP -> ttbr/cred -> task_admit + Reschedule-IPI) auf einem live Kern. */
        uart_puts("    EL0 auf Sekundaerkern: Laufzeit-Spawn auf Kern 1 (siehe [smp-spawn])\n");
    } else {
        uart_puts("    uebersprungen (kein Storage).\n");
    }
#endif

#ifdef V3D_PROBE
    v3d_probe();   /* Vulkan V5: V3D-Hardware-Erkennung (erster Bring-up-Schritt) */
#endif
#ifdef DEV_REMOTE
    dev_remote_selftest();   /* Dev-Remote D1: Protokoll-Kern-Selbsttest (synthetisch) */
    dev_agent_start();       /* Dev-Remote D2: Dispatch-Selbsttest (QEMU) + Live-Netz-Agent (nur Pi4) */
#endif
#ifdef DIAG_BLINK
    diag_latch(16);        /* M8: Scheduler-Start erreicht (Pin 36) */
    task_create(diag_heartbeat_task, 0, 7, "diagbeat");  /* danach GPIO21/Pin40 toggelt = Scheduler laeuft */
#endif
#ifdef DIAG_LOG
    diag_log_to_sd();      /* Schnitt kurz vor dem Scheduler ([1]-[8]) */
    task_create(diag_log_task, 0, 7, "diaglog");   /* danach periodische Nachschuesse (Laufzeit) */
#endif
#ifdef PCIE_PROBE
    /* Erst den Abort-Fixup beweisen (laeuft AUCH in QEMU: 384-GiB-VA ist ueberall unmapped) -> der
     * HW-kritische Toleranz-Pfad ist verifiziert, bevor echtes PCIe angefasst wird. */
    pcie_fixup_selftest();
    /* Der eigentliche PCIe/VL805-Zugriff nur auf echter HW: QEMU raspi4b emuliert die PCIe-RC nicht
     * -> Registerzugriff = External-Abort (jetzt vom Fixup abgefangen, aber trotzdem HW-only sinnvoll). */
    if (mmu_ram_from_dtb()) {
        pcie_probe();   /* BCM2711 PCIe/VL805-Zustandsdiagnose (USB-A-Bringup, Schritt 1; boot-sicher) */
        /* On-demand VL805-xHCI-MMIO-Probe (Trigger hdd1:XHCIGO.FLG, Watchdog-gesichert) -- NIE beim
         * Boot, damit ein evtl. Haenger das Netzwerk nicht killt. Nur HW (in QEMU kein VL805). */
        task_create(pcie_xhci_task, 0, 6, "xhciprobe");
    }
#endif
    uart_puts("[8] Scheduler starten (alle Kerne):\n");
    smp_sched_release();   /* Sekundaerkerne in ihren Scheduler freigeben */
    sched_start();         /* Kern 0; kehrt nie zurueck */

    for (;;) {
        wfe();
    }
}
