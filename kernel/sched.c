/*
 * kernel/sched.c  --  Preemptiver Festprioritaeten-Scheduler (SMP, partitioniert)
 *
 * - Statischer Task-Pool + statische Kernel-Stacks (noch kein Heap).
 * - SMP-Modell: PARTITIONIERT. Jeder Task hat eine feste Kern-Affinitaet (owner);
 *   jeder Kern schedulet AUSSCHLIESSLICH seine eigenen Tasks (per-core current[]).
 *   Dadurch laeuft ein Task nie auf zwei Kernen -> context_switch/ctx_sp eines Tasks
 *   wird nur vom owner beruehrt -> kein Migrations-/Lock-Handoff-Race. Der Spinlock
 *   schuetzt nur die Slot-Allokation in task_create (geteiltes tasks[]).
 * - Pro Kern ein Idle-Task (garantiertes Wechselziel). EL0-/User-Tasks duerfen dank
 *   per-Prozess-TTBR0/ASID (mmu_create_aspace) auf JEDEM Kern laufen; der partitionierte
 *   Scheduler haelt jeden Task an seinem owner-Kern fest (keine Migration).
 * - Preemption ueber den per-core Timer-Tick: sched_tick() setzt need_resched[cid],
 *   der IRQ-Handler ruft nach dem EOI schedule_if_needed().
 *
 * Invariante: schedule()/context_switch() laufen IMMER mit maskierten IRQs.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "timer.h"
#include "mmu.h"
#include "gic.h"
#include "spinlock.h"
#include "sched.h"

#define NCORES       4
#define MAX_TASKS    28            /* Task-Pool (Slots); 28 -> Kopf fuer Selbsttest-Tasks Mailbox- + secbuf-Guardian-Tasks) + Laufzeit-Spawns
                                    * ohne Pool-Enge (frueher knapp bei 16, dann 24 an der Grenze) */
#define KSTACK_SIZE  8192          /* pro Task */
#define TIME_SLICE   10            /* Ticks pro Quantum (100 Hz -> 100 ms) */
#define CTX_FRAME    12            /* x19..x30 = 12 Register */
#define STACK_CANARY 0x5354414B47554152ULL   /* "STAKGUAR" - Stack-Overflow-Wache */

typedef enum {
    TASK_UNUSED = 0,
    TASK_SETUP,          /* Slot belegt, aber NOCH NICHT schedulebar (ttbr/cred werden gesetzt) */
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,        /* wartet auf ein IPC-Objekt (wait_obj) */
    TASK_ZOMBIE,         /* beendet, aber Slot noch nicht freigegeben (letzter ctx_sp-Write
                          * im context_switch steht noch aus) -> spaeter eingesammelt */
} task_state_t;

/* volatile-qualifiziert sind die Felder, die ein Task-OWNER-Kern LOCK-FREI liest, waehrend ein
 * FREMDKERN sie unter s_ipclock schreibt (state/priority in pick_next; wake_tick in sched_tick;
 * kill_pending in sched_exit_if_killed). Das ist HYGIENE (verhindert Compiler-Elision/Hoisting
 * der Cross-Core-Reads), NICHT der Ordnungs-Mechanismus: die Inter-Core-Ordnung liefert der
 * dmb_sy-Release + need_resched[owner]+SGI-Handshake (Acquire in schedule_if_needed) bzw. s_ipclock.
 * Load-bearing-Invariante: JEDER Cross-Core-state-Writer schreibt AUSSCHLIESSLICH ->READY (alle
 * nicht-lauffaehigen Uebergaenge macht der owner selbst) -> ein frueh/spaet beobachtetes READY ist
 * stets sicher; nur deshalb genuegt auf der Leseseite die Barriere des Handshakes. */
typedef struct {
    uint64_t              ctx_sp;   /* gesicherter SP fuer context_switch (nur owner) */
    volatile task_state_t state;
    volatile uint8_t      priority;
    uint8_t               used;
    uint8_t               owner;    /* Kern-Affinitaet (0..NCORES-1) */
    uint32_t              slice;    /* verbleibende Ticks im Quantum (nur owner) */
    volatile uint64_t     wake_tick;/* Weck-Tick bei TASK_SLEEPING */
    uint64_t     user_phys;       /* phys. User-Bereich (0 = Kernel-Task) */
    uint64_t     ttbr;            /* TTBR0 dieses Tasks (Kernel-Aspace bzw. Prozess-Aspace) */
    void        *wait_obj;        /* IPC-Objekt, auf das gewartet wird (TASK_BLOCKED) */
    uint32_t     uid;             /* Credential: Benutzer-ID */
    uint32_t     caps;            /* Credential: Capabilities */
    uint64_t     pid;             /* monotone Prozess-ID (nie recycled; != Slot-Index) */
    uint64_t     ppid;            /* Eltern-PID (0 = kernel-gestartet/keiner) -- fuer wait/kill */
    volatile uint8_t kill_pending; /* von sched_kill_pid gesetzt -> Selbstbeendigung (TASK_EXIT_KILLED);
                                    * sticky 0->1, lock-frei am Safe-Point gelesen (sched_exit_if_killed) */
    uint8_t      timed_out;       /* 1 = das letzte BLOCKED-Warten endete per Deadline; nur unter s_ipclock */
    const char  *name;
} tcb_t;

static tcb_t   tasks[MAX_TASKS];
/* Monoton steigender PID-Zaehler: eine PID bezeichnet EINDEUTIG eine Task-Instanz. Der Slot-Index
 * (tasks[]) wird bei Task-Exit recycled; die PID nicht -> keine Verwechslung alter/neuer Instanzen.
 * Unter s_tasklock vergeben (task_create_internal). Startet bei 1 (0 = ungueltig/keine).
 * uint64_t: bei jeder erdenklichen Uptime/Spawn-Rate unerschoepflich -> der Zaehler wickelt nie um,
 * also wird 0 nie an einen gueltigen Task vergeben und keine PID je wiederverwendet (Invariante haelt
 * WORTWOERTLICH, nicht nur bis 2^32). */
