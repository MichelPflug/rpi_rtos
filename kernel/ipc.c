/*
 * kernel/ipc.c  --  Semaphore & Mutex mit Priority-Inheritance
 *
 * Baut auf den Scheduler-Primitiven sched_block_on/sched_wake_one auf. Alle
 * Operationen laufen in einem kritischen Abschnitt (IRQs maskiert), der
 * vorherige DAIF-Zustand wird gesichert/wiederhergestellt -> auch aus bereits
 * maskiertem Kontext sicher aufrufbar.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "sched.h"
#include "timer.h"
#include "exceptions.h"
#include "ipc.h"

/* ---------------- Semaphor ---------------- */

void sem_init(semaphore_t *s, int initial)
{
    s->count = initial;
}

/* Blockiert bis count > 0 ODER die Deadline abgelaufen ist. timeout_ticks == 0 -> unbegrenzt.
 * Rueckgabe: 0 = erworben (count dekrementiert), -1 = Timeout (nichts erworben). */
int sem_wait_timeout(semaphore_t *s, uint64_t timeout_ticks)
{
    uint64_t flags = READ_SYSREG(daif);
    irq_disable();
    sched_ipc_lock();
    uint64_t deadline = timeout_ticks ? (timer_ticks() + timeout_ticks) : 0;
    int ret = 0;
    while (s->count <= 0) {
        if (deadline && timer_ticks() >= deadline) {
            ret = -1;               /* Deadline schon erreicht -> nicht (mehr) blockieren */
            break;
        }
        int to = sched_block_on_timeout(s, deadline);   /* gibt den Lock frei; neu pruefen */
        if (to && s->count <= 0) {
            ret = -1;               /* per Timeout geweckt und kein post kam dazwischen */
            break;
        }
    }
    if (ret == 0) {
        s->count--;
    }
    sched_ipc_unlock();
    WRITE_SYSREG(daif, flags);
    return ret;
}

void sem_wait(semaphore_t *s)
{
    (void)sem_wait_timeout(s, 0);   /* 0 = unbegrenzt */
}

void sem_post(semaphore_t *s)
{
    uint64_t flags = READ_SYSREG(daif);
    irq_disable();
    sched_ipc_lock();
    s->count++;
    int w = sched_wake_one(s);      /* hoechstprioren Warter wecken (cross-core moeglich) */
    sched_ipc_unlock();
    if (w >= 0) {
        sched_reschedule();         /* ggf. sofort einplanen -- AUSSERHALB des IPC-Locks
                                     * (sonst koennte der eingeplante Warter ihn nicht nehmen) */
    }
    WRITE_SYSREG(daif, flags);
}

/* ---------------- Mutex (Priority-Inheritance) ---------------- */

void mutex_init(mutex_t *m)
{
    m->locked          = 0;
    m->owner_tid       = -1;
    m->owner_orig_prio = 0;
}

/* Sperrt den Mutex, optional mit Timeout. timeout_ticks == 0 -> unbegrenzt.
 * Rueckgabe: 0 = erworben (Owner), -1 = Timeout (nicht erworben). */
int mutex_lock_timeout(mutex_t *m, uint64_t timeout_ticks)
{
    uint64_t flags = READ_SYSREG(daif);
    irq_disable();
    sched_ipc_lock();

    int me  = sched_current_tid();
    int ret = 0;
    if (!m->locked) {
        m->locked          = 1;
        m->owner_tid       = me;
        m->owner_orig_prio = sched_get_prio(me);
    } else if (m->owner_tid == me) {
        /* Rekursives/doppeltes Lock auf einem einstufigen Mutex ist ein KERNEL-BUG.
         * Frueher stiller No-op -> der Aufrufer glaubte, erneut erworben zu haben, und das
         * FOLGENDE mutex_unlock gab den noch benutzten kritischen Abschnitt frei (Silent-
         * Corruption/Double-Unlock). Fail-loud: hart brechen statt leise korrumpieren. */
        panic("rekursives mutex_lock (einstufiges Design) -- Selbst-Deadlock/Korruption vermieden");
    } else {
        /* Priority-Inheritance: ist der Warter hoeherprior (kleinere Zahl) als
         * der aktuelle Owner, hebe den Owner auf die Warter-Prioritaet an,
         * damit er nicht von mittelprioren Tasks verzoegert wird.
         * Grenze (bewusst, Tier-2/transitive-PI): laeuft der Warter per TIMEOUT ab, wird der
         * Boost NICHT zurueckgenommen -- der Owner behaelt die erhoehte Prio bis zum unlock
         * (dort auf owner_orig_prio zurueckgesetzt). Fuer einstufige PI vertretbar. */
        uint8_t myp = sched_get_prio(me);
        if (myp < sched_get_prio(m->owner_tid)) {
            uart_puts("[ipc] PI: boost tid=");
            uart_putdec(m->owner_tid);
            uart_puts(" auf Prio ");
            uart_putdec(myp);
            uart_puts("\n");
            sched_set_prio(m->owner_tid, myp);
        }
        /* Warten, bis uns der Unlock die Ownership uebergibt (owner_tid == me) oder die
         * Deadline abgelaufen ist. Timeout und Ownership-Handoff schliessen sich unter
         * s_ipclock gegenseitig aus -> bei Timeout ist owner_tid garantiert != me. */
        uint64_t deadline = timeout_ticks ? (timer_ticks() + timeout_ticks) : 0;
        while (m->owner_tid != me) {
            if (deadline && timer_ticks() >= deadline) {
                ret = -1;
                break;
            }
            int to = sched_block_on_timeout(m, deadline);
            if (to && m->owner_tid != me) {
                ret = -1;
                break;
            }
        }
    }

    sched_ipc_unlock();
    WRITE_SYSREG(daif, flags);
    return ret;
}

void mutex_lock(mutex_t *m)
{
    (void)mutex_lock_timeout(m, 0);   /* 0 = unbegrenzt */
}

void mutex_unlock(mutex_t *m)
{
    uint64_t flags = READ_SYSREG(daif);
    irq_disable();
    sched_ipc_lock();

    int me = sched_current_tid();
    /* Nur der Owner eines gesperrten Mutex darf entsperren (sonst Korruption
     * von Lock-Zustand und Prioritaet). */
    if (!m->locked || m->owner_tid != me) {
        uart_puts("[ipc] WARN: mutex_unlock durch Nicht-Owner ignoriert\n");
        sched_ipc_unlock();
        WRITE_SYSREG(daif, flags);
        return;
    }

    /* Eigene (evtl. geboostete) Prioritaet auf den Ausgangswert zuruecksetzen. */
    sched_set_prio(me, m->owner_orig_prio);

    int w = sched_wake_one(m);     /* hoechstprioren Warter (= neuer Owner) wecken */
    if (w >= 0) {
        m->owner_tid       = w;    /* Ownership-Handoff; locked bleibt 1 */
        m->owner_orig_prio = sched_get_prio(w);
    } else {
        m->locked    = 0;
        m->owner_tid = -1;
    }

    sched_ipc_unlock();
    if (w >= 0) {
        sched_reschedule();        /* neuen Owner einplanen -- AUSSERHALB des IPC-Locks */
    }
    WRITE_SYSREG(daif, flags);
}
