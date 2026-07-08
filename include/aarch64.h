/*
 * include/aarch64.h  --  AArch64-Systemregister- und Barrier-Helfer
 */
#ifndef RPI_RTOS_AARCH64_H
#define RPI_RTOS_AARCH64_H

#include <stdint.h>

/* Systemregister lesen/schreiben (GNU statement expression). */
#define READ_SYSREG(reg) ({                          \
    uint64_t __v;                                     \
    __asm__ volatile("mrs %0, " #reg : "=r"(__v));    \
    __v;                                              \
})

#define WRITE_SYSREG(reg, val)                                                \
    __asm__ volatile("msr " #reg ", %0" :: "r"((uint64_t)(val)) : "memory")

static inline void isb(void)    { __asm__ volatile("isb" ::: "memory"); }
static inline void dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void dmb_sy(void) { __asm__ volatile("dmb sy" ::: "memory"); }

static inline void irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }

static inline void wfi(void) { __asm__ volatile("wfi" ::: "memory"); }
static inline void wfe(void) { __asm__ volatile("wfe" ::: "memory"); }
static inline void sev(void) { __asm__ volatile("sev" ::: "memory"); }

static inline uint64_t current_el(void) { return READ_SYSREG(CurrentEL) >> 2; }

/* Kern-ID (MPIDR_EL1.Aff0) -- 0..3 auf dem BCM2711-Cluster. */
static inline uint32_t cpu_id(void) { return (uint32_t)(READ_SYSREG(mpidr_el1) & 0xff); }

#endif /* RPI_RTOS_AARCH64_H */
