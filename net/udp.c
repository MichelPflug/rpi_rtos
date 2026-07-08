/*
 * net/udp.c  --  UDP (Port-Demultiplexing, Pseudo-Header-Checksumme)
 */
#include <stdint.h>
#include "net.h"
#include "kmem.h"

#define UDP_MAX_BINDINGS 4

typedef struct {
    uint16_t      port;        /* 0 = frei */
    udp_handler_t handler;
} udp_binding_t;

static udp_binding_t s_bindings[UDP_MAX_BINDINGS];

int udp_bind(uint16_t port, udp_handler_t handler)
{
    net_enter();
    int r = -1;
    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (s_bindings[i].port == 0) {
            s_bindings[i].port = port;
            s_bindings[i].handler = handler;
            r = 0;
            break;
        }
    }
    net_leave();
    return r;
}

/* Pseudo-Header (12 Byte) + UDP-Segment aufsummieren und falten. */
static uint16_t udp_checksum(ip4_addr_t src, ip4_addr_t dst,
                             const uint8_t *seg, uint16_t seglen)
{
    uint8_t pseudo[12];
    pseudo[0] = (uint8_t)(src >> 24);
    pseudo[1] = (uint8_t)(src >> 16);
    pseudo[2] = (uint8_t)(src >> 8);
    pseudo[3] = (uint8_t)src;
    pseudo[4] = (uint8_t)(dst >> 24);
    pseudo[5] = (uint8_t)(dst >> 16);
    pseudo[6] = (uint8_t)(dst >> 8);
    pseudo[7] = (uint8_t)dst;
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_UDP;
    pseudo[10] = (uint8_t)(seglen >> 8);
    pseudo[11] = (uint8_t)seglen;

    uint32_t sum = csum_partial(pseudo, sizeof(pseudo), 0);
    sum = csum_partial(seg, seglen, sum);
    return csum_fold(sum);
}

void udp_input(netif_t *nif, ip4_addr_t src, ip4_addr_t dst, uint8_t *payload,
               uint16_t len)
{
    if (len < sizeof(udp_hdr_t)) {
        return;
    }
    udp_hdr_t *uh = (udp_hdr_t *)payload;

    uint16_t ulen = ntohs(uh->len);
    if (ulen < sizeof(udp_hdr_t) || ulen > len) {
        return;
    }

    /* Optionale Checksumme: nur pruefen, wenn das Feld != 0 ist. Der Pseudo-
     * Header muss die TATSAECHLICHE Ziel-IP des Datagramms verwenden (bei
     * Broadcast/Multicast != nif->ip), sonst werden gueltige Pakete verworfen. */
    if (uh->checksum != 0) {
        if (udp_checksum(src, dst, payload, ulen) != 0) {
            return;
        }
    }

    uint16_t dport = ntohs(uh->dst_port);
    uint16_t sport = ntohs(uh->src_port);
    const uint8_t *data = payload + sizeof(udp_hdr_t);
    uint16_t dlen = (uint16_t)(ulen - sizeof(udp_hdr_t));

    for (int i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (s_bindings[i].port == dport && s_bindings[i].handler) {
            s_bindings[i].handler(nif, src, sport, data, dlen);
            return;
        }
    }
}

int udp_send(netif_t *nif, ip4_addr_t dst, uint16_t dport, uint16_t sport,
             const uint8_t *data, uint16_t len)
{
    static uint8_t seg[NET_FRAME_MAX];   /* geteilt -> unter dem Big-Net-Lock */

    net_enter();
    int ret = -1;
    uint32_t seglen = (uint32_t)sizeof(udp_hdr_t) + len;
    if (seglen <= NET_FRAME_MAX) {
        udp_hdr_t *uh = (udp_hdr_t *)seg;
        uh->src_port = htons(sport);
        uh->dst_port = htons(dport);
        uh->len = htons((uint16_t)seglen);
        uh->checksum = 0;
        memcpy(seg + sizeof(udp_hdr_t), data, len);

        uint16_t ck = udp_checksum(nif->ip, dst, seg, (uint16_t)seglen);
        /* Eine berechnete 0 wird als 0xFFFF uebertragen (RFC 768). */
        uh->checksum = htons(ck == 0 ? 0xFFFF : ck);

        ret = ip_send(nif, dst, IPPROTO_UDP, seg, (uint16_t)seglen);
    }
    net_leave();
    return ret;
}
