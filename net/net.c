/*
 * net/net.c  --  Stack-Kern: Internet-Checksumme, Ethernet, Ausgabe-Helfer
 */
#include <stdint.h>
#include "net.h"
#include "kmem.h"
#include "uart.h"
#include "aarch64.h"
#include "spinlock.h"

/* --- Big-Net-Lock: serialisiert den GESAMTEN Stack-Zustand ---
 * Der TCP/IP-Stack teilt globalen, nicht-reentranten Zustand: den `static frame[]`-
 * Sendepuffer (eth_send), die TCP-PCB-Tabelle/ISN/Reassembly, den ARP-Cache und die
 * DHCP/DNS/HTTP-Zustandsmaschinen. Heute wird er ausschliesslich EINKERNIG per Poll
 * gefahren (virt-Harness; der raspi/SMP-Build faehrt kein Netz; GENET ist ungenutzt;
 * kein EL0-Socket-Syscall) -> aktuell KEINE Nebenlaeufigkeit. Dieses Lock ist die
 * DEFENSIVE Absicherung fuer das kuenftige real-HW-Fahrmodell (GENET-IRQ-RX gegen den
 * Poll-/App-Pfad oder ein zweiter Kern): dann serialisiert es jede Stack-Operation.
 *
 * REENTRANT (owner+depth, wie uart_begin/end): der Eingangspfad ruft aus sich heraus
 * verriegelte Sender auf (z.B. eth_input -> UDP-Handler -> udp_send, tcp_input ->
 * App-Callback -> tcp_write) -> ein nicht-reentrantes Lock wuerde sich selbst
 * verklemmen. Gleicher Kern -> nur Tiefe erhoehen; fremder Kern -> blockieren.
 * IRQ-maskiert beim Halten (Owner = dieser Kern) -> die Owner-Pruefung ist rennfrei. */
static spinlock_t      s_netlock = SPINLOCK_INIT;
static volatile int    s_net_owner = -1;   /* haltender Kern (-1 = frei) */
static int             s_net_depth;        /* Reentranz-Tiefe (nur vom Owner angefasst) */
static uint64_t        s_net_flags;        /* DAIF des aeussersten Halters */

#ifdef RTOS_SELFTEST
/* T1.11-Guardian-Instrumentierung: beweist, dass das Lock REENTRANT betreten wird
 * (max-Tiefe > 1 waehrend echter Stack-Arbeit) UND jeder enter durch ein leave
 * balanciert wird (kein geleckter/ueberzaehliger Aufruf). */
static int             s_net_maxdepth;     /* je beobachtete Maximaltiefe */
static volatile int    s_net_unbalanced;   /* Latch: leave ohne passenden enter */
uint32_t net_lock_maxdepth(void)   { return (uint32_t)s_net_maxdepth; }
uint32_t net_lock_unbalanced(void) { return (uint32_t)s_net_unbalanced; }
#endif

void net_enter(void)
{
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    int me = (int)cpu_id();
    if (s_net_owner == me) {          /* schon von diesem Kern gehalten -> nur vertiefen */
        s_net_depth++;
#ifdef RTOS_SELFTEST
        if (s_net_depth > s_net_maxdepth) { s_net_maxdepth = s_net_depth; }
#endif
        return;
    }
    spin_lock(&s_netlock);
    s_net_owner = me;
    s_net_depth = 1;
    s_net_flags = f;                  /* DAIF des aeussersten enter merken */
#ifdef RTOS_SELFTEST
    if (s_net_depth > s_net_maxdepth) { s_net_maxdepth = s_net_depth; }
#endif
}

void net_leave(void)
{
#ifdef RTOS_SELFTEST
    if (s_net_owner != (int)cpu_id() || s_net_depth <= 0) { s_net_unbalanced = 1; return; }
#endif
    if (--s_net_depth == 0) {
        uint64_t f = s_net_flags;
        s_net_owner = -1;
        spin_unlock(&s_netlock);
        WRITE_SYSREG(daif, f);
    }
}

/* Test-only Zeit-Offset (Default 0): erlaubt dem in-guest Loopback-Conformance-Test, die
 * Uhr fuer tcp_tick gezielt VORzustellen, um RTO-basierte Pfade (Persist-Probe, Retransmit)
 * synchron auszuloesen, ohne real 500ms zu warten. Im Normalbetrieb 0 -> kein Effekt; der
 * Test setzt ihn nach Gebrauch wieder auf 0 (vor jedem echten Verkehr).
 * NUR im Selbsttest-Build: der Produktions-/RC-Build (ohne RTOS_SELFTEST) enthaelt diese
 * Zeit-Manipulation nicht -> s_time_off_ms wird zur Konstanten 0 (wegoptimiert). */
