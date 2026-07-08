/*
 * include/virtio_net.h  --  virtio-net (virtio-mmio, modern/v2) fuer QEMU virt
 *
 * Reines Polling (kein IRQ). Bindet an ein netif und liefert empfangene Frames
 * per eth_input() an den Stack.
 */
#ifndef RPI_RTOS_VIRTIO_NET_H
#define RPI_RTOS_VIRTIO_NET_H

#include "net.h"

/* Sucht das virtio-net-Geraet, handelt Features aus, richtet RX/TX-Queues ein
 * und fuellt nif->mac / nif->transmit. Liefert 0 bei Erfolg. */
int virtio_net_init(netif_t *nif);

/* Verarbeitet alle anstehenden RX-Pakete (im Poll-Loop aufrufen). */
void virtio_net_poll(void);

#endif /* RPI_RTOS_VIRTIO_NET_H */
