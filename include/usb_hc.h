/*
 * include/usb_hc.h  --  USB-Host-Controller-Abstraktion (HCD-vtable)
 *
 * Entkoppelt die KLASSEN-Treiber (usbmsc = Bulk-Only-Massenspeicher, usbkbd = HID-Boot-
 * Tastatur) vom konkreten Host-Controller. So laufen dieselben Klassen-Treiber ueber
 *   - DWC2  (BCM2711 on-board USB, raspi-Build)          -> drivers/usb/dwc2.c
 *   - xHCI  (VL805/qemu-xhci hinter PCIe, virt-Build)     -> drivers/usb/xhci.c
 * Der aktive HC registriert seine ops per usb_hc_register(); die Klassen-Treiber rufen
 * ueber usb_hc(). Die Transfer-Semantik (Rueckgabewerte) ist an den bestehenden
 * DWC2-Vertrag gepinnt, damit usbmsc/usbkbd unveraendert weiterlaufen.
 */
#ifndef RPI_RTOS_USB_HC_H
#define RPI_RTOS_USB_HC_H

#include <stdint.h>

typedef struct usb_hc_ops {
    const char *name;                 /* "dwc2" / "xhci" (Diagnose) */

    /* --- Bulk-Only-Massenspeicher (usbmsc) --- */
    /* Ein Bulk-Transfer auf den Massenspeicher-Endpoints. dir: 0 = OUT, 1 = IN.
     * Liefert die uebertragenen Bytes, -2 = Endpoint-STALL (Aufrufer kann clear_halt
     * + weiterlesen), <0 (sonst) = harter Fehler. */
    int  (*bulk)(int dir, void *buf, int len);
    /* Endpoint-Halt einer Richtung loeschen (STALL-Recovery). */
    void (*clear_halt)(int dir);
    /* Bulk-Only-Mass-Storage-Reset (Desync/Phase-Error-Recovery: beide Toggles -> DATA0). */
    void (*bot_reset)(void);

    /* --- HID-Boot-Tastatur (usbkbd), optional (NULL = vom HC nicht unterstuetzt) --- */
    /* Fertigen 8-Byte-Report aus dem IRQ-Ring holen: 8 = neu, 0 = nichts, <0 = Fehler. */
    int  (*kbd_irq_getreport)(uint8_t report[8]);
    /* Synchroner HW-Poll eines 8-Byte-Reports: 8 = neu, 0 = NAK, <0 = Fehler. */
    int  (*kbd_poll)(uint8_t report[8]);

    /* --- HID-Boot-Maus (usbmouse), optional (NULL = vom HC nicht unterstuetzt) --- */
    /* Fertigen Boot-Maus-Report aus dem IRQ-Ring holen (Byte0=Buttons, Byte1=dx, Byte2=dy,
     * dx/dy signed 8-bit relativ): 8 = neu, 0 = nichts, <0 = Fehler. Bei DWC2 derselbe HID-
     * Interrupt-IN-Ring wie die Tastatur (nur EIN HID-Geraet gleichzeitig). */
    int  (*mouse_getreport)(uint8_t report[8]);

    /* Art des eingerichteten Geraets: 0 = keins, 1 = HID-Tastatur, 2 = Massenspeicher, 3 = HID-Maus. */
    int  (*dev_kind)(void);
} usb_hc_ops_t;

/* Der aktive Host-Controller meldet seine ops an (idempotent; der letzte gewinnt). */
void usb_hc_register(const usb_hc_ops_t *ops);

/* Die aktiven ops (NULL, wenn kein HC registriert ist). Klassen-Treiber pruefen auf NULL. */
const usb_hc_ops_t *usb_hc(void);

#endif /* RPI_RTOS_USB_HC_H */