static uint64_t g_next_pid = 1;
static uint8_t kstacks[MAX_TASKS][KSTACK_SIZE] __attribute__((aligned(16)));

/* Exit-Records: ueberleben die Slot-Freigabe. Wenn ein EL0-Kindprozess (user_phys!=0, ppid!=0)
 * endet UND sein Elternprozess noch lebt, wird (pid,ppid,code) hier abgelegt und der wartende
 * Elternprozess geweckt; sched_wait_pid konsumiert den Record. Unter s_ipclock (derselbe Lock wie
 * sched_block_on/sched_wake_one). task_exit legt NUR ab, wenn der Elternprozess lebt (sonst kann
 * ihn nie jemand ernten) -> keine verwaisten Records. Ist die Tabelle dennoch voll, verdraengt
 * exitrec_put bevorzugt einen verwaisten Record (Elternprozess inzwischen tot). */
static struct { uint64_t pid; uint64_t ppid; int32_t code; uint8_t used; } s_exitrec[MAX_TASKS];

static int            current[NCORES];        /* laufender Task je Kern */
static volatile int   need_resched[NCORES];
static int            sched_started[NCORES];
static int            s_reap[NCORES];          /* zu reklamierender ZOMBIE-Slot je Kern (-1 = keiner) */
static volatile uint32_t s_ipi_count[NCORES];    /* empfangene Reschedule-IPIs je Kern (Diagnose) */
static volatile uint32_t s_pi_remote_resched;    /* Cross-Core-PI-Boost-Reschedules (Diagnose/Guardian) */
static uint64_t       boot_ctx_sp[NCORES];     /* Wegwerf-Kontext fuer den ersten Wechsel */
static spinlock_t     s_tasklock = SPINLOCK_INIT;   /* schuetzt die Slot-Allokation */
static spinlock_t     s_ipclock  = SPINLOCK_INIT;   /* IPC: schuetzt Block/Wake-Uebergaenge
                                                     * + die IPC-Objektzustaende cross-core */

extern void context_switch(uint64_t *prev_sp, uint64_t *next_sp);
extern void task_trampoline(void);

#ifdef GUI_FP
/* Eager-FP/SIMD-Kontext (q0..q31 + FPCR/FPSR = 528 B/Task, arch/aarch64/fpctx.S).
 * Nur in GUI_FP-Builds (dort ist FPEN=0b11, mehrere EL0-Prozesse duerfen FP nutzen);
 * ohne GUI_FP existiert weder der Puffer noch ein Aufruf -> Kernel bleibt FP-frei.
 * Der Bereich eines Tasks wird NUR von dessen owner-Kern beruehrt (partitionierter
 * Scheduler, schedule()/sched_enter() laufen owner-lokal) -> kein Cross-Core-Race.
 * Zero-Init bei Slot-(Wieder-)Vergabe -> jeder Prozess startet mit deterministisch
 * GENULLTEM FP-Zustand (kein V-Register-/FPCR-Leak zwischen Prozessen). */
#define FPCTX_BYTES 528
extern void fpctx_save(void *area);
extern void fpctx_restore(const void *area);
static uint8_t fpctx[MAX_TASKS][FPCTX_BYTES] __attribute__((aligned(16)));
#endif

/* Kernel-interner Idle-Task (einer pro Kern): garantiert immer ein Wechselziel. */
static void idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
        wfi();
    }
}

void sched_init(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].used  = 0;
        tasks[i].state = TASK_UNUSED;
    }
    for (int c = 0; c < NCORES; c++) {
        current[c]       = -1;
        need_resched[c]  = 0;
        sched_started[c] = 0;
        s_reap[c]        = -1;
    }

    /* Ein Idle-Task pro Kern (niedrigste Prioritaet) -> garantierte Invariante. */
    for (uint32_t c = 0; c < NCORES; c++) {
        task_create_on(c, idle_entry, (void *)0, 255, "idle");
    }
}

/* Gemeinsamer Slot-Aufbau. admit=1: sofort schedulebar (TASK_READY) -- fuer Kernel-Tasks,
 * die den Kernel-Adressraum nutzen. admit=0: Slot wird als TASK_SETUP publiziert (belegt,
 * aber von lock-freien Scheduler-Lesern NICHT waehlbar) -> der Aufrufer setzt erst ttbr/
 * cred und ruft dann task_admit(), das den Task atomar freigibt. Das schliesst das
 * Laufzeit-Fenster, in dem ein Sekundaerkern einen User-Task mit noch nicht gesetztem
 * TTBR0 einplanen koennte (Phantom-Aspace). */
