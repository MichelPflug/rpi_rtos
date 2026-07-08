/*
 * include/pci.h  --  Minimaler PCIe-ECAM-Config-Zugriff (qemu virt: pci-host-ecam-generic)
 */
#ifndef RPI_RTOS_PCI_H
#define RPI_RTOS_PCI_H

#include <stdint.h>

/* Bus/Device/Function gepackt (wie Linux: bus<<8 | dev<<3 | fn). */
typedef uint32_t pci_bdf_t;
#define PCI_BDF(bus, dev, fn)  (((uint32_t)(bus) << 8) | ((uint32_t)(dev) << 3) | (uint32_t)(fn))

/* Config-Space (little-endian). */
uint32_t pci_cfg_read32(pci_bdf_t bdf, uint32_t off);
uint16_t pci_cfg_read16(pci_bdf_t bdf, uint32_t off);
void     pci_cfg_write32(pci_bdf_t bdf, uint32_t off, uint32_t val);
void     pci_cfg_write16(pci_bdf_t bdf, uint32_t off, uint16_t val);

/* Sucht das ERSTE Gerät auf Bus 0 mit Class-Code (24-bit: class<<16|subclass<<8|progif).
 * Liefert die BDF in *out und 1, sonst 0. */
int pci_find_class(uint32_t class24, pci_bdf_t *out);

/* Vergibt BAR-Index 'bar' (32- oder 64-bit MEM) die MMIO-Adresse 'addr' und aktiviert
 * MEM-Space + Bus-Master im Command-Register. Liefert die zugewiesene MMIO-Basis (0 = Fehler). */
uint64_t pci_bar_assign(pci_bdf_t bdf, int bar, uint64_t addr);

#endif /* RPI_RTOS_PCI_H */
