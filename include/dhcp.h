/*
 * include/dhcp.h  --  Minimaler DHCP-Client (IPv4)
 *
 * DISCOVER -> OFFER -> REQUEST -> ACK -> BOUND; setzt netif IP/Netmask/Gateway aus dem
 * Lease. Mit Lease-Timer: T1 (Renewing, Unicast) + T2 (Rebinding, Broadcast) + Ablauf.
 * Reines Polling: dhcp_start() einmal aufrufen, dhcp_tick() im Loop.
 */
#ifndef RPI_RTOS_DHCP_H
#define RPI_RTOS_DHCP_H

#include "net.h"

void dhcp_start(netif_t *nif);   /* sendet DISCOVER, registriert UDP-Port 68 */
void dhcp_tick(netif_t *nif);    /* Retransmit + Lease-Erneuerung (T1/T2/Ablauf) im Poll-Loop */
void dhcp_renew(netif_t *nif);   /* sofortige Erneuerung anstossen (no-op wenn nicht gebunden) */
int  dhcp_bound(void);           /* 1, sobald eine IP geleast wurde */

#endif /* RPI_RTOS_DHCP_H */
