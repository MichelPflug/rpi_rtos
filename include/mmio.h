/*
 * include/mmio.h  --  Memory-Mapped-I/O-Zugriffe (32-bit)
 *
 * Alle Peripherie-Register des BCM2711 werden ueber volatile MMIO angesprochen.
 * Die Peripherie ist als Device-nGnRnE (identitaetsgemappt) eingerichtet -- ob
 * MMU an oder aus, der Speichertyp ist derselbe. Zugriffe sind INNERHALB
 * derselben Peripherie streng geordnet; beim Wechsel ZWISCHEN Peripherien auf
 * echter HW ggf. eine Barriere (dsb/dmb) setzen (vgl. BCM2711-Datenblatt 1.3).
 */
#ifndef RPI_RTOS_MMIO_H
#define RPI_RTOS_MMIO_H

#include <stdint.h>

static inline void mmio_write32(uintptr_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write8(uintptr_t addr, uint8_t value)
{
    *(volatile uint8_t *)addr = value;
}

static inline uint8_t mmio_read8(uintptr_t addr)
{
    return *(volatile uint8_t *)addr;
}

#endif /* RPI_RTOS_MMIO_H */
