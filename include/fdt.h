/*
 * include/fdt.h  --  Minimaler Flattened-Device-Tree-Leser (nur /memory)
 *
 * Der Raspberry-Pi-Firmware-Boot uebergibt dem Kernel einen DTB-Zeiger in x0 (start.S
 * rettet ihn nach g_dtb_ptr). Fuer T1.12 (voller RAM-Ausbau) brauchen wir die WAHRE
 * Gesamt-RAM-Groesse -- auf 4/8-GB-Pi-4 steht sie NUR im DTB /memory (die Mailbox-
 * "Get ARM memory" ist 32-bit und meldet nur die Low-Split-Region). Dieser Leser
 * extrahiert die (base,size)-Regionen aus /memory; er ist bewusst klein + streng
 * bounds-gepueft (Firmware-Eingabe). QEMU raspi4b uebergibt KEINEN DTB -> fdt_get_memory
 * liefert 0 (der MMU-Aufbau nutzt dann die konservative Grobkarte).
 */
#ifndef RPI_RTOS_FDT_H
#define RPI_RTOS_FDT_H

#include <stdint.h>

#define FDT_MAGIC 0xd00dfeedu

typedef struct {
    uint64_t base;
    uint64_t size;
} fdt_mem_region_t;

/* Liest die RAM-Regionen aus /memory des DTB an 'dtb' (Identity-/PA-Zeiger, MMU-agnostisch).
 * Schreibt bis zu 'max' Regionen nach out[], liefert die Anzahl (0 = kein/ungueltiger DTB
 * oder kein /memory). Validiert Magic + Gesamtgroesse + alle Offsets/Laengen. */
int fdt_get_memory(uint64_t dtb, fdt_mem_region_t *out, int max);

#endif /* RPI_RTOS_FDT_H */
