/*
 * kernel/proc.c  --  Laden & Starten von EL0-User-Prozessen
 *
 * Liest ein ELF64 via VFS in einen eigenen physischen 2-MiB-Bereich und legt einen
 * Task mit EIGENEM Adressraum an (per-Prozess TTBR0/ASID, mmu_create_aspace): die
 * User-VA (USER_BASE) zeigt je Prozess auf dessen phys. Bereich. Der Scheduler setzt
 * beim Umschalten TTBR0 auf den Adressraum des Tasks -> Prozesse koennen auf
 * beliebigen Kernen laufen (isolierte Adressraeume, EL0 SMP-faehig).
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "mmu.h"
#include "user.h"    /* USER_CAP_GUI */
#include "elf.h"
#include "vfs.h"
#include "sched.h"
#include "spinlock.h"
#include "proc.h"

static uint8_t elf_buf[1048576];         /* Lade-Puffer (1 MiB): deckt die wachsende Vulkan-Lib
                                          * (VKTEST.ELF > 128 KiB durch V2-Interpreter-Ausbau). Liegt im
                                          * BSS -> kein kernel8.img-Zuwachs; laedt weiter in die 8-MiB-Tile. */

/* Schuetzt die slot_used[]-Buchhaltung cross-core. Schluessel: ein Laufzeit-Spawn auf einen
 * Sekundaerkern wird zwar von Kern 0 (Shell/Demo) ALLOZIERT, aber auf dem ZIEL-Kern (z.B.
 * Kern 1) wieder FREIGEGEBEN -- der Exit-Hook proc_free_slot laeuft dort, wo der Prozess
 * endet. Alloc (Kern 0) und Free (Ziel-Kern) laufen also nebenlaeufig auf slot_used[].
 * Immer mit maskierten IRQs nehmen (aus preemptierbarem Kontext). */
static spinlock_t s_proclock = SPINLOCK_INIT;

/* Serialisiert den ELF-Ladevorgang in den GLOBALEN elf_buf. Ein Laufzeit-Spawn kann von
 * einem beliebigen Kern ausgehen (EL0 auf einem Sekundaerkern ruft SYS_SPAWN) bzw. auf
 * einem Kern preemptiert werden -> ohne diesen Lock wuerden zwei Laeufe denselben elf_buf
 * ueberschreiben und ein korruptes Image nach phys kopieren. Mit maskierten IRQs halten
 * (konsistent mit dem fat32/SD-Lock, der den SD-Read ohnehin maskiert). Lock-Reihenfolge:
 * s_loadlock aussen -> fs_lock innen (vfs_read_file); kein Pfad nimmt sie umgekehrt. */
static spinlock_t s_loadlock = SPINLOCK_INIT;

struct uproc {
    uint64_t entry;
    uint64_t sp;
    uint64_t phys;
    int      aspace;          /* per-Prozess-Adressraum (mmu_create_aspace) */
};
static struct uproc procs[MAX_USER_PROCS];
static int          slot_used[MAX_USER_PROCS];
static int          hook_registered;

/* Vom Scheduler beim Beenden eines User-Tasks gerufen: Slot wieder freigeben,
 * sodass die Shell beliebig oft Programme starten kann. */
static void proc_free_slot(uint64_t user_phys)
{
    /* Laeuft auf dem Kern, auf dem der Prozess endete (kann ein Sekundaerkern sein) ->
     * slot_used[] unter s_proclock anfassen (Alloc laeuft evtl. parallel auf Kern 0).
     * mmu_free_aspace (broadcast tlbi) AUSSERHALB des Locks -> keine Lock-Verschachtelung. */
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_proclock);
    int aspace = -1;
    for (int i = 0; i < MAX_USER_PROCS; i++) {
        if (slot_used[i] && procs[i].phys == user_phys) {
            aspace = procs[i].aspace;
            slot_used[i] = 0;
            break;
        }
    }
    spin_unlock(&s_proclock);
    WRITE_SYSREG(daif, f);
    if (aspace >= 0) {
        mmu_free_aspace(aspace);                /* ASID broadcast-invalidieren + Aspace frei */
    }
}

