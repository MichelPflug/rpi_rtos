/*
 * kernel/smp.c  --  SMP-Bring-up + paralleler Lasttest (Spinlock-Verifikation)
 *
 * QEMU raspi4b haelt die Sekundaerkerne (1..3) im Raspberry-Pi-SPIN-TABLE: jeder
 * Kern spinnt (wfe) und liest seine Release-Adresse 0xd8 + 8*core; ist sie != 0,
 * springt er dorthin. smp_init_secondaries() (Core 0, NACH mmu_init) schreibt die
 * Adresse von _secondary_entry (start.S) in 0xE0/0xE8/0xF0 und weckt die Kerne.
 *
 * Jeder Sekundaerkern: EL-Drop nach EL1 (start.S) -> mmu_init_secondary() (gleiche
 * inner-shareable Page-Tables -> kohaerent, Exclusives nutzbar) -> Lasttest: SMP_ITERS
 * spinlock-geschuetzte Inkremente eines GETEILTEN Zaehlers. Stimmt am Ende
 * g_shared_counter == 3*SMP_ITERS, hat der Spinlock alle Updates serialisiert (kein
 * Lost-Update) -> echte parallele Ausfuehrung mit korrektem Ausschluss bewiesen.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "mmu.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"
#include "ipc.h"
#include "spinlock.h"
#include "proc.h"
#include "smp.h"

#define SMP_CORES        4
#define SMP_ITERS        20000     /* je Kern; klein genug fuers QEMU-Verify-Zeitfenster */
#define SPINTABLE_BASE   0xd8UL     /* Release-Adresse Kern c = 0xd8 + 8*c */

extern char _secondary_entry[];     /* Spin-Table-Einsprung (start.S) */

static volatile uint32_t g_core_el[SMP_CORES];     /* != 0: Kern online (Wert = EL) */
static volatile uint64_t g_local_count[SMP_CORES]; /* Inkremente je Kern (ungelockt) */
static volatile uint32_t g_core_done[SMP_CORES];   /* Worker fertig */
static volatile uint64_t g_shared_counter;         /* durch g_count_lock geschuetzt */
static spinlock_t        g_count_lock = SPINLOCK_INIT;

/* SMP-Scheduler-Demo (Stufe 2): Freigabe der Sekundaerkerne in den Scheduler +
 * Nachweis, dass je ein Kernel-Task auf seinem eigenen Kern lief. */
static volatile int      g_smp_sched_go;           /* Core 0 -> Sekundaerkerne in sched */
static volatile uint32_t g_wk_cpu[SMP_CORES];      /* auf welchem Kern lief Worker N */
static volatile uint32_t g_wk_rounds[SMP_CORES];
static volatile uint32_t g_wk_done[SMP_CORES];

/* Cross-core IPC-Demo: ein Warter-Task (Kern 1) blockiert auf einem Semaphor, ein
 * Poster-Task (Kern 0) postet -> der Warter wird auf seinem Kern geweckt. */
static semaphore_t       g_demo_sem;
static volatile uint32_t g_ipc_waiter_ready;       /* Warter hat sem_wait erreicht (sem=0 -> blockiert) */
static volatile uint32_t g_ipc_waiter_cpu;         /* Kern, auf dem der Warter geweckt wurde */
static volatile uint32_t g_ipc_done;

/* Laufzeit-Spawn-Demo: ein Kernel-Task auf Kern 0 startet -- NACHDEM der Sekundaer-Scheduler
 * von Kern 1 nachweislich laeuft -- einen EL0-Prozess auf Kern 1 (echter Runtime-Spawn). */
static volatile int      g_spawn_tid = -2;         /* -2: noch nicht; >=0 tid; -1 Fehler */
static volatile uint32_t g_spawn_done;

static void smp_worker(uint64_t core_id)
{
    uint64_t local = 0;
    for (int i = 0; i < SMP_ITERS; i++) {
        spin_lock(&g_count_lock);
        g_shared_counter++;
        spin_unlock(&g_count_lock);
        local++;
    }
    if (core_id < SMP_CORES) {
        g_local_count[core_id] = local;
    }
}

