/*
 * boards/virt/mmu_virt.c  --  Minimaler MMU-Aufbau fuer den virt-Harness
 *
 * Flacher Identity-Map ueber 1-GiB-Bloecke (Start-Level 1, 4-KiB-Granule):
 *   0x00000000..0x3FFFFFFF : Device      (GIC, PL011 @0x09, virtio-mmio @0x0a)
 *   0x40000000..0xFFFFFFFF : Normal-NC   (RAM)
 *
 * RAM wird bewusst als Normal Non-Cacheable gemappt und die Caches bleiben aus
 * (SCTLR.C=0/I=0). Damit ist DMA der virtio-Queues ohne Cache-Pflege kohaerent
 * und unaligned-Zugriffe (Paket-Header) sind erlaubt (Normal-Memory, A=0).
 */
#include <stdint.h>
#include "aarch64.h"

#define DESC_BLOCK   0x1ULL
#define DESC_AF      (1ULL << 10)
#define ATTR_IDX(n)  ((uint64_t)(n) << 2)
#define DESC_PXN     (1ULL << 53)
#define DESC_UXN     (1ULL << 54)

#define ATTR_DEVICE  0   /* MAIR-Index 0: Device-nGnRnE */
#define ATTR_NORMAL  1   /* MAIR-Index 1: Normal Non-Cacheable */

static uint64_t l1_table[512] __attribute__((aligned(4096)));

void mmu_virt_init(void)
{
    for (int i = 0; i < 512; i++) {
        uint64_t base = (uint64_t)i << 30;   /* i * 1 GiB */
        if (i == 0) {
            /* Peripherie-GiB: Device, nicht ausfuehrbar. */
            l1_table[i] = base | ATTR_IDX(ATTR_DEVICE) | DESC_AF |
                          DESC_PXN | DESC_UXN | DESC_BLOCK;
        } else if (i <= 3) {
            /* RAM-GiBs (0x40000000..0xFFFFFFFF): Normal-NC, ausfuehrbar. */
            l1_table[i] = base | ATTR_IDX(ATTR_NORMAL) | DESC_AF | DESC_BLOCK;
        } else if (i == 256) {
            /* T1.14: PCIe-ECAM des qemu-virt (pci-host-ecam-generic) liegt bei
             * 0x40_10000000 (GiB 256) -> als Device mappen (39-bit VA deckt 512 GiB). */
            l1_table[i] = base | ATTR_IDX(ATTR_DEVICE) | DESC_AF |
                          DESC_PXN | DESC_UXN | DESC_BLOCK;
        } else {
            l1_table[i] = 0;   /* nicht gemappt */
        }
    }

    /* MAIR: Attr0 = Device-nGnRnE (0x00), Attr1 = Normal Inner+Outer NC (0x44). */
    WRITE_SYSREG(mair_el1, 0x4400ULL);

    /* TCR: T0SZ=25 (39-bit VA, Start-Level 1), TG0=4KiB, Walk NC,
     *      IPS=40-bit, EPD1=1 (TTBR1 aus). */
    WRITE_SYSREG(tcr_el1, 0x0000000200800019ULL);

    WRITE_SYSREG(ttbr0_el1, (uint64_t)(uintptr_t)l1_table);
    isb();

    __asm__ volatile("tlbi vmalle1; dsb sy; isb" ::: "memory");

    /* MMU an, Caches aus (RES1-Basis 0x30d00800 + M=1). */
    WRITE_SYSREG(sctlr_el1, 0x0000000030d00801ULL);
    isb();
}

/* Stub: der virt-Harness laeuft single-core (-smp 1), Sekundaerkerne werden nie
 * freigegeben. Vorhanden, damit das von start.S referenzierte kernel/smp.c (das
 * mmu_init_secondary() ruft) im virt-Build linkt. Wird nie ausgefuehrt. */
void mmu_init_secondary(void)
{
}
