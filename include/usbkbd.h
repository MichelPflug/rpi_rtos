/*
 * include/usbkbd.h  --  USB-HID-Boot-Tastatur -> ASCII + Konsolen-Eingabe-Multiplex
 */
#ifndef RPI_RTOS_USBKBD_H
#define RPI_RTOS_USBKBD_H

/* Nach erfolgreicher Enumeration aufrufen: ab dann liefert usbkbd_getchar()
 * Tastendruecke der USB-Tastatur. */
void usbkbd_enable(void);

/* Nicht blockierend: ASCII-Zeichen eines NEU gedrueckten Tasters oder -1. */
int usbkbd_getchar(void);

/* Naechstes Byte einer laufenden Pfeiltasten-CSI-Sequenz (ESC '[' Final) oder -1.
 * console_getc bedient das mit Vorrang vor der UART (atomare Mehrbyte-Zustellung). */
int usbkbd_seq_pending(void);

/* Blockierend: ein Zeichen von der seriellen Konsole ODER der USB-Tastatur
 * (je nachdem, was zuerst kommt). Ersetzt uart_getc fuer Login/Shell. */
char console_getc(void);

/* Non-blocking-Variante: naechstes Zeichen oder -1 (fuer den GUI-Tastatur-Poll im Timer-Tick). */
int  console_getc_nb(void);

/* Einen 8-Byte-HID-Boot-Keyboard-Report -> ASCII (Flankenerkennung, interner s_prev-Zustand).
 * allow_seq=1: Pfeiltasten als CSI. Liefert ein Zeichen oder -1. Auch vom xHCI-Keyboard-Poll genutzt. */
int usbkbd_decode(const uint8_t *rep, int allow_seq);

#ifdef PCIE_PROBE
/* Ein dekodiertes ASCII-Zeichen der lokalen xHCI-USB-Tastatur (Pi4-USB-A) in die Konsolen-Eingabe
 * einreihen -> console_getc(_nb) liefert es an die Shell. Vom xHCI-Poll-Task (pcie.c) aufgerufen. */
void usbkbd_xhci_push(int c);
#endif

#endif /* RPI_RTOS_USBKBD_H */