void secondary_main(uint64_t core_id)
{
    mmu_init_secondary();           /* MMU+Caches an -> kohaerent + Exclusives nutzbar */
    exceptions_init();              /* VBAR_EL1 dieses Kerns (gemeinsame Vektortabelle) */
    gic_init_cpu();                 /* eigenes GICC + banked SGI/PPI-Gruppe */
    timer_init_secondary();         /* eigener CNTP + PPI 30 (GICD ist von Core 0 an) */
    if (core_id < SMP_CORES) {
        g_core_el[core_id] = (uint32_t)current_el();
    }
    irq_enable();                   /* ab jetzt eigene Timer-IRQs annehmen */

    smp_worker(core_id);            /* Spinlock-Lasttest (Ausschluss-Beweis) */
    if (core_id < SMP_CORES) {
        dsb_sy();
        g_core_done[core_id] = 1;
        dsb_sy();
    }

    /* Warten, bis Core 0 den Scheduler + die Tasks bereitgestellt hat, dann in den
     * Scheduler dieses Kerns eintreten (laeuft fortan dessen Tasks + Idle). */
    while (!g_smp_sched_go) {
        wfe();
    }
    dmb_sy();                       /* Acquire: die von Core 0 angelegten Task-TCBs sehen,
                                     * bevor sched_start_secondary tasks[] liest */
    sched_start_secondary();        /* kehrt nie zurueck */
    for (;;) {
        wfi();                      /* unerreichbar */
    }
}

void smp_init_secondaries(void)
{
    uint64_t entry = (uint64_t)(uintptr_t)_secondary_entry;

    for (int c = 1; c < SMP_CORES; c++) {
        uint64_t slot = SPINTABLE_BASE + 8UL * (uint64_t)c;
        *(volatile uint64_t *)slot = entry;
        /* Auf echter HW lesen die Sekundaerkerne ihre Release-Adresse MMU-AUS (non-
         * cacheable von der PoC), waehrend Core 0 hier MIT Caches schreibt (Normal-WB).
         * dsb allein raeumt keine dirty Cache-Line zur PoC -> den Slot explizit dorthin
         * clean'en, sonst liest der Kern eine stale 0 und startet nie. (In QEMU kohaerent
         * und daher unsichtbar; analog zur dc-civac-Wartung der Page-Tables in mmu_init.) */
        __asm__ volatile("dc cvac, %0" :: "r"(slot) : "memory");
    }
    dsb_sy();
    sev();                          /* die in wfe wartenden Kerne wecken */

    /* Auf Fertigmeldung aller drei Worker warten (bounded; in QEMU schnell). */
    for (volatile uint64_t spin = 0; spin < 2000000000ull; spin++) {
        if (g_core_done[1] && g_core_done[2] && g_core_done[3]) {
            break;
        }
    }
    /* Acquire-Barriere vor dem Lesen der Worker-Ergebnisse: die Schreiber haben
     * g_shared_counter/g_local_count VOR g_core_done veroeffentlicht (dsb dort), aber
     * eine Control-Dependency auf den done-Load ordnet auf AArch64 nur load->store,
     * nicht load->load -> ohne diese Barriere koennte Core 0 stale Ergebnisse lesen. */
    dmb_sy();

    int online = 0;
    for (int c = 1; c < SMP_CORES; c++) {
        if (g_core_el[c]) { online++; }
    }
    uint64_t expected = (uint64_t)SMP_ITERS * 3ull;

    uart_puts("    [smp] ");
    uart_putdec((uint32_t)online);
    uart_puts(" Kerne online (EL");
    uart_putdec(g_core_el[1]);
    uart_puts("); Spinlock-Zaehler=");
    uart_putdec((uint32_t)g_shared_counter);
    uart_puts(" erwartet=");
    uart_putdec((uint32_t)expected);
    uart_puts(g_shared_counter == expected ? " => OK\n" : " => FALSCH\n");
    uart_puts("    [smp] pro Kern: Kern1=");
    uart_putdec((uint32_t)g_local_count[1]);
    uart_puts(" Kern2=");
    uart_putdec((uint32_t)g_local_count[2]);
    uart_puts(" Kern3=");
    uart_putdec((uint32_t)g_local_count[3]);
    uart_puts("\n");

    /* Per-Core-Timer-IRQ-Probe: warten, bis jeder Sekundaerkern eigene Timer-IRQs
     * gezaehlt hat (beweist, dass QEMU/HW die per-core PPI 30 an die Kerne zustellt). */
    for (volatile uint64_t spin = 0; spin < 2000000000ull; spin++) {
        if (timer_core_ticks(1) && timer_core_ticks(2) && timer_core_ticks(3)) {
            break;
        }
    }
    int ticking = (timer_core_ticks(1) && timer_core_ticks(2) && timer_core_ticks(3));
    uart_puts("    [smp] Per-Core-Timer-IRQ: Kern1=");
    uart_putdec((uint32_t)timer_core_ticks(1));
    uart_puts(" Kern2=");
    uart_putdec((uint32_t)timer_core_ticks(2));
    uart_puts(" Kern3=");
    uart_putdec((uint32_t)timer_core_ticks(3));
    uart_puts(ticking ? " -> alle ticken: ja\n" : " -> alle ticken: nein\n");
}

