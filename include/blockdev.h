/*
 * include/blockdev.h  --  Block-Device-Abstraktion (512-Byte-Bloecke)
 *
 * Entkoppelt die Dateisysteme vom konkreten Treiber (SD/EMMC2 jetzt, spaeter
 * z.B. USB-Massenspeicher).
 */
#ifndef RPI_RTOS_BLOCKDEV_H
#define RPI_RTOS_BLOCKDEV_H

#include <stdint.h>

typedef struct blockdev {
    /* Liest/schreibt genau einen 512-Byte-Block (LBA). 0 = ok, <0 = Fehler.
     * write_block darf 0 sein (read-only Geraet). */
    int (*read_block)(struct blockdev *bd, uint64_t lba, void *buf);
    int (*write_block)(struct blockdev *bd, uint64_t lba, const void *buf);
    void *ctx;
} blockdev_t;

#endif /* RPI_RTOS_BLOCKDEV_H */
