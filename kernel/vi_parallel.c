/*
 * kernel/vi_parallel.c  --  VISION-gegateter Kernel-Parallel-For
 *
 * Ganzer Inhalt #ifdef VISION -> ohne das Flag ein LEERES Objekt (wie arch/aarch64/fpctx.S),
 * der Kernel bleibt byte-identisch. Mechanik (siehe include/vi_parallel.h):
 *   - Co-Thread = Kernel-Task (EL1), dessen Entry-Fn co_enter() per enter_user-Muster
 *     (Vorlage kernel/proc.c) nach EL0 eret'et -- aber mit GETEILTEM ttbr0 des Aufrufers
 *     (task_set_user_aspace(tid, user_phys=0, caller_ttbr)) und gesetzten x0..x2 = (arg,wid,n).
 *   - user_phys=0 -> der Exit-Hook (proc_free_slot) feuert NICHT -> der geteilte Adressraum
 *     wird nicht vorzeitig freigegeben. Der Aufrufer (haelt user_phys!=0) endet erst NACH dem
 *     Join, sodass mmu_free_aspace nie einem laufenden Worker den Boden entzieht.
 *   - Join-Barrier per zaehlendem Semaphor (cross-core: sem_post weckt den Aufrufer-Kern per IPI).
 */
#ifdef VISION

#include <stdint.h>
#include "aarch64.h"
#include "sched.h"
#include "ipc.h"
#include "proc.h"
#include "vi_parallel.h"
#include "uvc.h"          /* A4.1b: dwc2_uvc_grab (UVC-HW-Backend) */

#define VI_MAX_WORKERS   4
#define VI_WORKER_STACK  (256u * 1024u)      /* 256 KiB je Co-Thread, als Slice unter USER_STACK_TOP */

struct co_arg {
    uint64_t entry;   /* EL0-Fn-VA */
    uint64_t arg;     /* -> x0 */
    uint64_t wid;     /* -> x1 */
    uint64_t n;       /* -> x2 */
    uint64_t sp;      /* sp_el0 (Stack-Slice) */
};

static semaphore_t   g_barrier;
static int           g_nworkers;
static struct co_arg g_coargs[VI_MAX_WORKERS];

/* Kernel-Task-Entry eines Co-Threads: nach EL0 an (entry, sp, x0..x2) eret'en. Klon von
 * enter_user (proc.c), aber mit gesetzten Argumenten. Die 5 Eingaben sind fest an x20..x24
 * gebunden (register-vars), damit die x0/x1/x2-Zuweisung nicht mit den Eingaberegistern
 * aliast; danach werden ALLE GP-Register ausser x0..x2 genullt (kein Kernel-Zeiger-Leak). */
static void co_enter(void *a)
{
    struct co_arg *c = (struct co_arg *)a;
    register uint64_t r_sp  __asm__("x20") = c->sp;
    register uint64_t r_ent __asm__("x21") = c->entry;
    register uint64_t r_a0  __asm__("x22") = c->arg;
    register uint64_t r_a1  __asm__("x23") = c->wid;
    register uint64_t r_a2  __asm__("x24") = c->n;
    __asm__ volatile(
        "msr    daifset, #2\n"              /* IRQs maskieren: Setup + eret atomar */
        "msr    sp_el0, %0\n"
        "msr    elr_el1, %1\n"
        "mov    x9, #0x340\n"               /* SPSR: EL0t, nur IRQ aktiv */
        "msr    spsr_el1, x9\n"
        "mov    x0, %2\n"                   /* arg */
        "mov    x1, %3\n"                   /* wid */
        "mov    x2, %4\n"                   /* n */
        "mov    x3, xzr\n  mov  x4, xzr\n  mov  x5, xzr\n  mov  x6, xzr\n"
        "mov    x7, xzr\n  mov  x8, xzr\n  mov  x9, xzr\n  mov  x10, xzr\n"
        "mov    x11, xzr\n mov  x12, xzr\n mov  x13, xzr\n mov  x14, xzr\n"
        "mov    x15, xzr\n mov  x16, xzr\n mov  x17, xzr\n mov  x18, xzr\n"
        "mov    x19, xzr\n mov  x20, xzr\n mov  x21, xzr\n mov  x22, xzr\n"
        "mov    x23, xzr\n mov  x24, xzr\n mov  x25, xzr\n mov  x26, xzr\n"
        "mov    x27, xzr\n mov  x28, xzr\n mov  x29, xzr\n mov  x30, xzr\n"
        "isb\n"
        "eret\n"
        :: "r"(r_sp), "r"(r_ent), "r"(r_a0), "r"(r_a1), "r"(r_a2) : "memory");
    __builtin_unreachable();
}

