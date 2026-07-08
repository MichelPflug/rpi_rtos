/*
 * include/spinlock.h  --  Einfacher SMP-Spinlock (AArch64, LDAXR/STXR + WFE/SEV)
 *
 * Test-and-set mit Acquire-/Release-Semantik. Setzt voraus, dass die Lock-Variable
 * in Normal-cacheable-Speicher liegt (Exclusives funktionieren nur dort) -> der Kern
 * muss MMU + Caches aktiviert haben. Auf einem Single-Core-Pfad weiterhin korrekt
 * (das Lock ist dann unkontestiert), aber dort genuegt meist IRQ-Masking.
 */
#ifndef RPI_RTOS_SPINLOCK_H
#define RPI_RTOS_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *s)
{
    uint32_t tmp;
    __asm__ volatile(
        "       sevl\n"                 /* erstes wfe nicht blockieren */
        "1:     wfe\n"
        "2:     ldaxr   %w0, [%1]\n"     /* aktuellen Wert (Acquire) lesen */
        "       cbnz    %w0, 1b\n"       /* gehalten -> warten */
        "       stxr    %w0, %w2, [%1]\n"/* 1 schreiben (exklusiv) */
        "       cbnz    %w0, 2b\n"       /* verloren -> erneut versuchen */
        : "=&r"(tmp)
        : "r"(&s->lock), "r"(1u)
        : "memory");
}

static inline void spin_unlock(spinlock_t *s)
{
    __asm__ volatile("stlr  wzr, [%0]" :: "r"(&s->lock) : "memory");  /* 0 (Release) */
    __asm__ volatile("sev"             ::: "memory");                  /* Warter wecken */
}

/* Nicht-blockierender Versuch: 1 = Lock bekommen, 0 = besetzt (oder Exclusive verloren).
 * Fuer IRQ-Kontext, der NIE blockieren darf (z.B. der Cursor-Mover im Timer-IRQ). */
static inline int spin_trylock(spinlock_t *s)
{
    uint32_t prev, res;
    __asm__ volatile(
        "       ldaxr   %w0, [%2]\n"      /* aktuellen Wert (Acquire) lesen */
        "       cbnz    %w0, 1f\n"        /* schon gehalten -> Fehlschlag */
        "       stxr    %w1, %w3, [%2]\n" /* 1 schreiben; res=0 bei Erfolg */
        "       b       2f\n"
        "1:     mov     %w1, #1\n"        /* besetzt -> res=1 */
        "2:\n"
        : "=&r"(prev), "=&r"(res)
        : "r"(&s->lock), "r"(1u)
        : "memory");
    return res == 0;
}

#endif /* RPI_RTOS_SPINLOCK_H */