/* Fehlerpfad: einen reservierten Slot wieder freigeben (cross-core unter s_proclock). */
static void slot_release(int slot)
{
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_proclock);
    slot_used[slot] = 0;
    spin_unlock(&s_proclock);
    WRITE_SYSREG(daif, f);
}

/* Laeuft als Kernel-Thread (EL1) und wechselt dann nach EL0. arg = &procs[i]. */
static void enter_user(void *arg)
{
    struct uproc *u     = (struct uproc *)arg;
    uint64_t      entry = u->entry;
    uint64_t      sp    = u->sp;
    __asm__ volatile(
        "msr    daifset, #2\n"          /* IRQs maskieren: Setup + eret atomar */
        "msr    sp_el0, %0\n"
        "msr    elr_el1, %1\n"
        "mov    x0, #0x340\n"            /* SPSR: EL0t, nur IRQ aktiv (eret reaktiviert) */
        "msr    spsr_el1, x0\n"
        /* Alle GP-Register loeschen -> keine Kernel-Zeiger leaken nach EL0. */
        "mov    x0, xzr\n  mov  x1, xzr\n  mov  x2, xzr\n  mov  x3, xzr\n"
        "mov    x4, xzr\n  mov  x5, xzr\n  mov  x6, xzr\n  mov  x7, xzr\n"
        "mov    x8, xzr\n  mov  x9, xzr\n  mov  x10, xzr\n mov  x11, xzr\n"
        "mov    x12, xzr\n mov  x13, xzr\n mov  x14, xzr\n mov  x15, xzr\n"
        "mov    x16, xzr\n mov  x17, xzr\n mov  x18, xzr\n mov  x19, xzr\n"
        "mov    x20, xzr\n mov  x21, xzr\n mov  x22, xzr\n mov  x23, xzr\n"
        "mov    x24, xzr\n mov  x25, xzr\n mov  x26, xzr\n mov  x27, xzr\n"
        "mov    x28, xzr\n mov  x29, xzr\n mov  x30, xzr\n"
        "isb\n"
        "eret\n"
        :: "r"(sp), "r"(entry) : "memory");
    __builtin_unreachable();
}

/* Geladene Instruktionen kohaerent machen: D-Cache zum PoU bereinigen (an der
 * identitaetsgemappten Lade-Adresse), dann gesamten I-Cache invalidieren. */
static void isync_loaded(uint64_t base, uint64_t size)
{
    for (uint64_t a = base; a < base + size; a += 64) {
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    }
    /* I-Cache broadcast (inner-shareable) invalidieren: ein User-Prozess kann auf einem
     * ANDEREN Kern laufen als dem, der hier laedt -- 'ic iallu' (kern-lokal) wuerde dessen
     * I-Cache bei Slot-/phys-Reuse nicht treffen (stale Instruktionen). */
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
}

