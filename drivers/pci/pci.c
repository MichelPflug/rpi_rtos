/*
 * drivers/pci/pci.c  --  Minimaler PCIe-ECAM-Config-Zugriff (qemu virt)
 *
 * ECAM = memory-mapped Config-Space: Adresse = BASE + (bus<<20 | dev<<15 | fn<<12 | off).
 * Nur so viel, wie T1.14 braucht (Gerät finden, BAR vergeben, MEM+Bus-Master an).
 */
#include <stdint.h>
#include "mmio.h"
#include "pci.h"

/* Aus dem qemu-virt-DTB (pcie@10000000): ECAM-Basis + 32-bit-MMIO-Fenster. */
#define PCI_ECAM_BASE   0x4010000000ULL
#define PCI_MMIO32_BASE 0x10000000ULL      /* nur zur Bounds-Dokumentation */

/* --- Config-Register-Offsets --- */
#define PCI_VENDOR_ID   0x00
#define PCI_COMMAND     0x04
#define PCI_CLASS_REV   0x08               /* [31:8] = class24, [7:0] = revision */
#define PCI_BAR0        0x10
#define PCI_CMD_MEM     (1u << 1)
#define PCI_CMD_MASTER  (1u << 2)

static uint64_t ecam_addr(pci_bdf_t bdf, uint32_t off)
{
    uint32_t bus = (bdf >> 8) & 0xff, dev = (bdf >> 3) & 0x1f, fn = bdf & 0x7;
    return PCI_ECAM_BASE + ((uint64_t)bus << 20) + ((uint64_t)dev << 15) +
           ((uint64_t)fn << 12) + off;
}

uint32_t pci_cfg_read32(pci_bdf_t bdf, uint32_t off)
{
    return mmio_read32((uintptr_t)ecam_addr(bdf, off & ~3u));
}
uint16_t pci_cfg_read16(pci_bdf_t bdf, uint32_t off)
{
    uint32_t v = pci_cfg_read32(bdf, off);
    return (uint16_t)(v >> ((off & 2u) * 8));
}
void pci_cfg_write32(pci_bdf_t bdf, uint32_t off, uint32_t val)
{
    mmio_write32((uintptr_t)ecam_addr(bdf, off & ~3u), val);
}
void pci_cfg_write16(pci_bdf_t bdf, uint32_t off, uint16_t val)
{
    /* ECHTER 16-bit-Store (ECAM erlaubt sized accesses). Ein 32-bit-Read-Modify-Write
     * wuerde das Nachbar-Wort zurueckschreiben -- z.B. beim Command-Register (0x04) das
     * RW1C-Status-Register (0x06): ein zurueckgeschriebenes gesetztes Bit loescht den
     * gelatchten Fehlerstatus. */
    *(volatile uint16_t *)(uintptr_t)ecam_addr(bdf, off & ~1u) = val;
}

int pci_find_class(uint32_t class24, pci_bdf_t *out)
{
    for (uint32_t dev = 0; dev < 32; dev++) {
        pci_bdf_t bdf = PCI_BDF(0, dev, 0);
        uint16_t vid = pci_cfg_read16(bdf, PCI_VENDOR_ID);
        if (vid == 0xFFFF || vid == 0x0000) {
            continue;                       /* Slot leer */
        }
        uint32_t cr = pci_cfg_read32(bdf, PCI_CLASS_REV);
        if ((cr >> 8) == class24) {
            if (out) { *out = bdf; }
            return 1;
        }
    }
    return 0;
}

uint64_t pci_bar_assign(pci_bdf_t bdf, int bar, uint64_t addr)
{
    uint32_t off = (uint32_t)(PCI_BAR0 + bar * 4);
    uint32_t lo = pci_cfg_read32(bdf, off);
    if (lo & 0x1u) {
        return 0;                           /* I/O-BAR, hier nicht unterstuetzt */
    }
    int is64 = ((lo >> 1) & 0x3u) == 0x2u;  /* Typ 10b = 64-bit MEM */

    /* Adresse schreiben (32- oder 64-bit) und MEM+Bus-Master aktivieren. */
    pci_cfg_write32(bdf, off, (uint32_t)(addr & 0xFFFFFFF0u) | (lo & 0xFu));
    if (is64) {
        pci_cfg_write32(bdf, off + 4, (uint32_t)(addr >> 32));
    }
    uint16_t cmd = pci_cfg_read16(bdf, PCI_COMMAND);
    pci_cfg_write16(bdf, PCI_COMMAND, cmd | PCI_CMD_MEM | PCI_CMD_MASTER);
    return addr;
}
