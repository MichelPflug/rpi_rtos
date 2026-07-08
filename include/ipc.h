/*
 * include/ipc.h  --  IPC-Primitive: Semaphore & Mutex (mit Priority-Inheritance)
 */
#ifndef RPI_RTOS_IPC_H
#define RPI_RTOS_IPC_H

#include <stdint.h>

/* Zaehlendes Semaphor. */
typedef struct {
    volatile int count;
} semaphore_t;

void sem_init(semaphore_t *s, int initial);
void sem_wait(semaphore_t *s);   /* blockiert unbegrenzt, bis count > 0 */
void sem_post(semaphore_t *s);   /* erhoeht count, weckt einen Warter */

/* Wie sem_wait, aber hoechstens 'timeout_ticks' Timer-Ticks (100 Hz). Rueckgabe:
 * 0 = erworben, -1 = Timeout. timeout_ticks == 0 -> unbegrenzt (wie sem_wait). */
int  sem_wait_timeout(semaphore_t *s, uint64_t timeout_ticks);

/* Mutex mit Priority-Inheritance (gegen Prioritaetsinversion) und Ownership-
 * Handoff beim Entsperren. Einfachstufig (keine verschachtelten Mutexe). */
typedef struct {
    volatile int  locked;
    int           owner_tid;
    unsigned char owner_orig_prio;   /* Prioritaet des Owners vor evtl. Boost */
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);     /* blockiert unbegrenzt bis erworben */
void mutex_unlock(mutex_t *m);

/* Wie mutex_lock, aber hoechstens 'timeout_ticks' Timer-Ticks. Rueckgabe:
 * 0 = erworben (Owner), -1 = Timeout. timeout_ticks == 0 -> unbegrenzt (wie mutex_lock). */
int  mutex_lock_timeout(mutex_t *m, uint64_t timeout_ticks);

#endif /* RPI_RTOS_IPC_H */
