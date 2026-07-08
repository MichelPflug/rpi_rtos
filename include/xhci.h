/*
 * include/xhci.h  --  Generischer xHCI-1.1-Host-Controller
 */
#ifndef RPI_RTOS_XHCI_H
#define RPI_RTOS_XHCI_H

#include <stdint.h>

/* Initialisiert den Controller an 'mmio_base' (HCRST, DCBAA/Command-/Event-Ring, R/S).
 * 0 = ok, <0 = Fehler (Codes < 0 sind Phasen-spezifisch, siehe xhci.c). */
int xhci_init(uint64_t mmio_base);

/* Enumeriert das an einem Root-Port angeschlossene Gerät: Port-Reset -> Enable-Slot ->
 * Address-Device -> GET_DESCRIPTOR(device). Schreibt VID/PID nach vid und pid. 0 = ok. */
int xhci_enumerate(uint16_t *vid, uint16_t *pid);

/* --- Weitere Transfers (Folge-Increment: Bulk-Klassentransfers, MSC) --- */

/* GET_DESCRIPTOR (Control-IN) von Typ/Index in buf (len Byte). Completion-Code (1 = ok). */
int xhci_get_descriptor(uint8_t type, uint8_t index, void *buf, uint16_t len);

/* SET_CONFIGURATION (Control-OUT). Completion-Code. */
int xhci_set_config(uint8_t cfg);

/* Nach xhci_enumerate mit einem HUB: Hub konfigurieren + Downstream-Ports powern/scannen.
 * Gibt bNbrPorts (>0) oder <0. Diagnose fuer die Multi-Tier-Enumeration (Maus/Tastatur am Pi4-VL805). */
int xhci_hub_scan(void);

/* Multi-HID (Maus + Tastatur gleichzeitig hinter dem Hub): ZUERST alle Hub-Ports reseten (g_slot muss
 * dabei der Hub sein), DANN die Downstream-Geraete sequenziell enumerieren + je einen Interrupt-EP
 * einrichten, dann beide primen und per Slot-Demux pollen. */
int xhci_hub_port_reset(uint8_t hub_port);                         /* Port reset -> Speed (1/2/3) oder <0 */
int xhci_hub_enum_device(int idx, uint8_t hub_port, uint32_t dspeed, uint16_t *vid, uint16_t *pid);  /* 0 = ok */
int xhci_hid_setup(int idx);                                      /* Interrupt-IN-EP fuer Geraet idx; DCI oder <0 */
void xhci_hid_prime(int idx);                                     /* einen ausstehenden Transfer fuer idx anlegen */
int  xhci_hid_wait(int nhid, uint8_t *out, uint32_t outlen, uint32_t ms);  /* Report -> Geraet-Index (>=0) / -1 */

/* Bulk-IN- (in_dci) + Bulk-OUT-Endpoint (out_dci) am adressierten Slot einrichten
 * (Configure Endpoint). DCI = 2*EP-Nummer + (IN ? 1 : 0). 0 = ok. */
int xhci_config_bulk(uint8_t in_dci, uint16_t in_mps, uint8_t out_dci, uint16_t out_mps);

/* Ein Bulk-Transfer auf der Endpoint-DCI (buf in identity-NC-RAM). Completion-Code (1 = ok). */
int xhci_bulk(uint8_t dci, void *buf, uint32_t len);

/* Diagnose nach xhci_enumerate: Slot-ID, Port-Nummer, Port-Speed (xHCI-Speed-ID). */
uint32_t xhci_last_slot(void);
uint32_t xhci_last_port(void);
uint32_t xhci_last_speed(void);

/* HCD-vtable (usb_hc) fuer xHCI: nach xhci_config_bulk via usb_hc_register(xhci_hc_ops())
 * aktivieren -> die geteilten Klassen-Treiber usbmsc laufen dann ueber xHCI. */
struct usb_hc_ops;
const struct usb_hc_ops *xhci_hc_ops(void);

#endif /* RPI_RTOS_XHCI_H */
