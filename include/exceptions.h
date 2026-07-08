/*
 * include/exceptions.h  --  Exception-/IRQ-Behandlung
 */
#ifndef RPI_RTOS_EXCEPTIONS_H
#define RPI_RTOS_EXCEPTIONS_H

#include <stdint.h>

/* Setzt VBAR_EL1 auf die Vektortabelle. */
void exceptions_init(void);

/* Kontrolliertes System-Panik bei nicht behebbarem Kernel-Fehler: Diagnose ausgeben,
 * ALLE Kerne stoppen. Kehrt nie zurueck. */
void panic(const char *msg);

/* Aus vectors.S aufgerufen. */
void c_exception_handler(uint64_t vector_index);
void c_sync_handler(uint64_t *frame);
void c_irq_handler(void);

#endif /* RPI_RTOS_EXCEPTIONS_H */