/* --- SMP-Scheduler-Demo (Stufe 2) --- */

/* Kernel-Worker mit Affinitaet zu Kern arg (1..3): laeuft als eingeplanter Task auf
 * seinem Kern, schlaeft zwischen den Runden (Kern wechselt zum Idle, Timer weckt). */
static void smp_sched_worker(void *arg)
{
    uint64_t id = (uint64_t)arg;
    for (int r = 0; r < 5; r++) {
        if (id < SMP_CORES) {
            g_wk_cpu[id]    = cpu_id();          /* tatsaechlicher Kern dieses Tasks */
            g_wk_rounds[id] = (uint32_t)(r + 1);
        }
        task_sleep_ticks(2);                      /* Scheduler: schlafen -> Idle -> Timer weckt */
    }
    if (id < SMP_CORES) {
        g_wk_done[id] = 1;
    }
    /* return -> task_trampoline -> task_exit (Kern schaltet auf seinen Idle) */
}

/* Cross-core IPC-Demo: blockiert auf Kern 1 auf dem Semaphor, bis der Poster auf Kern 0
 * postet -> beweist Wecken ueber Kern-Grenzen (sched_wake_one + IPC-Lock). */
static void ipc_waiter(void *arg)
{
    (void)arg;
    g_ipc_waiter_ready = 1;                        /* signalisiert: erreiche sem_wait (sem=0) */
    dsb_sy();
    sem_wait(&g_demo_sem);                         /* blockiert bis sem_post (von Kern 0) */
    g_ipc_waiter_cpu = cpu_id();                   /* hier auf Kern 1 wieder eingeplant */
    dsb_sy();
    g_ipc_done = 1;
}

static void ipc_poster(void *arg)
{
    (void)arg;
    /* Warten, bis der Warter sem_wait erreicht hat (sem=0 -> er blockiert garantiert), dann
     * ein wenig Sicherheitsmarge -> der Post weckt sicher einen WIRKLICH blockierten Warter. */
    while (!g_ipc_waiter_ready) {
        task_sleep_ticks(1);
    }
    task_sleep_ticks(2);
    sem_post(&g_demo_sem);                          /* weckt den Warter auf dem anderen Kern */
}

/* Laufzeit-Spawn-Task (Kern 0): wartet, bis der Kern-1-Worker mindestens eine Runde gemacht
 * hat (-> der Sekundaer-Scheduler von Kern 1 laeuft NACHWEISLICH), und startet dann einen
 * EL0-Prozess mit Affinitaet zu Kern 1. Das ist ein echter Runtime-Spawn auf einen bereits
 * live schedulenden Sekundaerkern -- prueft die Publish-Ordering (TASK_SETUP -> ttbr/cred ->
 * task_admit) + das Wecken des Ziel-Kerns per Reschedule-IPI. */
