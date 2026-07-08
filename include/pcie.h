/*
 * include/pcie.h  --  BCM2711 PCIe / VL805-xHCI Bring-up (Pi4 USB-A-Ports). 
 */
#ifndef RPI_RTOS_PCIE_H
#define RPI_RTOS_PCIE_H
#ifdef PCIE_PROBE

void pcie_probe(void);           /* liest PCIe-RC-Register + VL805-Config, gibt Diagnose auf die Konsole (boot-sicher) */
void pcie_fixup_selftest(void);  /* beweist den Abort-Fixup (laeuft auch in QEMU) vor dem echten Bringup */
void pcie_xhci_task(void *arg);  /* on-demand VL805-xHCI-MMIO-Probe (Trigger hdd1:XHCIGO.FLG, Watchdog-gesichert) */
/* xHCI-Maus-Zustand (GUI-Sitzung): Cursor-Position + Buttons, sobald der Maus-Poll-Task sie geaendert
 * hat. Von gui_input_tick (Timer-IRQ, single-producer) gelesen. 1 = geaendert (+ *x/*y/*btn gesetzt). */
int xhci_mouse_get(int *x, int *y, int *btn);

#endif /* PCIE_PROBE */
#endif /* RPI_RTOS_PCIE_H */
