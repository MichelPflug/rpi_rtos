/*
 * include/usbmsc.h  --  USB-Massenspeicher (Bulk-Only-Transport + SCSI)
 */
#ifndef RPI_RTOS_USBMSC_H
#define RPI_RTOS_USBMSC_H

#include <stdint.h>

/* Initialisiert das enumerierte Bulk-Only-Massenspeichergeraet (INQUIRY +
 * READ CAPACITY) und demonstriert/verifiziert das Lesen von Sektoren. */
void usbmsc_probe(void);

/* count 512-Byte-Sektoren ab LBA lesen/schreiben. 0 = ok, <0 = Fehler. */
int  usbmsc_read(uint32_t lba, uint32_t count, void *buf);
int  usbmsc_write(uint32_t lba, uint32_t count, const void *buf);

/* Kapazitaet in 512-Byte-Sektoren (0 = nicht bereit). */
uint32_t usbmsc_sectors(void);

#endif /* RPI_RTOS_USBMSC_H */
