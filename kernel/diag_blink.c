/*
 * kernel/diag_blink.c  --  Blind-Boot-Diagnose ueber gelatchte Header-GPIOs. Ganz #ifdef DIAG_BLINK.
 *
 * DIREKTE GPIO-Register (BCM2711 @ 0xFE200000), keine Mailbox/GIC/Timer-Abhaengigkeit -> ein Latch
 * funktioniert an JEDEM Boot-Meilenstein, auch vor der MMU. Der Pin bleibt HIGH -> statisch mit dem
 * Multimeter ablesbar (kein Zaehlen/Timing).
 */
#ifdef DIAG_BLINK

#include <stdint.h>
#include "mmio.h"
#include "diag_blink.h"
#include "sched.h"

#define GPIO_BASE   0xFE200000UL
#define GPSET0      (GPIO_BASE + 0x1CUL)   /* GPIO 0..31 auf HIGH */
#define GPCLR0      (GPIO_BASE + 0x28UL)   /* GPIO 0..31 auf LOW */
#define HB_PIN      21u                    /* Heartbeat: GPIO21 = Pin 40 */

/* GPIO 'pin' als Output konfigurieren (GPFSEL) und HIGH treiben (GPSET0). Gilt fuer pin 0..27. */
void diag_latch(unsigned pin)
{
    uintptr_t reg = (uintptr_t)GPIO_BASE + (uintptr_t)(pin / 10u) * 4u;   /* GPFSEL0/1/2 */
    uint32_t  sh  = (pin % 10u) * 3u;
    uint32_t  v   = mmio_read32(reg);
    v &= ~(7u << sh);
    v |=  (1u << sh);                       /* 001 = Output */
    mmio_write32(reg, v);
    mmio_write32(GPSET0, 1u << pin);        /* dauerhaft HIGH latchen */
}

void diag_heartbeat_task(void *arg)
{
    (void)arg;
    uintptr_t reg = (uintptr_t)GPIO_BASE + (uintptr_t)(HB_PIN / 10u) * 4u;
    uint32_t  sh  = (HB_PIN % 10u) * 3u;
    uint32_t  v   = mmio_read32(reg);
    v &= ~(7u << sh); v |= (1u << sh);
    mmio_write32(reg, v);
    for (;;) {                               /* ~0,5 s Takt -> Multimeter schwingt sichtbar */
        mmio_write32(GPSET0, 1u << HB_PIN); task_sleep_ticks(50);
        mmio_write32(GPCLR0, 1u << HB_PIN); task_sleep_ticks(50);
    }
}

#endif /* DIAG_BLINK */