int proc_exec_as_on(uint32_t core, const char *path, uint32_t uid, uint32_t caps,
                    uint64_t ppid, uint64_t *out_pid)
{
    /* Freien Slot suchen und SOFORT reservieren. s_proclock (+ IRQs maskiert): der Free auf
     * dem Ziel-Kern darf nicht mit dieser Allokation kollidieren. */
    uint64_t flags = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_proclock);
    int slot = -1;
    for (int i = 0; i < MAX_USER_PROCS; i++) {
        if (!slot_used[i]) { slot = i; slot_used[i] = 1; break; }
    }
    spin_unlock(&s_proclock);
    WRITE_SYSREG(daif, flags);
    if (slot < 0) {
        uart_puts("[proc] kein freier Prozess-Slot\n");
        return -1;
    }

    /* Laden in den globalen elf_buf + Kopie nach phys unter s_loadlock (IRQ-maskiert):
     * macht den gesamten Ladevorgang atomar gegen einen parallelen Spawn auf einem anderen
     * Kern ODER eine Preemption auf demselben Kern. Nur das elf_buf-Fenster ist abgedeckt;
     * isync_loaded danach arbeitet auf phys (nicht elf_buf). */
    uint64_t phys = USER_PHYS_BASE + (uint64_t)slot * USER_SIZE;
    uint64_t entry = 0;
    uint64_t lf = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_loadlock);
    int n = vfs_read_file(path, elf_buf, sizeof(elf_buf));
    /* n = WAHRE Dateigroesse: n > sizeof(elf_buf) heisst, die ELF passt nicht in den Ladepuffer
     * (nur min(n, buf) Bytes wurden gelesen) -> ablehnen, statt eine TRUNKIERTE ELF zu laden. */
    int loaded = (n >= 0) && (n <= (int)sizeof(elf_buf)) &&
                 (elf_load(elf_buf, (uint32_t)n, phys, &entry) == 0);
    spin_unlock(&s_loadlock);
    WRITE_SYSREG(daif, lf);
    if (!loaded) {
        slot_release(slot);
        uart_puts(n < 0 ? "[proc] Datei nicht gefunden: "
                        : "[proc] ELF-Laden fehlgeschlagen: ");
        uart_puts(path);
        uart_puts("\n");
        return -1;
    }
    isync_loaded(phys, USER_SIZE);

    procs[slot].entry = entry;
    procs[slot].sp    = USER_STACK_TOP;
    procs[slot].phys  = phys;

    /* Per-Prozess-Adressraum anlegen (eigene TTBR0/ASID, USER_BASE -> phys). Das GUI-Framebuffer-
     * Fenster bekommt nur ein Prozess mit USER_CAP_GUI (Least-Privilege fuer den Bildschirm). */
    int aspace = mmu_create_aspace(phys, (caps & USER_CAP_GUI) != 0);
    if (aspace < 0) {
        slot_release(slot);
        uart_puts("[proc] kein Adressraum frei\n");
        return -1;
    }
    procs[slot].aspace = aspace;
    uint64_t ttbr = mmu_aspace_ttbr(aspace);

    if (!hook_registered) {
        sched_set_exit_hook(proc_free_slot);
        hook_registered = 1;
    }

    /* Task-Erzeugung als TASK_SETUP (NICHT schedulebar): ttbr + cred setzen, DANN
     * task_admit() -> der Task wird erst freigegeben, wenn TTBR0 und cred stehen. Das
     * schliesst das Laufzeit-Fenster, in dem der ZIEL-Kern (evtl. != Kern 0) den Task mit
     * stale/Default-TTBR0 (Kernel-Aspace) einplanen und nach EL0 (USER_BASE, im Kernel-
     * Aspace ausgelassen) springen koennte. task_admit weckt den Ziel-Kern per IPI. */
    flags = READ_SYSREG(daif);
    irq_disable();
    int tid = task_create_suspended_on(core, enter_user, &procs[slot], 1, "user");
    /* PID sichern, SOLANGE der Task noch TASK_SETUP ist (nicht schedulebar): nach task_admit
     * kann er auf einem Sekundaerkern schon laufen + exiten + reaped werden -> task_pid()==0. */
    uint64_t pid = (tid >= 0) ? task_pid(tid) : 0;
    if (tid >= 0) {
        task_set_user_aspace(tid, phys, ttbr);
        sched_set_cred(tid, uid, caps);
        sched_set_ppid(tid, ppid);      /* Eltern-PID fuer wait/kill (0 = kernel-gestartet) */
        task_admit(tid);                /* publiziert ttbr/cred + weckt den Eigentuemer-Kern */
    }
    if (out_pid) {
        *out_pid = pid;                 /* PID des Kindes an den Aufrufer (SYS_SPAWN -> EL0) */
    }
    WRITE_SYSREG(daif, flags);
    if (tid < 0) {
        mmu_free_aspace(aspace);        /* Aspace freigeben (ausserhalb der IRQ-off-Sektion) */
        slot_release(slot);
        return -1;
    }

    uart_begin();                       /* Zeile cross-core atomar (Sekundaerkern druckt evtl. parallel) */
    uart_puts("[proc] User-Prozess pid=");
    uart_putdec(pid);                   /* monotone PID (nie recycled, != Slot-Index) */
    uart_puts(" (uid ");
    uart_putdec(uid);
    uart_puts(", slot ");
    uart_putdec(slot);
    uart_puts(")\n");
    uart_end();
    return tid;
}

int proc_exec_as(const char *path, uint32_t uid, uint32_t caps)
{
    return proc_exec_as_on(0, path, uid, caps, 0, 0);   /* Default: Kern 0, kein Elternprozess */
}

int proc_exec(const char *path)
{
    return proc_exec_as(path, 0, 0xFFFFFFFFu);   /* root, alle Capabilities */
}
