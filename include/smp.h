/*
 * include/smp.h  --  SMP-Bring-up (sekundaere Cortex-A72-Kerne)
 */
#ifndef RPI_RTOS_SMP_H
#define RPI_RTOS_SMP_H

#include <stdint.h>

/* Einsprung der Sekundaerkerne aus start.S (eigener Stack, MMU noch aus). */
void secondary_main(uint64_t core_id);

/* Von Core 0 (kmain) aufgerufen: Sekundaerkerne freigeben (Per-Core-MMU/GIC/Timer),
 * Spinlock-Lasttest + Per-Core-Timer-IRQ-Probe; gibt Statuszeilen aus. */
void smp_init_secondaries(void);

/* SMP-Scheduler-Demo (Stufe 2): je einen Kernel-Worker mit Affinitaet zu Kern 1..3
 * + einen Reporter auf Kern 0 anlegen (nach sched_init, vor sched_start). */
void smp_sched_demo_create(void);

/* Sekundaerkerne in ihren Scheduler freigeben (nach sched_init + Task-Anlage,
 * unmittelbar vor sched_start auf Kern 0). */
void smp_sched_release(void);

#endif /* RPI_RTOS_SMP_H */