static int task_create_internal(uint32_t core, task_entry_t entry, void *arg,
                                uint8_t priority, const char *name, int admit)
{
    if (core >= NCORES) {
        return -1;
    }
    /* IRQs maskiert halten, solange s_tasklock gehalten wird: reap_pending() nimmt s_tasklock
     * jetzt AUCH aus dem Scheduler-/IRQ-Pfad (schedule() -> reap_pending). Ohne Maskierung
     * koennte ein Timer-IRQ auf DEMSELBEN Kern mitten in dieser Sektion schedule()->reap_pending
     * ausloesen und den (nicht-reentranten) s_tasklock erneut nehmen -> Self-Deadlock. */
    uint64_t flags = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_tasklock);
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock(&s_tasklock);
        WRITE_SYSREG(daif, flags);
        return -1;
    }
    tcb_t *t   = &tasks[slot];
    t->pid      = g_next_pid++;    /* monotone PID (unter s_tasklock -> eindeutig, nie recycled) */
    t->ppid         = 0;           /* Default kein Elternprozess; proc.c setzt ihn beim EL0-Spawn */
    t->kill_pending = 0;
    t->timed_out    = 0;
    t->state    = admit ? TASK_READY : TASK_SETUP;
    t->priority = priority;
    t->owner    = (uint8_t)core;
    t->slice    = TIME_SLICE;
    t->wake_tick = 0;
    t->user_phys = 0;
    t->wait_obj  = 0;
    t->uid       = 0;             /* fail-closed bis sched_set_cred/proc_exec_as */
    t->caps      = 0;
    t->ttbr      = mmu_kernel_ttbr();   /* Default: Kernel-Adressraum (User setzt eigenen) */
    t->name     = name;

    /* Stack-Guard am unteren Stack-Ende. */
    *(volatile uint64_t *)&kstacks[slot][0] = STACK_CANARY;

#ifdef GUI_FP
    /* T3.1: FP-Kontext des Slots nullen -- der Task startet mit sauberem (genulltem)
     * FP/SIMD-Zustand, auch bei Slot-Wiederverwendung (kein Leak des Vorgaengers). */
    for (int i = 0; i < FPCTX_BYTES; i++) { fpctx[slot][i] = 0; }
#endif

    uint64_t  top   = (uint64_t)&kstacks[slot][KSTACK_SIZE];
    uint64_t *frame = (uint64_t *)(top - CTX_FRAME * 8);
    for (int i = 0; i < CTX_FRAME; i++) {
        frame[i] = 0;
    }
    frame[0]  = (uint64_t)entry;            /* x19 */
    frame[1]  = (uint64_t)arg;              /* x20 */
    frame[11] = (uint64_t)task_trampoline;  /* x30 */
    t->ctx_sp = (uint64_t)frame;

    /* used=1 als ALLERLETZTES + Release-Barriere: lock-freie Scheduler-Leser auf ANDEREN
     * Kernen (pick_next/sched_tick) duerfen den Slot erst als belegt sehen, wenn owner/
     * state/ctx_sp vollstaendig geschrieben sind -- sonst koennte ein Kern einen halb-
     * initialisierten (oder via SYS_SPAWN zur Laufzeit wiederverwendeten) Slot waehlen
     * und in einen stale ctx_sp springen. (Bei admit=0 ist state=TASK_SETUP -> der Slot
     * gilt zwar als belegt, ist aber bis task_admit() nicht waehlbar.) */
    dsb_sy();
    t->used = 1;

    spin_unlock(&s_tasklock);
    WRITE_SYSREG(daif, flags);
    return slot;
}

int task_create_on(uint32_t core, task_entry_t entry, void *arg,
                   uint8_t priority, const char *name)
{
    return task_create_internal(core, entry, arg, priority, name, /*admit=*/1);
}

/* Wie task_create_on, legt den Task aber NICHT-schedulebar (TASK_SETUP) an. Der Aufrufer
 * setzt ttbr/cred (task_set_user_aspace/sched_set_cred) und gibt ihn dann mit task_admit()
 * frei. Fuer Laufzeit-Spawn eines EL0-Prozesses auf einem (evtl. anderen) Kern. */
int task_create_suspended_on(uint32_t core, task_entry_t entry, void *arg,
                             uint8_t priority, const char *name)
{
    return task_create_internal(core, entry, arg, priority, name, /*admit=*/0);
}

#if defined(RTOS_SELFTEST) && defined(GUI_FP)
/* Guardian (white-box, Review T3.x): Die EL0-Reuse-Probe (dritte FPTEST-Instanz)
 * ist NICHT slot-deterministisch -- first-fit vergibt meist einen Slot, dessen fpctx
 * durch das Restore/Save-Waschen ohnehin genullt ist (jeder Task laedt beim Einwechseln
 * SEINEN Kontext, bevor sein Save zurueckschreibt). Dieser Test beweist die Zero-Init-
 * Zeile in task_create_internal DIREKT: den first-fit-Slot mit 0xAA vergiften, einen
 * Wegwerf-Task anlegen (TASK_SETUP, laeuft nie), fpctx muss genullt sein, Slot freigeben.
 * VOR sched_start() aufrufen (single-threaded -> keine Allokator-Races). */
static void fpctx_probe_entry(void *arg) { (void)arg; task_exit(0); }
int sched_fpctx_zeroinit_selftest(void)
{
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) { if (!tasks[i].used) { slot = i; break; } }
    if (slot < 0) { return 0; }
    for (int i = 0; i < FPCTX_BYTES; i++) { fpctx[slot][i] = 0xAA; }

    int tid = task_create_suspended_on(0, fpctx_probe_entry, 0, 250, "fpprobe");
    if (tid < 0) { return 0; }
    int clean = 1;
    for (int i = 0; i < FPCTX_BYTES; i++) { if (fpctx[tid][i] != 0) { clean = 0; break; } }

    /* Slot zurueckgeben: nie admitted (TASK_SETUP) -> von keinem Scheduler-Leser waehlbar;
     * Freigabe unter s_tasklock wie in reap_pending. */
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_tasklock);
    tasks[tid].state = TASK_UNUSED;
    tasks[tid].owner = 0xFF;
    dsb_sy();
    tasks[tid].used  = 0;
    spin_unlock(&s_tasklock);
    WRITE_SYSREG(daif, f);
    return clean;
}
#endif

