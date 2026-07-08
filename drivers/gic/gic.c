/*
 * drivers/gic/gic.c  --  ARM GIC-400 (GICv2) fuer BCM2711
 *
 * Distributor (GICD) @ 0xFF841000, CPU-Interface (GICC) @ 0xFF842000.
 * Wir konfigurieren die Interrupts als Gruppe 1 (Non-secure) und aktivieren
 * Distributor + CPU-Interface. SGIs/PPIs sind per-CPU (kein ITARGETSR noetig).
 *
 * > Zu verifizieren: Die Gruppen-/CTLR-Konfiguration haengt davon ab, ob die
 *   Ausfuehrung secure oder non-secure ist. In QEMU raspi4b empirisch getestet.
 */
#include <stdint.h>
#include "mmio.h"
#include "aarch64.h"
#include "gic.h"

#define GICD_BASE   0xFF841000UL
#define GICC_BASE   0xFF842000UL

#define GICD_CTLR        (GICD_BASE + 0x000)
#define GICD_IGROUPR     (GICD_BASE + 0x080)   /* +4*n */
#define GICD_ISENABLER   (GICD_BASE + 0x100)   /* +4*n */
#define GICD_IPRIORITYR  (GICD_BASE + 0x400)   /* +intid (byte) */
#define GICD_ITARGETSR   (GICD_BASE + 0x800)   /* +intid (byte) */
#define GICD_SGIR        (GICD_BASE + 0xF00)   /* Software-Generated-Interrupt-Register */

#define GICC_CTLR   (GICC_BASE + 0x000)
#define GICC_PMR    (GICC_BASE + 0x004)
#define GICC_IAR    (GICC_BASE + 0x00C)
#define GICC_EOIR   (GICC_BASE + 0x010)

/* Per-core CPU-Interface (GICC) + banked SGI/PPI-Gruppe. In GICv2 sind GICC sowie
 * IGROUPR0/IPRIORITYR/ISENABLER fuer INTID 0..31 PRO KERN gebankt -> jeder Kern muss
 * dies fuer sich aufrufen. */
void gic_init_cpu(void)
{
    /* SGIs/PPIs (INTID 0..31) dieses Kerns in Gruppe 1 legen (banked). */
    mmio_write32(GICD_IGROUPR + 0, 0xFFFFFFFF);

    /* Hinweis: SGIs (INTID 0..15, u.a. SGI_RESCHED) brauchen KEIN ISENABLER. Im GIC-400
     * (BCM2711) ist GICD_ISENABLER0 = 0x0000FFFF (Reset), die SGI-Bits sind RAO/WI ->
     * SGIs sind dauerhaft freigegeben (ARM DDI 0471B, Tab. 3-2). Nur PPIs/SPIs (Bits >=16,
     * Reset 0) muessen via gic_enable_irq scharfgeschaltet werden. */

    /* CPU-Interface: alle Prioritaeten zulassen.
     * GICC_CTLR = EnableGrp0 | EnableGrp1 | AckCtl(Bit2) (AckCtl: im Secure-Fall/QEMU
     * noetig, um Gruppe-1-IRQs via GICC_IAR zu acknowledgen; auf echter HW reserviert). */
    mmio_write32(GICC_PMR, 0xF0);
    mmio_write32(GICC_CTLR, 0x7);
    dsb_sy();
}

void gic_init(void)
{
    /* Distributor (shared) waehrend der Konfiguration aus. */
    mmio_write32(GICD_CTLR, 0);

    /* Distributor: EnableGrp0 | EnableGrp1. */
    mmio_write32(GICD_CTLR, 0x3);

    /* Eigenes (Core-0-)CPU-Interface initialisieren. */
    gic_init_cpu();
}

void gic_enable_irq(uint32_t intid, uint8_t priority)
{
    /* Interrupt in Gruppe 1 (Non-secure/IRQ) legen. gic_init setzt nur IGROUPR0
     * (SGIs/PPIs); SPIs >= 32 liegen in hoeheren IGROUPR-Woertern und blieben sonst
     * in Gruppe 0 -> in QEMU (secure) nicht als IRQ an c_irq_handler zugestellt. */
    uint32_t greg = GICD_IGROUPR + (intid / 32) * 4;
    mmio_write32(greg, mmio_read32(greg) | (1u << (intid % 32)));

    /* Prioritaet (byte-adressierbar). */
    mmio_write8(GICD_IPRIORITYR + intid, priority);

    /* Ziel-CPU = CPU0 (nur fuer SPIs >= 32 relevant; PPIs sind per-CPU). */
    if (intid >= 32) {
        mmio_write8(GICD_ITARGETSR + intid, 0x01);
    }

    /* Interrupt freigeben (ISENABLER, 32 IRQs pro Wort). */
    mmio_write32(GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));

    /* GIC-Konfig abschliessen, bevor der Aufrufer den Timer/die IRQs scharf schaltet
     * (ordnet die Device-Writes gegen die folgenden System-Register-Writes). */
    dsb_sy();
}

void gic_send_sgi(uint32_t sgi_id, uint32_t target_core)
{
    /* dsb: vorher geschriebene Shared-Daten (need_resched/Task-State) sichtbar machen,
     * BEVOR der Ziel-Kern per IPI aufwacht und sie liest. GICD_SGIR:
     *   [25:24] TargetListFilter = 00 (CPUTargetList nutzen)
     *   [23:16] CPUTargetList    = Bit des Zielkerns
     *   [15]    NSATT            = 1: SGI in Gruppe 1 (Non-secure) erzeugen
     *   [3:0]   SGIINTID
     * NSATT MUSS gesetzt sein: unter QEMU laeuft der Kernel secure, PPIs/SGIs liegen
     * via IGROUPR0=0xFFFFFFFF in Gruppe 1 und werden so (wie der Timer-PPI) per GICC_IAR
     * acknowledged. Ohne NSATT erzeugt der Distributor einen Gruppe-0-SGI; dessen Ack/EOI-
     * Prioritaetsabsenkung passt dann nicht und der Ziel-Kern bleibt mit erhoehter Running-
     * Priority haengen (Timer-IRQ maskiert -> Kern eingefroren). */
    dsb_sy();
    mmio_write32(GICD_SGIR,
                 (1u << 15) | ((1u << (target_core & 0x7)) << 16) | (sgi_id & 0xF));
}

uint32_t gic_acknowledge_irq(void)
{
    return mmio_read32(GICC_IAR);
}

void gic_end_irq(uint32_t iar)
{
    mmio_write32(GICC_EOIR, iar);
    dsb_sy();   /* EOI-Completion vor Stackwechsel/IRQ-Freigabe garantieren */
}
