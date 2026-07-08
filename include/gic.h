/*
 * include/gic.h  --  ARM GIC-400 (GICv2) Treiber
 */
#ifndef RPI_RTOS_GIC_H
#define RPI_RTOS_GIC_H

#include <stdint.h>

/* Distributor + (eigenes) CPU-Interface initialisieren (Core 0). */
void gic_init(void);

/* Nur das per-core CPU-Interface initialisieren (GICC + banked SGI/PPI-Gruppe).
 * Jeder Sekundaerkern ruft dies fuer sich auf; der Distributor bleibt Core 0 ueberlassen. */
void gic_init_cpu(void);

/* Einen Interrupt (PPI/SPI) mit Prioritaet aktivieren und auf CPU0 routen. */
void gic_enable_irq(uint32_t intid, uint8_t priority);

/* CPU-Interface: IRQ annehmen (GICC_IAR) / abschliessen (GICC_EOIR). */
uint32_t gic_acknowledge_irq(void);
void     gic_end_irq(uint32_t iar);

/* Software-IRQ (SGI) an einen anderen Kern senden (Reschedule-IPI). */
#define SGI_RESCHED  0u                 /* SGI 0 = sofortiges Cross-Core-Umplanen */
#define SGI_HALT     1u                 /* SGI 1 = alle anderen Kerne dauerhaft anhalten (panic) */
void     gic_send_sgi(uint32_t sgi_id, uint32_t target_core);

#endif /* RPI_RTOS_GIC_H */