/* Gibt einen mit task_create_suspended_on angelegten Task frei (TASK_SETUP -> TASK_READY).
 * Mit maskierten IRQs aufrufen, NACHDEM ttbr/cred gesetzt sind. dsb publiziert ttbr/cred
 * (Release), BEVOR der Task fuer fremde Kerne schedulebar wird; laeuft er auf einem ANDEREN
 * Kern, weckt ein Reschedule-IPI dessen Eigentuemer-Kern sofort. */
void task_admit(int tid)
{
    if (tid < 0 || tid >= MAX_TASKS) {
        return;
    }
    tcb_t *t = &tasks[tid];
    dsb_sy();                      /* ttbr/cred (Release) veroeffentlichen, BEVOR READY steht */
    t->state = TASK_READY;
    uint32_t owner = t->owner;
    if (owner < NCORES && owner != cpu_id()) {
        dmb_sy();                  /* READY veroeffentlichen, BEVOR need_resched gesetzt wird --
                                    * sonst koennte der Ziel-Kern das Flag vor dem State sehen,
                                    * es per Timer-Tick loeschen und den noch-SETUP-Task ueber-
                                    * springen (verzoegertes Wecken). Spiegelt sched_wake_one. */
        need_resched[owner] = 1;
        dmb_sy();
        gic_send_sgi(SGI_RESCHED, owner);
    }
}

int task_create(task_entry_t entry, void *arg, uint8_t priority, const char *name)
{
    return task_create_on(0, entry, arg, priority, name);   /* Default: Kern 0 */
}

/* Hoechste Prioritaet unter den Tasks DIESES Kerns bestimmen und per Round-Robin
 * den naechsten lauffaehigen waehlen. Nur Tasks mit owner==cid kommen in Frage. */
static int pick_next(int cid)
{
    int cur = current[cid];
    int best_prio = 256;
    for (int i = 0; i < MAX_TASKS; i++) {
        tcb_t *t = &tasks[i];
        if (!t->used || t->owner != cid) continue;
        int cand = (t->state == TASK_READY) ||
                   (i == cur && t->state == TASK_RUNNING);
        if (cand && t->priority < best_prio) {
            best_prio = t->priority;
        }
    }
    for (int k = 1; k <= MAX_TASKS; k++) {
        int i = (cur + k) % MAX_TASKS;
        tcb_t *t = &tasks[i];
        if (!t->used || t->owner != cid) continue;
        int cand = (t->state == TASK_READY) ||
                   (i == cur && t->state == TASK_RUNNING);
        if (cand && t->priority == best_prio) {
            return i;
        }
    }
    return cur;   /* Fallback (eigener Idle ist immer lauffaehig) */
}

/* Einen verzoegert freizugebenden ZOMBIE-Slot dieses Kerns einsammeln, SOBALD wir NICHT mehr
 * auf seinem Kontext laufen (current[cid] != reap): dann ist sein letzter ctx_sp-Write im
 * context_switch garantiert abgeschlossen, der Slot kann gefahrlos wiederverwendet werden.
 * Reklamation unter s_tasklock (synchron mit dem Allokator-Scan); danach schreibt kein Kern
 * mehr tasks[r].ctx_sp -> der Allokator darf den Slot ohne Race neu befuellen. Mit
 * maskierten IRQs aufrufen. */
static void reap_pending(int cid)
{
    int r = s_reap[cid];
    if (r < 0 || r == current[cid]) {
        return;
    }
    s_reap[cid] = -1;
    spin_lock(&s_tasklock);
    tasks[r].state = TASK_UNUSED;
    tasks[r].owner = 0xFF;
    dsb_sy();
    tasks[r].used  = 0;
    spin_unlock(&s_tasklock);
}

/* Kernroutine. Nur mit maskierten IRQs aufrufen. Operiert auf dem aufrufenden Kern. */
static void schedule(void)
{
    int cid = (int)cpu_id();
    reap_pending(cid);            /* freigegebenen Vorgaenger-Slot einsammeln (s. task_exit) */
    need_resched[cid] = 0;
    int curidx = current[cid];
    int next   = pick_next(cid);
    tcb_t *prev = &tasks[curidx];

    if (next == curidx) {
        prev->slice = TIME_SLICE;   /* Quantum erneuern, kein Wechsel */
        prev->state = TASK_RUNNING; /* Invariante: der laufende Task ist RUNNING -- auch wenn
                                     * ein cross-core sched_wake_one ihn (als noch current) auf
                                     * READY gesetzt hatte. */
        return;
    }

    tcb_t *nxt = &tasks[next];
    /* Acquire: erst NACH dieser Barriere die Felder (ttbr/ctx_sp) des gewaehlten Tasks
     * lesen. Pairt mit der Release-Barriere (dsb) in task_create_internal (vor used=1) bzw.
     * task_admit (vor state=READY): ein evtl. von einem ANDEREN Kern frisch publizierter
     * Task wird hier mit konsistentem ttbr/ctx_sp gesehen (sonst koennte der Table-Walker
     * beim Laufzeit-Spawn einen stale TTBR0 laden). */
    dmb_sy();
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    nxt->state = TASK_RUNNING;
    nxt->slice = TIME_SLICE;
    current[cid] = next;

    /* Adressraum des naechsten Tasks installieren (per-Prozess TTBR0/ASID; Kernel-Tasks
     * nutzen den Kernel-Aspace). ASID-getaggt -> kein TLB-Flush beim Switch. */
    mmu_switch(nxt->ttbr);

#ifdef GUI_FP
    /* FP/SIMD-Zustand des scheidenden Tasks sichern, den des naechsten laden.
     * Zwischen restore und dem eret/Weiterlaufen des naechsten Tasks fasst kein Kernel-
     * Code die V-Register an (Kernel baut -mgeneral-regs-only; IRQs sind maskiert). */
    fpctx_save(fpctx[curidx]);
    fpctx_restore(fpctx[next]);
#endif
    context_switch(&prev->ctx_sp, &nxt->ctx_sp);
}