static void smp_runtime_spawner(void *arg)
{
    (void)arg;
    while (!g_wk_rounds[1]) {
        task_sleep_ticks(1);
    }
    int tid = proc_exec_as_on(1, "hdd0:INIT.ELF", 1, 0, 0, 0);   /* EL0 zur Laufzeit auf Kern 1 */
    g_spawn_tid = tid;
    dsb_sy();
    g_spawn_done = 1;
}

/* Reporter-Task (Kern 0): wartet eingeplant auf die drei Worker + die IPC-Demo und meldet
 * (Zeilen atomar, IRQs maskiert), dass je ein Kernel-Task auf seinem Kern lief und der
 * Semaphor-Warter cross-core geweckt wurde. */
static void smp_sched_reporter(void *arg)
{
    (void)arg;
    while (!(g_wk_done[1] && g_wk_done[2] && g_wk_done[3] && g_ipc_done && g_spawn_done)) {
        task_sleep_ticks(2);
    }
    int ok  = (g_wk_cpu[1] == 1 && g_wk_cpu[2] == 2 && g_wk_cpu[3] == 3);
    int ipc = (g_ipc_waiter_cpu == 1);
    int spawn = (g_spawn_tid >= 0);

    uart_begin();                                 /* alle Zeilen cross-core atomar (kein Verschraenken) */
    uart_puts("    [smp-sched] Kernel-Tasks: Kern1->CPU");
    uart_putdec(g_wk_cpu[1]);
    uart_puts(" Kern2->CPU");
    uart_putdec(g_wk_cpu[2]);
    uart_puts(" Kern3->CPU");
    uart_putdec(g_wk_cpu[3]);
    uart_puts(ok ? " -> je Task auf eigenem Kern: ja\n"
                 : " -> je Task auf eigenem Kern: nein\n");
    uart_puts("    [smp-ipc] Semaphor-Warter (Kern 1) von Kern 0 geweckt, CPU=");
    uart_putdec(g_ipc_waiter_cpu);
    uart_puts(ipc ? " -> cross-core IPC: ok\n" : " -> cross-core IPC: FALSCH\n");
    uint32_t ipiN = sched_ipi_count(1);
    uart_puts("    [smp-ipc] Reschedule-IPI auf Kern 1 empfangen: ");
    uart_putdec(ipiN);
    uart_puts(ipiN > 0 ? " -> IPI ok\n" : " -> kein IPI\n");
    /* tid= ist bewusst der tasks[]-SLOT (interne Diagnose, korreliert mit dem "... slot N" der
     * [proc]-Zeile), NICHT die monotone PID -- Programmlogik haengt nicht davon ab. */
    uart_puts("    [smp-spawn] Laufzeit-Spawn -> Kern 1 (Sekundaer-Scheduler live): tid=");
    uart_putdec((uint32_t)g_spawn_tid);
    uart_puts(spawn ? " -> ok\n" : " -> FEHLER\n");
    uart_end();
}

void smp_sched_demo_create(void)
{
    /* Je ein Worker fuer Kern 1..3 (Affinitaet) + ein Reporter auf Kern 0. */
    task_create_on(1, smp_sched_worker, (void *)1, 5, "wrk1");
    task_create_on(2, smp_sched_worker, (void *)2, 5, "wrk2");
    task_create_on(3, smp_sched_worker, (void *)3, 5, "wrk3");
    /* Cross-core IPC-Demo: Warter auf Kern 1, Poster auf Kern 0. */
    sem_init(&g_demo_sem, 0);
    task_create_on(1, ipc_waiter, (void *)0, 6, "ipcw");
    task_create_on(0, ipc_poster, (void *)0, 6, "ipcp");
    /* Laufzeit-Spawn eines EL0-Prozesses auf Kern 1 (nach dem Scheduler-Start). */
    task_create_on(0, smp_runtime_spawner, (void *)0, 6, "spawn");
    task_create_on(0, smp_sched_reporter, (void *)0, 4, "smprep");
}

void smp_sched_release(void)
{
    dsb_sy();                                     /* Release: die angelegten Task-TCBs
                                                   * sichtbar machen, BEVOR das Flag steht */
    g_smp_sched_go = 1;
    dsb_sy();
    sev();                                        /* wartende Sekundaerkerne wecken */
}
