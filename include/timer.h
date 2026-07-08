/*
 * include/timer.h  --  ARM Generic Timer (EL1 physical timer, CNTP)
 */
#ifndef RPI_RTOS_TIMER_H
#define RPI_RTOS_TIMER_H

#include <stdint.h>

/* Non-secure EL1 physical timer -> PPI INTID 30. */
#define TIMER_PPI_INTID  30

/* Periodischen Tick mit 'hz' Hz starten (Core 0; legt das globale Intervall fest). */
void timer_init(uint32_t hz);

/* Per-Core-Timer eines Sekundaerkerns armen (nutzt das von timer_init gesetzte
 * Intervall; setzt diesem Kern eine eigene CNTP-Deadline + aktiviert PPI 30). */
void timer_init_secondary(void);

/* Aus dem IRQ-Handler bei jedem Timer-Interrupt aufgerufen (per-core deadline/Zaehler). */
void timer_irq(void);

/* Globale Tick-Anzahl (von Core 0 gefuehrt). */
uint64_t timer_ticks(void);

/* Timer-IRQs eines bestimmten Kerns (Diagnose/SMP-Probe). */
uint64_t timer_core_ticks(uint32_t cid);

#endif /* RPI_RTOS_TIMER_H */