void task_set_user_aspace(int tid, uint64_t user_phys, uint64_t ttbr)
{
    if (tid >= 0 && tid < MAX_TASKS) {
        tasks[tid].user_phys = user_phys;
        tasks[tid].ttbr      = ttbr;
    }
}

int sched_current_tid(void)
{
    return current[cpu_id()];
}

/* Monotone PID des laufenden Tasks (fuer SYS_GETPID / Diagnose). 0 = keiner. */
uint64_t sched_current_pid(void)
{
    int s = current[cpu_id()];
    return (s >= 0 && s < MAX_TASKS && tasks[s].used) ? tasks[s].pid : 0;
}

/* Monotone PID eines Task-Slots (fuer Logmeldungen des Erzeugers). 0 = ungueltig. */
uint64_t task_pid(int tid)
{
    return (tid >= 0 && tid < MAX_TASKS && tasks[tid].used) ? tasks[tid].pid : 0;
}

/* --- Credential (uid/caps) --- */
void sched_set_cred(int tid, uint32_t uid, uint32_t caps)
{
    if (tid >= 0 && tid < MAX_TASKS) {
        tasks[tid].uid  = uid;
        tasks[tid].caps = caps;
    }
}

uint32_t sched_current_uid(void)
{
    return tasks[current[cpu_id()]].uid;
}

uint32_t sched_current_caps(void)
{
    return tasks[current[cpu_id()]].caps;
}

static void (*s_exit_hook)(uint64_t user_phys);
void sched_set_exit_hook(void (*fn)(uint64_t user_phys))
{
    s_exit_hook = fn;
}

/* --- IPC-Unterstuetzung (nur mit maskierten IRQs aufrufen) ---
 * Cross-core wait/wake IST abgesichert: Block/Wake laufen unter s_ipclock (spinlock LDAXR/STLR =
 * Acquire/Release); die lock-frei gelesenen Felder (state/priority) sind volatile. Der weckende
 * Kern veroeffentlicht state=READY per dmb_sy und stoesst den OWNER-Kern via need_resched[owner]
 * + SGI_RESCHED an (Acquire in schedule_if_needed); sched_wake_one waehlt das Ziel anhand wait_obj
 * und weckt dessen owner-Kern -- unabhaengig vom Aufruf-Kern. Zustellung ist bounded-by-one-slice
 * (need_resched-Clobber im nebenlaeufigen freiwilligen schedule(), s. sched_set_prio). */
/* IPC-Lock fuer ipc.c: serialisiert Block/Wake + IPC-Objektzustaende cross-core. Der
 * Aufrufer haelt zusaetzlich IRQs maskiert. */
void sched_ipc_lock(void)   { spin_lock(&s_ipclock); }
void sched_ipc_unlock(void) { spin_unlock(&s_ipclock); }

/* Aufrufer haelt s_ipclock + IRQs maskiert. Setzt den aktuellen Task BLOCKED und gibt den
 * IPC-Lock WAEHREND schedule() frei -- sonst koennte der weckende Kern ihn nicht nehmen
 * (Deadlock). Nach dem Wecken wird der Lock wieder uebernommen (der Aufrufer prueft die
 * Bedingung erneut). */
/* Blockiert den aktuellen Task auf 'obj' mit optionaler Deadline. deadline == 0 -> unendlich;
 * andernfalls weckt der Timer-Tick den Task, sobald timer_ticks() >= deadline (IPC-Timeout).
 * Rueckgabe: 1 = per Timeout geweckt, 0 = per sched_wake_one (echtes Ereignis) oder Kill.
 * Aufrufer haelt s_ipclock + IRQs maskiert (wird waehrend schedule() freigegeben). */
int sched_block_on_timeout(void *obj, uint64_t deadline)
{
    int cid = (int)cpu_id();
    int me  = current[cid];
    tasks[me].timed_out = 0;              /* frischer Warte-Versuch */
    tasks[me].state     = TASK_BLOCKED;
    tasks[me].wait_obj  = obj;
    tasks[me].wake_tick = deadline;       /* 0 = keine Deadline (unendlich) */
    spin_unlock(&s_ipclock);
    schedule();
    spin_lock(&s_ipclock);
    return tasks[me].timed_out;
}

void sched_block_on(void *obj)
{
    /* Unendliches Warten -- deadline 0 setzt zugleich wake_tick=0 zurueck, damit ein evtl.
     * stale Weck-Tick aus einem frueheren task_sleep den Task nicht spurios per Timeout weckt. */
    (void)sched_block_on_timeout(obj, 0);
}

int sched_wake_one(void *obj)
{
    int best = -1, best_prio = 256;
    for (int i = 0; i < MAX_TASKS; i++) {
        tcb_t *t = &tasks[i];
        if (t->used && t->state == TASK_BLOCKED && t->wait_obj == obj &&
            t->priority < best_prio) {
            best_prio = t->priority;
            best = i;
        }
    }
    if (best >= 0) {
        tasks[best].state     = TASK_READY;
        tasks[best].wait_obj  = 0;
        tasks[best].wake_tick = 0;     /* Deadline verwerfen -- echtes Ereignis, kein Timeout */
        tasks[best].timed_out = 0;
        dmb_sy();              /* READY veroeffentlichen, BEVOR need_resched gesetzt wird
                                * (der weckende Kern darf das Flag nicht vor dem State sehen) */
        uint32_t owner = tasks[best].owner;
        need_resched[owner] = 1;
        /* Laeuft der geweckte Task auf einem ANDEREN Kern: diesen per Reschedule-IPI sofort
         * umplanen lassen, statt bis zu einen Timer-Tick (~10 ms) zu warten. */
        if (owner < NCORES && owner != cpu_id()) {
            gic_send_sgi(SGI_RESCHED, owner);
        }
    }
    return best;
}