#ifdef RTOS_SELFTEST
static uint64_t s_time_off_ms;
void net_test_advance_ms(uint64_t delta) { s_time_off_ms += delta; }
void net_test_reset_time(void)           { s_time_off_ms = 0; }
#else
#define s_time_off_ms 0u
#endif

/* Monotone Zeit in ms aus dem ARM Generic Timer (CNTPCT_EL0/CNTFRQ_EL0).
 * Portabel und ohne IRQ -- EL1 darf CNTPCT_EL0 immer lesen. */
uint64_t net_now_ms(void)
{
    uint64_t cnt = READ_SYSREG(cntpct_el0);
    uint64_t frq = READ_SYSREG(cntfrq_el0);
    if (!frq) {
        return s_time_off_ms;
    }
    /* Overflow-sicher (cnt*1000 wuerde sonst nach ~9 Jahren Uptime ueberlaufen). */
    return (cnt / frq) * 1000u + ((cnt % frq) * 1000u) / frq + s_time_off_ms;
}

/* --- Internet-Checksumme (RFC 1071) ---
 * Woerter werden big-endian aufsummiert (p[0]<<8 | p[1]); das Ergebnis von
 * csum_fold() ist der numerische Wert, dessen On-Wire-Bytes (hi, lo) sind ->
 * mit htons() ins Headerfeld schreiben. Beim Pruefen eines empfangenen Headers
 * (Checksumme inklusive) ergibt die Faltung 0. */
uint32_t csum_partial(const void *data, uint32_t len, uint32_t init)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = init;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)p[0] << 8;   /* letztes ungerades Byte = High-Byte */
    }
    return sum;
}

uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFF);
}

/* --- Ausgabe-Helfer --- */
void net_print_ip(ip4_addr_t ip)
{
    uart_putdec((ip >> 24) & 0xFF);
    uart_putc('.');
    uart_putdec((ip >> 16) & 0xFF);
    uart_putc('.');
    uart_putdec((ip >> 8) & 0xFF);
    uart_putc('.');
    uart_putdec(ip & 0xFF);
}

void net_print_mac(const mac_addr_t *m)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < ETH_ALEN; i++) {
        if (i) {
            uart_putc(':');
        }
        uart_putc(hx[(m->b[i] >> 4) & 0xF]);
        uart_putc(hx[m->b[i] & 0xF]);
    }
}

/* --- Ethernet --- */
static const mac_addr_t BROADCAST_MAC = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };

void eth_input(netif_t *nif, uint8_t *frame, uint16_t len)
{
    net_enter();                 /* RX-Waist: der gesamte Eingangspfad unter dem Big-Net-Lock */
    if (len >= sizeof(eth_hdr_t)) {
        eth_hdr_t *eh = (eth_hdr_t *)frame;
        uint16_t type = ntohs(eh->ethertype);
        uint8_t  *payload = frame + sizeof(eth_hdr_t);
        uint16_t  plen = (uint16_t)(len - sizeof(eth_hdr_t));

        switch (type) {
        case ETHERTYPE_ARP:
            arp_input(nif, frame, len);
            break;
        case ETHERTYPE_IPV4:
            ip_input(nif, payload, plen);
            break;
        default:
            break;   /* IPv6/VLAN/... ignorieren */
        }
    }
    net_leave();
}

int eth_send(netif_t *nif, const mac_addr_t *dst, uint16_t ethertype,
             const uint8_t *payload, uint16_t plen)
{
    static uint8_t frame[NET_FRAME_MAX];

    if (plen > NET_FRAME_MAX - sizeof(eth_hdr_t)) {
        return -1;
    }
    eth_hdr_t *eh = (eth_hdr_t *)frame;
    memcpy(eh->dst, dst->b, ETH_ALEN);
    memcpy(eh->src, nif->mac.b, ETH_ALEN);
    eh->ethertype = htons(ethertype);
    memcpy(frame + sizeof(eth_hdr_t), payload, plen);

    uint16_t total = (uint16_t)(sizeof(eth_hdr_t) + plen);
    if (total < 60) {                      /* Ethernet-Mindestlaenge (ohne FCS) */
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    return nif->transmit(nif, frame, total);
}

/* Broadcast-MAC exportieren (von arp.c genutzt). */
const mac_addr_t *eth_broadcast(void) { return &BROADCAST_MAC; }
