/*
 * include/dwc2.h  --  Synopsys DWC2 USB-2.0-OTG Host-Controller (BCM2711)
 *
 * Auf der Pi 4 sitzt der DWC2 bei 0xFE980000 (Low-Peripheral-Modus). QEMU `raspi4b`
 * emuliert ihn (GSNPSID 0x4F54294A); die 4 USB-A-Ports der echten Pi 4 haengen
 * dagegen am VL805/xHCI hinter PCIe (in QEMU NICHT emuliert). Wir betreiben den
 * DWC2 im Host-Modus per DMA. Enumeration + Massenspeicher pollen; HID-Tastatur-
 * Completion und Hot-Plug laufen IRQ-getrieben (GIC-SPI 105) -- siehe dwc2.c.
 */
#ifndef RPI_RTOS_DWC2_H
#define RPI_RTOS_DWC2_H

#include <stdint.h>

typedef enum {
    USB_SPEED_HIGH = 0,
    USB_SPEED_FULL = 1,
    USB_SPEED_LOW  = 2,
    USB_SPEED_NONE = 3,
} usb_speed_t;

/* DWC2 USB-Interrupt: GIC-SPI 105 auf BCM2711 (= VC-IRQ 9 + 96; empirisch in QEMU
 * bestaetigt). */
#define DWC2_IRQ_SPI 105

/* Controller initialisieren (Reset, Host-Modus, Port-Power). 0 = ok. */
int dwc2_init(void);

/* DWC2-Interrupt-Handler (aus c_irq_handler bei SPI 105 aufrufen). */
void dwc2_irq(void);

/* IRQ-getriebenes HID-Boot-Geraet (Tastatur ODER Maus): nach der Enumeration aktivieren; der
 * Timer-Tick (100 Hz) armt den Interrupt-IN-Kanal, der IRQ liefert fertige Reports in einen Ring. */
void dwc2_kbd_irq_enable(void);
void dwc2_kbd_tick(void);
int  dwc2_kbd_irq_getreport(uint8_t report[8]);
/* 1, sobald der HID-Interrupt-IN scharfgeschaltet ist (g_kbd_irq gesetzt) -- fuer Selbsttests. */
int  dwc2_kbd_irq_active(void);

/* Aktiviert nur den Port-IRQ (Hot-Plug-Erkennung), wenn beim Boot kein Geraet anliegt. */
void dwc2_hotplug_enable(void);

/* Anzahl erkannter Hot-Plug-Changes (Connect/Disconnect am Root-Port). */
uint32_t dwc2_hotplug_count(void);

/* Root-Port zuruecksetzen und das angeschlossene Geraet erkennen.
 * Liefert die Geschwindigkeit oder USB_SPEED_NONE, wenn nichts angeschlossen ist. */
usb_speed_t dwc2_port_reset_detect(void);

/* Geraet enumerieren (Adresse, Deskriptoren, Konfiguration) und ein unterstuetztes
 * Geraet einrichten (HID-Boot-Tastatur oder Bulk-Only-Massenspeicher). 0 = bereit. */
int dwc2_enumerate(void);

/* Art des eingerichteten Geraets: 0 = keins, 1 = HID-Tastatur, 2 = Massenspeicher, 3 = HID-Maus. */
int dwc2_dev_kind(void);

/* Bulk-Transfer auf den Massenspeicher-Endpoints. dir: 0 = OUT, 1 = IN.
 * Daten-Toggle wird je Richtung gefuehrt. Liefert uebertragene Bytes oder <0. */
int dwc2_bulk(int dir, void *buf, int len);

/* BOT-Fehler-Recovery: Endpoint-Halt einer Richtung loeschen bzw. Bulk-Only-Reset
 * des gesamten Massenspeicher-Interface (beide Toggles -> DATA0). */
void dwc2_clear_halt(int dir);
void dwc2_bot_reset(void);

/* Einen 8-Byte-HID-Boot-Report vom Interrupt-IN-Endpoint pollen.
 * Liefert 8 bei neuem Report, 0 wenn momentan nichts anliegt (NAK), <0 bei Fehler. */
int dwc2_kbd_poll(uint8_t report[8]);

#endif /* RPI_RTOS_DWC2_H */