uint8_t sched_get_prio(int tid)
{
    return (tid >= 0 && tid < MAX_TASKS) ? tasks[tid].priority : 255;
}

void sched_set_prio(int tid, uint8_t prio)
{
    if (tid < 0 || tid >= MAX_TASKS) {
        return;
    }
    tasks[tid].priority = prio;
    /* priority ist ein LOCK-FREI gelesener pick_next-Eingang. Ein PI-Boost (mutex_lock)
     * kann einen Task auf einem ANDEREN Kern hochstufen -> diesen owner-Kern anstossen (wie
     * sched_wake_one/sched_kill_pid), sonst evaluiert er die neue Prio erst beim naechsten
     * Slice-Ablauf (~100 ms) und die PI-Latenzschranke (Zweck der Priority-Inheritance) reisst.
     * Frueher fehlte dieser Handshake komplett. Reihenfolge: dmb_sy(Release) VOR need_resched,
     * damit A72 das Flag nicht vor dem priority-Store sieht; gic_send_sgi macht intern dsb_sy
     * (need_resched global sichtbar vor dem SGI). */
    dmb_sy();
    uint32_t owner = tasks[tid].owner;
    if (owner < NCORES) {
        need_resched[owner] = 1;
        if (owner != cpu_id()) {
            s_pi_remote_resched++;              /* unter s_ipclock (alle PI-Aufrufer) -> serialisiert */
            gic_send_sgi(SGI_RESCHED, owner);
        }
    }
    /* Bekannte Restgrenze (RC-vertretbar, dokumentiert): raeumt der owner-Kern in einem
     * NEBENLAEUFIGEN freiwilligen schedule() (yield/sleep/block) sein need_resched-Flag ab,
     * nachdem der Booster es setzte, aber sein barrierefreies pick_next die neue Prio noch nicht
     * sieht, verzoegert sich der Boost auf einen Slice. Die typische Latenz ist gefixt; die
     * strikte Schranke braeuchte "sticky" need_resched (nur nach Acquire-Konsum geloescht) --
     * Post-RC (Teil der transitiven PI, Tier 2). */
}

/* Anzahl Cross-Core-PI-Boost-Reschedules (Diagnose/Guardian). */
uint32_t sched_pi_remote_count(void)
{
    return s_pi_remote_resched;
}

void sched_reschedule(void)
{
    schedule();
}

void sched_tick(void)
{
    int cid = (int)cpu_id();
    if (!sched_started[cid]) {
        return;
    }
    uint64_t now = timer_ticks();          /* globale Zeitbasis (Kern 0) */

    for (int i = 0; i < MAX_TASKS; i++) {
        tcb_t *t = &tasks[i];
        if (t->used && t->owner == cid && t->state == TASK_SLEEPING &&
            t->wake_tick <= now) {
            t->state = TASK_READY;
            need_resched[cid] = 1;
        }
    }

    /* IPC-Timeout: blockierte Tasks mit abgelaufener Deadline (wake_tick != 0) wecken. Unter
     * s_ipclock, damit dies mit sched_wake_one (Cross-Core) mutually exclusive ist -> genau
     * einer gewinnt den BLOCKED->READY-Uebergang; wer den Task per Timeout weckt, setzt
     * timed_out=1 (der Warter liefert dann -1/-ETIMEDOUT), wer ihn per Ereignis weckt, 0. */
    spin_lock(&s_ipclock);
    for (int i = 0; i < MAX_TASKS; i++) {
        tcb_t *t = &tasks[i];
        if (t->used && t->owner == cid && t->state == TASK_BLOCKED &&
            t->wake_tick != 0 && t->wake_tick <= now) {
            t->state     = TASK_READY;
            t->wait_obj  = 0;
            t->wake_tick = 0;
            t->timed_out = 1;
            need_resched[cid] = 1;
        }
    }
    spin_unlock(&s_ipclock);

    tcb_t *cur = &tasks[current[cid]];
    if (cur->slice > 0) {
        cur->slice--;
    }
    if (cur->slice == 0) {
        need_resched[cid] = 1;
    }

    if (*(volatile uint64_t *)&kstacks[current[cid]][0] != STACK_CANARY) {
        uart_puts("\n*** KERNEL PANIC: Stack-Overflow in Task '");
        uart_puts(cur->name ? cur->name : "?");
        uart_puts("' ***\n");
        for (;;) {
            wfe();
        }
    }
}

/* Aus dem IRQ-Handler bei einem Reschedule-IPI (SGI) aufgerufen. Der eigentliche Umplan-
 * Schritt passiert ohnehin in schedule_if_needed() nach dem EOI; hier nur mitzaehlen. */
void sched_ipi_received(void)
{
    s_ipi_count[cpu_id()]++;
}

uint32_t sched_ipi_count(uint32_t cid)
{
    return (cid < NCORES) ? s_ipi_count[cid] : 0;
}

void schedule_if_needed(void)
{
    int cid = (int)cpu_id();
    if (!sched_started[cid] || !need_resched[cid]) {
        return;
    }
    dmb_sy();              /* Acquire: nach Beobachten von need_resched die vom weckenden Kern
                            * veroeffentlichten Task-States (READY) garantiert sehen */
    need_resched[cid] = 0;
    schedule();
}

void sched_yield(void)
{
    uint64_t flags = READ_SYSREG(daif);
    irq_disable();
    schedule();
    WRITE_SYSREG(daif, flags);
}

