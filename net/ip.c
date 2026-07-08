/*
 * net/ip.c  --  IPv4 (Eingang mit Checksummenpruefung, Ausgang mit Routing)
 *
 * Keine Fragmentierung, keine Optionen beim Senden. Routing minimal:
 * Ziel im eigenen Subnetz -> direkt, sonst ueber das Gateway.
 */
#include <stdint.h>
#include "net.h"
#include "kmem.h"
#include "uart.h"

static uint16_t s_ip_id = 1;

void ip_input(netif_t *nif, uint8_t *payload, uint16_t len)
{
    if (len < sizeof(ip_hdr_t)) {
        return;
    }
    ip_hdr_t *ih = (ip_hdr_t *)payload;

    uint8_t version = ih->ver_ihl >> 4;
    uint8_t ihl = (ih->ver_ihl & 0x0F) * 4;
    if (version != 4 || ihl < sizeof(ip_hdr_t) || ihl > len) {
        return;
    }

    /* Header-Checksumme pruefen (Faltung muss 0 ergeben). */
    if (inet_checksum(ih, ihl) != 0) {
        return;
    }

    uint16_t total = ntohs(ih->total_len);
    if (total < ihl || total > len) {
        return;   /* unplausible Laenge / abgeschnitten */
    }

    /* Nur an uns gerichtete Unicast-Pakete (oder Broadcast) annehmen. Solange
     * wir noch keine IP haben (nif->ip == 0, z.B. waehrend DHCP), alles annehmen
     * -- so kommen auch an die angebotene IP unicastete DHCP-Antworten durch. */
    ip4_addr_t dst = ntohl(ih->dst);
    ip4_addr_t bcast = nif->ip | ~nif->netmask;
    if (nif->ip != 0 && dst != nif->ip && dst != 0xFFFFFFFFu && dst != bcast) {
        return;
    }

    /* Fragmente (MF gesetzt oder Offset != 0) verwerfen. */
    if (ntohs(ih->flags_frag) & 0x3FFF) {
        return;
    }

    ip4_addr_t src = ntohl(ih->src);
    uint8_t   *l4 = payload + ihl;
    uint16_t   l4len = (uint16_t)(total - ihl);

    switch (ih->proto) {
    case IPPROTO_ICMP:
        icmp_input(nif, src, dst, l4, l4len);
        break;
    case IPPROTO_UDP:
        udp_input(nif, src, dst, l4, l4len);
        break;
    case IPPROTO_TCP:
        tcp_input(nif, src, dst, l4, l4len);
        break;
    default:
        break;
    }
}

int ip_send(netif_t *nif, ip4_addr_t dst, uint8_t proto,
            const uint8_t *payload, uint16_t plen)
{
    static uint8_t pkt[NET_FRAME_MAX];

    if ((uint32_t)sizeof(ip_hdr_t) + plen > NET_FRAME_MAX - sizeof(eth_hdr_t)) {
        return -1;
    }

    /* Ziel-MAC bestimmen. Broadcast (limitiert oder Subnetz) geht direkt an die
     * Ethernet-Broadcast-MAC ohne ARP (wichtig fuer DHCP, bevor wir eine IP haben). */
    mac_addr_t dmac;
    ip4_addr_t subnet_bcast = nif->ip | ~nif->netmask;
    if (dst == IP_BROADCAST || (nif->netmask != 0 && dst == subnet_bcast)) {
        dmac = *eth_broadcast();
    } else {
        /* Next-Hop: gleiches Subnetz -> direkt, sonst Gateway. */
        ip4_addr_t next_hop;
        if ((dst & nif->netmask) == (nif->ip & nif->netmask)) {
            next_hop = dst;
        } else {
            next_hop = nif->gateway;
        }
        if (!arp_lookup(next_hop, &dmac)) {
            arp_request(nif, next_hop);
            return -1;   /* Aufrufer moege nach ARP-Reply erneut senden */
        }
    }

    ip_hdr_t *ih = (ip_hdr_t *)pkt;
    ih->ver_ihl = 0x45;
    ih->dscp_ecn = 0;
    ih->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + plen));
    ih->id = htons(s_ip_id++);
    ih->flags_frag = htons(0x4000);     /* Don't Fragment */
    ih->ttl = 64;
    ih->proto = proto;
    ih->checksum = 0;
    ih->src = htonl(nif->ip);
    ih->dst = htonl(dst);
    ih->checksum = htons(inet_checksum(ih, sizeof(ip_hdr_t)));

    memcpy(pkt + sizeof(ip_hdr_t), payload, plen);

    if (eth_send(nif, &dmac, ETHERTYPE_IPV4, pkt,
                 (uint16_t)(sizeof(ip_hdr_t) + plen)) < 0) {
        return -1;
    }
    return plen;
}
