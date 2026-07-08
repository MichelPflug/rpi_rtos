/*
 * net/arp.c  --  Address Resolution Protocol (IPv4 ueber Ethernet)
 *
 * Kleine Cache-Tabelle (LRU-frei, rundlaufende Verdraengung). Beantwortet
 * Requests fuer unsere IP und cached Sender-Adressen aus Requests/Replies.
 */
#include <stdint.h>
#include "net.h"
#include "kmem.h"
#include "uart.h"

#define ARP_CACHE_SIZE 8

typedef struct {
    ip4_addr_t ip;          /* host order, 0 = leer */
    mac_addr_t mac;
} arp_entry_t;

static arp_entry_t s_cache[ARP_CACHE_SIZE];
static int         s_next;   /* Verdraengungs-Cursor */

void arp_cache_put(ip4_addr_t ip, const mac_addr_t *mac)
{
    if (ip == 0) {
        return;
    }
    /* Vorhandenen Eintrag aktualisieren. */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_cache[i].ip == ip) {
            s_cache[i].mac = *mac;
            return;
        }
    }
    /* Sonst freien Slot oder den naechsten verdraengen. */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_cache[i].ip == 0) {
            s_cache[i].ip = ip;
            s_cache[i].mac = *mac;
            return;
        }
    }
    s_cache[s_next].ip = ip;
    s_cache[s_next].mac = *mac;
    s_next = (s_next + 1) % ARP_CACHE_SIZE;
}

int arp_lookup(ip4_addr_t ip, mac_addr_t *out)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_cache[i].ip == ip) {
            if (out) {
                *out = s_cache[i].mac;
            }
            return 1;
        }
    }
    return 0;
}

/* IP host-order -> 4 Byte network order. */
static void ip_to_bytes(ip4_addr_t ip, uint8_t out[4])
{
    out[0] = (uint8_t)(ip >> 24);
    out[1] = (uint8_t)(ip >> 16);
    out[2] = (uint8_t)(ip >> 8);
    out[3] = (uint8_t)ip;
}

static ip4_addr_t bytes_to_ip(const uint8_t in[4])
{
    return ((ip4_addr_t)in[0] << 24) | ((ip4_addr_t)in[1] << 16) |
           ((ip4_addr_t)in[2] << 8) | (ip4_addr_t)in[3];
}

void arp_request(netif_t *nif, ip4_addr_t target_ip)
{
    net_enter();               /* app- UND intern (ip_send bei Cache-Miss) erreichbar -> reentrant */
    arp_hdr_t a;
    memset(&a, 0, sizeof(a));
    a.htype = htons(1);                 /* Ethernet */
    a.ptype = htons(ETHERTYPE_IPV4);
    a.hlen = ETH_ALEN;
    a.plen = 4;
    a.oper = htons(1);                  /* request */
    memcpy(a.sha, nif->mac.b, ETH_ALEN);
    ip_to_bytes(nif->ip, a.spa);
    /* tha bleibt 0 (unbekannt) */
    ip_to_bytes(target_ip, a.tpa);

    eth_send(nif, eth_broadcast(), ETHERTYPE_ARP, (const uint8_t *)&a, sizeof(a));
    net_leave();
}

void arp_input(netif_t *nif, uint8_t *frame, uint16_t len)
{
    if (len < sizeof(eth_hdr_t) + sizeof(arp_hdr_t)) {
        return;
    }
    arp_hdr_t *a = (arp_hdr_t *)(frame + sizeof(eth_hdr_t));

    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != ETHERTYPE_IPV4 ||
        a->hlen != ETH_ALEN || a->plen != 4) {
        return;
    }

    ip4_addr_t spa = bytes_to_ip(a->spa);
    ip4_addr_t tpa = bytes_to_ip(a->tpa);
    mac_addr_t sha;
    memcpy(sha.b, a->sha, ETH_ALEN);

    /* RFC 826: bestehenden Eintrag immer aktualisieren; einen NEUEN Eintrag nur
     * anlegen, wenn das Paket an uns gerichtet ist (tpa == unsere IP). Das
     * verhindert das Cachen fremder Broadcast-Sender (Cache-Vergiftung/
     * Verdraengungs-DoS bei nur 8 Slots). */
    if (arp_lookup(spa, 0) || tpa == nif->ip) {
        arp_cache_put(spa, &sha);
    }

    uint16_t oper = ntohs(a->oper);
    if (oper == 1 && tpa == nif->ip) {
        /* Request fuer unsere IP -> Reply senden. */
        arp_hdr_t r;
        memset(&r, 0, sizeof(r));
        r.htype = htons(1);
        r.ptype = htons(ETHERTYPE_IPV4);
        r.hlen = ETH_ALEN;
        r.plen = 4;
        r.oper = htons(2);              /* reply */
        memcpy(r.sha, nif->mac.b, ETH_ALEN);
        ip_to_bytes(nif->ip, r.spa);
        memcpy(r.tha, sha.b, ETH_ALEN);
        ip_to_bytes(spa, r.tpa);
        eth_send(nif, &sha, ETHERTYPE_ARP, (const uint8_t *)&r, sizeof(r));
    } else if (oper == 2) {
        uart_puts("[arp] ");
        net_print_ip(spa);
        uart_puts(" is-at ");
        net_print_mac(&sha);
        uart_puts("\n");
    }
}