void task_sleep_ticks(uint64_t ticks)
{
    uint64_t flags = READ_SYSREG(daif);
    irq_disable();
    int cid = (int)cpu_id();
    tcb_t *t = &tasks[current[cid]];
    t->wake_tick = timer_ticks() + ticks;
    t->state     = TASK_SLEEPING;
    schedule();
    WRITE_SYSREG(daif, flags);
}

/* --- Prozess-Handles: Exit-Records + wait/kill (unter s_ipclock) --- */
static int exitrec_find(uint64_t pid, uint64_t ppid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (s_exitrec[i].used && s_exitrec[i].pid == pid && s_exitrec[i].ppid == ppid) {
            return i;
        }
    }
    return -1;
}

/* Lebenden Task (used, nicht ZOMBIE) mit dieser PID finden -> Slot-Index / -1. */
static int find_live_task_by_pid(uint64_t pid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].used && tasks[i].pid == pid && tasks[i].state != TASK_ZOMBIE) {
            return i;
        }
    }
    return -1;
}

static void exitrec_put(uint64_t pid, uint64_t ppid, int32_t code)
{
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!s_exitrec[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        /* Tabelle voll -> bevorzugt einen verwaisten Record verdraengen (Elternprozess nicht mehr
         * am Leben -> koennte ihn ohnehin nie ernten). Erst wenn keiner verwaist ist, den ersten
         * Slot opfern (dann geht ein echter Exit-Status verloren; nur bei >MAX_TASKS lebenden,
         * nicht-erntenden Elternprozessen erreichbar). */
        for (int i = 0; i < MAX_TASKS; i++) {
            if (find_live_task_by_pid(s_exitrec[i].ppid) < 0) { slot = i; break; }
        }
        if (slot < 0) { slot = 0; }
    }
    s_exitrec[slot].pid  = pid;
    s_exitrec[slot].ppid = ppid;
    s_exitrec[slot].code = code;
    s_exitrec[slot].used = 1;
}

/* Lebt ein EL0-Kindprozess 'target' des Elternprozesses 'parent' noch (nicht ZOMBIE)? */
static int child_alive(uint64_t target, uint64_t parent)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].used && tasks[i].pid == target && tasks[i].ppid == parent &&
            tasks[i].user_phys && tasks[i].state != TASK_ZOMBIE) {
            return 1;
        }
    }
    return 0;
}

void sched_set_ppid(int tid, uint64_t ppid)
{
    if (tid >= 0 && tid < MAX_TASKS) {
        tasks[tid].ppid = ppid;
    }
}

uint64_t sched_current_ppid(void)
{
    int s = current[cpu_id()];
    return (s >= 0 && s < MAX_TASKS && tasks[s].used) ? tasks[s].ppid : 0;
}

long sched_wait_pid(uint64_t target)
{
    if (target == 0) {
        return -1;                     /* kein Wildcard-Wait in dieser Version */
    }
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    int cid    = (int)cpu_id();
    int me_idx = current[cid];
    uint64_t me = tasks[me_idx].pid;
    spin_lock(&s_ipclock);
    long ret;
    for (;;) {
        int r = exitrec_find(target, me);
        if (r >= 0) {                  /* Kind hat bereits geendet -> Code ernten + Record frei */
            ret = (long)s_exitrec[r].code;
            s_exitrec[r].used = 0;
            break;
        }
        if (!child_alive(target, me)) {
            ret = -1;                  /* kein eigenes/lebendes Kind mit dieser PID (auch: schon geerntet) */
            break;
        }
        /* Kind lebt noch, aber ohne Record -> blockieren; sched_block_on gibt s_ipclock waehrend
         * schedule() frei und nimmt ihn danach wieder. task_exit() des Kindes weckt uns. Danach
         * die Bedingung erneut pruefen (Schleife). */
        sched_block_on(&tasks[me_idx]);
        /* Kill-aware: Wurde DIESER Warter selbst per SYS_KILL markiert (sched_kill_pid weckt ihn
         * dafuer aus BLOCKED), beendet er sich HIER selbst -- sonst wuerde er bei noch lebendem Kind
         * endlos re-blocken und den Kill nie am EL0-Safe-Point erreichen. Hier ist nur s_ipclock
         * gehalten (kein fs_lock o.ae.) -> sicherer Kernel-Safe-Point. */
        if (tasks[me_idx].kill_pending) {
            spin_unlock(&s_ipclock);
            WRITE_SYSREG(daif, f);
            task_exit(TASK_EXIT_KILLED);   /* kehrt nie zurueck */
        }
    }
    spin_unlock(&s_ipclock);
    WRITE_SYSREG(daif, f);
    return ret;
}

int sched_kill_pid(uint64_t target, uint64_t caller_pid, int caller_is_admin)
{
    if (target == 0 || target == caller_pid) {
        return -1;                     /* ungueltig / kein Selbst-Kill ueber diesen Pfad */
    }
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_ipclock);
    int rc  = -1;
    int idx = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].used && tasks[i].pid == target && tasks[i].user_phys &&
            tasks[i].state != TASK_ZOMBIE) {
            idx = i;
            break;
        }
    }
    if (idx >= 0 && (tasks[idx].ppid == caller_pid || caller_is_admin)) {
        tasks[idx].kill_pending = 1;   /* Code ist konstant TASK_EXIT_KILLED -> kein separates Feld noetig */
        /* Ein schlafendes/blockiertes Ziel aufwecken, damit es einen Safe-Point erreicht und sich
         * beendet: SYS_SLEEP (SLEEPING) kehrt nach EL0 zurueck -> Safe-Point; SYS_WAIT (BLOCKED)
         * prueft kill_pending direkt nach dem Wecken in sched_wait_pid (kill-aware Schleife). */
        if (tasks[idx].state == TASK_SLEEPING || tasks[idx].state == TASK_BLOCKED) {
            tasks[idx].state     = TASK_READY;
            tasks[idx].wait_obj  = 0;
            tasks[idx].wake_tick = 0;   /* evtl. Timeout-Deadline verwerfen (Task wird beendet) */
        }
        dmb_sy();                      /* kill_pending/READY veroeffentlichen, bevor der Kern angestossen wird */
        uint32_t owner = tasks[idx].owner;
        need_resched[owner] = 1;
        if (owner < NCORES && owner != cpu_id()) {
            gic_send_sgi(SGI_RESCHED, owner);   /* remote: sofort umplanen -> Safe-Point-Check */
        }
        rc = 0;
    }
    spin_unlock(&s_ipclock);
    WRITE_SYSREG(daif, f);
    return rc;
}

