/*
 * net/icmp.c  --  ICMP (Echo-Reply auf Pings, eigene Echo-Requests)
 */
#include <stdint.h>
#include "net.h"
#include "kmem.h"
#include "uart.h"

void icmp_input(netif_t *nif, ip4_addr_t src, ip4_addr_t dst, uint8_t *payload,
                uint16_t len)
{
    if (len < sizeof(icmp_hdr_t)) {
        return;
    }
    /* Checksumme der gesamten ICMP-Nachricht pruefen. */
    if (inet_checksum(payload, len) != 0) {
        return;
    }

    icmp_hdr_t *ic = (icmp_hdr_t *)payload;

    if (ic->type == ICMP_ECHO_REQUEST && ic->code == 0) {
        /* Nur Unicast-Echo-Requests an uns beantworten -- keine Antwort auf
         * Broadcast/Multicast (Schutz gegen Smurf-Reflexion). */
        if (dst != nif->ip) {
            return;
        }
        /* Echo-Reply: Nachricht spiegeln, Typ aendern, Checksumme neu. */
        static uint8_t reply[NET_FRAME_MAX];
        if (len > sizeof(reply)) {
            return;
        }
        memcpy(reply, payload, len);
        icmp_hdr_t *r = (icmp_hdr_t *)reply;
        r->type = ICMP_ECHO_REPLY;
        r->code = 0;
        r->checksum = 0;
        r->checksum = htons(inet_checksum(reply, len));
        ip_send(nif, src, IPPROTO_ICMP, reply, len);
    } else if (ic->type == ICMP_ECHO_REPLY && ic->code == 0) {
        uart_puts("[icmp] echo reply von ");
        net_print_ip(src);
        uart_puts(" id=");
        uart_putdec(ntohs(ic->id));
        uart_puts(" seq=");
        uart_putdec(ntohs(ic->seq));
        uart_puts("\n");
    }
}

int icmp_send_echo(netif_t *nif, ip4_addr_t dst, uint16_t id, uint16_t seq)
{
    static const uint8_t pattern[32] = {
        'r','p','i','_','r','t','o','s',' ','p','i','n','g',' ','0','1',
        '2','3','4','5','6','7','8','9','a','b','c','d','e','f','-','*'
    };
    uint8_t msg[sizeof(icmp_hdr_t) + sizeof(pattern)];

    icmp_hdr_t *ic = (icmp_hdr_t *)msg;
    ic->type = ICMP_ECHO_REQUEST;
    ic->code = 0;
    ic->checksum = 0;
    ic->id = htons(id);
    ic->seq = htons(seq);
    memcpy(msg + sizeof(icmp_hdr_t), pattern, sizeof(pattern));
    ic->checksum = htons(inet_checksum(msg, sizeof(msg)));

    net_enter();
    int r = ip_send(nif, dst, IPPROTO_ICMP, msg, sizeof(msg));
    net_leave();
    return r;
}