int vi_par_spawn(uint64_t entry, uint64_t arg, int n, uint32_t uid, uint32_t caps)
{
    if (n < 2) { return 0; }
    if (n > VI_MAX_WORKERS) { n = VI_MAX_WORKERS; }

    uint64_t ttbr;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));   /* Adressraum des Aufrufers (im Syscall) */

    sem_init(&g_barrier, 0);
    int spawned = 0;
    for (int w = 1; w < n; w++) {
        g_coargs[w].entry = entry;
        g_coargs[w].arg   = arg;
        g_coargs[w].wid   = (uint64_t)w;
        g_coargs[w].n     = (uint64_t)n;
        /* Stack-Slice unter USER_STACK_TOP: der Aufrufer (wid=0) behaelt die oberen 512 KiB,
         * Worker w liegt bei TOP-(w+1)*256KiB (disjunkte 256-KiB-Slices, weit ueber der BSS). */
        g_coargs[w].sp    = USER_STACK_TOP - (uint64_t)(w + 1) * VI_WORKER_STACK;

        int tid = task_create_suspended_on((uint32_t)w, co_enter, &g_coargs[w], 1, "vipar");
        if (tid < 0) { break; }                            /* kein Task-Slot -> weniger Worker */
        task_set_user_aspace(tid, /*user_phys=*/0, ttbr);  /* geteilter Aspace, kein Free beim Exit */
        sched_set_cred(tid, uid, caps);
        task_admit(tid);                                   /* publiziert ttbr/cred + weckt Ziel-Kern */
        spawned++;
    }
    g_nworkers = spawned;
    return spawned;
}

void vi_par_join(void)
{
    for (int i = 0; i < g_nworkers; i++) { sem_wait(&g_barrier); }
    g_nworkers = 0;
    dmb_sy();                  /* Acquire: die Worker-Schreibzugriffe (Ergebnis) sichtbar machen,
                                * bevor der Aufrufer sie in EL0 liest */
}

void vi_par_worker_done(void)
{
    dsb_sy();                  /* Release: die EL0-Compute-Schreibzugriffe des Workers global
                                * sichtbar machen, BEVOR die Fertigmeldung erfolgt */
    sem_post(&g_barrier);      /* Fertigmeldung an den (blockierten) Aufrufer-Kern */
    task_exit(0);              /* Co-Thread beenden; user_phys=0 -> kein Aspace-Free */
    __builtin_unreachable();
}

/* A4.1-Seam (siehe vi_parallel.h). QEMU raspi4b emuliert kein UVC-Video-Geraet -> es gibt
 * nichts zu enumerieren -> -1. Der echte UVC-Klassentreiber (VideoControl/VideoStreaming-
 * Deskriptoren, PROBE/COMMIT-Aushandlung, isochroner/bulk Frame-Transfer ueber drivers/usb)
 * wird am Pi4 hier eingehaengt und liefert dann die Frame-Bytes. */
int vi_cam_grab(uint64_t user_buf, unsigned long max)
{
    /* A4.1b: der dwc2-UVC-HW-Glue liefert einen YUYV-Frame, wenn eine Bulk-UVC-Kamera enumeriert
     * wurde (g_dev_kind==4). In QEMU gibt es kein UVC-Geraet -> dwc2_uvc_grab liefert sofort -1,
     * der EL0-Aufrufer faellt auf ein Datei-Frame zurueck (A5-Loop). */
    return dwc2_uvc_grab(user_buf, max);
}

#endif /* VISION */