void sched_exit_if_killed(void)
{
    int cid = (int)cpu_id();
    int idx = current[cid];
    if (idx >= 0 && idx < MAX_TASKS && tasks[idx].used && tasks[idx].kill_pending) {
        task_exit(TASK_EXIT_KILLED);   /* kehrt nie zurueck */
    }
}

void task_exit(int code)
{
    irq_disable();
    int cid = (int)cpu_id();
    reap_pending(cid);                /* evtl. noch offenen Vorgaenger-Zombie zuerst einsammeln
                                       * (verhindert, dass ein zweiter Exit ihn ueberschreibt) */
    int idx  = current[cid];
    tcb_t *t = &tasks[idx];
    /* Exit-Record fuer einen wartbaren EL0-Kindprozess ablegen + einen in sched_wait_pid
     * blockierten Elternprozess wecken. Unter s_ipclock, VOR der Slot-/Aspace-Freigabe -- der
     * Record wird sichtbar, bevor der Task ZOMBIE wird (ein wait, das s_ipclock haelt, sieht ihn).
     * NUR ablegen, wenn der Elternprozess noch lebt (sonst kann ihn niemand je ernten -> kein
     * verwaister Record). Exit-Code auf 8 Bit maskiert (POSIX-artig) -> immer 0..255, kollidiert
     * nie mit dem -1-Fehler-Sentinel von sched_wait_pid. */
    if (t->user_phys && t->ppid) {
        spin_lock(&s_ipclock);
        int pidx = find_live_task_by_pid(t->ppid);
        if (pidx >= 0) {
            exitrec_put(t->pid, t->ppid, (int32_t)(code & 0xFF));
            sched_wake_one(&tasks[pidx]);   /* weckt den auf &tasks[pidx] blockierten Elternprozess */
        }
        spin_unlock(&s_ipclock);
    }
    if (t->user_phys && s_exit_hook) {
        s_exit_hook(t->user_phys);    /* Prozess-Ressourcen (Aspace/Slot) freigeben (proc.c) */
    }
    /* NICHT sofort used=0 setzen: schedule() unten ruft context_switch, das tasks[idx].ctx_sp
     * ein LETZTES Mal schreibt. Wuerde der Slot vorher freigegeben, koennte ein paralleler
     * Laufzeit-Spawn auf Kern 0 ihn neu vergeben und dieser ctx_sp-Write das frische ctx_sp
     * des neuen Tasks zerstoeren (Use-after-free). Stattdessen als ZOMBIE markieren (used bleibt
     * 1 -> Allokator ueberspringt ihn; state != READY -> pick_next waehlt ihn nie) und erst
     * reap_pending() beim naechsten schedule() dieses Kerns -- nach dem ctx_sp-Write -- frei. */
    t->state    = TASK_ZOMBIE;
    s_reap[cid] = idx;
    schedule();                       /* wechselt zum Idle/anderen Task, kehrt nie zurueck */

    irq_enable();
    for (;;) {
        wfe();
    }
}

/* Ersten lauffaehigen Task DIESES Kerns auswaehlen und hineinspringen. */
static void sched_enter(void)
{
    /* Erststart IMMER mit maskierten IRQs (Scheduler-Invariante) -- auf Sekundaerkernen
     * sind die IRQs in secondary_main schon freigegeben; das Trampolin gibt sie fuer den
     * Task wieder frei. Ohne dies koennte ein Timer-IRQ schedule() auf dem Wegwerf-Boot-
     * kontext reentrant ausloesen. */
    irq_disable();
    int cid = (int)cpu_id();
    int first = -1;
    int best_prio = 256;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].used && tasks[i].owner == cid &&
            tasks[i].state == TASK_READY && tasks[i].priority < best_prio) {
            best_prio = tasks[i].priority;
            first = i;
        }
    }
    if (first < 0) {
        return;   /* sollte nie passieren (eigener Idle ist READY) */
    }

    tasks[first].state = TASK_RUNNING;
    tasks[first].slice = TIME_SLICE;
    current[cid]   = first;
    sched_started[cid] = 1;

    mmu_switch(tasks[first].ttbr);   /* Adressraum des ersten Tasks */

#ifdef GUI_FP
    /* der Boot-Kontext hat keinen FP-Bereich (wird weggeworfen) -- nur den
     * (genullten) Zustand des ersten Tasks laden: deterministischer FP-Start statt
     * architektonisch unbestimmter Power-On-/Firmware-Reste in den V-Registern. */
    fpctx_restore(fpctx[first]);
#endif
    /* Erster Wechsel: Boot-Kontext dieses Kerns wegwerfen, in den ersten Task
     * springen. IRQs bleiben maskiert; das Trampolin gibt sie frei. */
    context_switch(&boot_ctx_sp[cid], &tasks[first].ctx_sp);
    /* nicht erreichbar */
}

void sched_start(void)
{
    sched_enter();    /* Kern 0 */
}

void sched_start_secondary(void)
{
    sched_enter();    /* Sekundaerkern: laeuft seine eigenen Tasks + Idle */
}
