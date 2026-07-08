/*
 * include/dns.h  --  Minimaler DNS-Client (A-Record-Resolver ueber UDP)
 *
 * Loest einen Hostnamen via einen DNS-Server (z.B. den SLIRP-DNS 10.0.2.3) in eine
 * IPv4-Adresse auf. Reines Polling: dns_tick() im Loop fuer Timeout/Retransmit.
 * Bewusst minimal: NUR A-Records (QTYPE 1), genau EINE Anfrage zur Zeit, keine
 * Caching-/TTL-Pflege, kein CNAME-Folgen (nimmt den ersten A-Record der Antwort).
 */
#ifndef RPI_RTOS_DNS_H
#define RPI_RTOS_DNS_H

#include <stdint.h>
#include "net.h"

/* Ergebnis: ok=1 -> ip ist die aufgeloeste Adresse; ok=0 -> Fehlschlag
 * (NXDOMAIN/Timeout/keine A-Antwort), ip undefiniert. name ist der angefragte Name. */
typedef void (*dns_result_fn)(const char *name, ip4_addr_t ip, int ok);

/* Startet eine Aufloesung gegen server:53. 0 = Anfrage abgeschickt, -1 = belegt
 * (es laeuft bereits eine) oder Fehler. cb wird genau einmal aufgerufen. */
int  dns_resolve(netif_t *nif, ip4_addr_t server, const char *name, dns_result_fn cb);

/* Im Poll-Loop aufrufen: Timeout-/Retransmit-Verwaltung der laufenden Anfrage. */
void dns_tick(netif_t *nif);

#endif /* RPI_RTOS_DNS_H */
