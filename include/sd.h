/*
 * include/sd.h  --  EMMC2/SDHCI-SD-Kartentreiber (BCM2711)
 */
#ifndef RPI_RTOS_SD_H
#define RPI_RTOS_SD_H

#include <stdint.h>

/* Initialisiert Controller + Karte. 0 = ok, <0 = Fehler. */
int sd_init(void);

/* Liest/schreibt genau einen 512-Byte-Block (LBA). 0 = ok, <0 = Fehler. */
int sd_read_block(uint64_t lba, void *buf);
int sd_write_block(uint64_t lba, const void *buf);

#endif /* RPI_RTOS_SD_H */
