/*
 * user/lib/ulib.h  --  Geteilte EL0-Hilfen fuer rpi_rtos-User-Apps (T2.2).
 *
 * Bisher kopierte jede App ihren eigenen `sys3`-SVC-Wrapper; hier steht er EINMAL (static inline,
 * zustandslos -> kein eigenes .o noetig). GUI-Apps ziehen zusaetzlich user/lib/gui.h.
 */
#ifndef RPI_RTOS_ULIB_H
#define RPI_RTOS_ULIB_H

#include "abi.h"

/* Syscall mit bis zu 3 Argumenten (x8=Nr, x0..x2=Args, x0=Ret). */
static inline long sys3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static inline long sys1(long n, long a) { return sys3(n, a, 0, 0); }
static inline long sys0(long n)         { return sys3(n, 0, 0, 0); }

#endif /* RPI_RTOS_ULIB_H */
