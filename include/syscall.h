/*
 * include/syscall.h  --  Syscall-Dispatch (aus dem EL0-Sync-Handler)
 */
#ifndef RPI_RTOS_SYSCALL_H
#define RPI_RTOS_SYSCALL_H

#include <stdint.h>

/* frame zeigt auf den Trap-Frame (frame[0..30] = x0..x30). Liest Syscall-Nummer
 * (x8) und Argumente (x0..), schreibt den Rueckgabewert nach frame[0] (x0). */
void syscall_dispatch(uint64_t *frame);

#ifdef RTOS_SELFTEST
/* Sanity-Check der AT-S1E0-User-Access-Pruefung (1 = AT lehnt inaccessible korrekt ab). */
int uaccess_selftest(void);
#endif

#endif /* RPI_RTOS_SYSCALL_H */
