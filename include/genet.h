/*
 * include/genet.h  --  BCM2711 GENET v5 Gigabit-Ethernet (echte Pi-4-HW)
 *
 * ACHTUNG: In QEMU `raspi4b` ist GENET NICHT emuliert -- ein Zugriff auf
 * 0xFD580000 loest einen synchronen External-Abort aus. Dieser Treiber ist
 * daher ausschliesslich fuer echte Hardware gedacht und in QEMU nicht testbar
 * (nur Code-Review). Er wird im raspi4b-Build kompiliert, aber von der QEMU-Demo
 * NICHT aufgerufen.
 */
#ifndef RPI_RTOS_GENET_H
#define RPI_RTOS_GENET_H

#include "net.h"

/* Initialisiert GENET (UMAC/RBUF/RGMII/PHY/DMA) und bindet nif->transmit.
 * Liefert 0 bei Erfolg. NUR auf echter Hardware aufrufen. */
int genet_init(netif_t *nif);

/* Empfangene Frames verarbeiten (im Poll-Loop). */
void genet_poll(void);

#endif /* RPI_RTOS_GENET_H */
