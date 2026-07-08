/*
 * net/dhcp.c  --  Minimaler DHCP-Client (RFC 2131, IPv4)
 *
 * Zustandsautomat DISCOVER -> SELECTING -> REQUESTING -> BOUND. Sendet als
 * Broadcast (UDP 68 -> 67) mit gesetztem Broadcast-Flag, damit der Server die
 * Antwort broadcastet (wir haben waehrend der Aushandlung noch keine IP).
 */
#include <stdint.h>
#include "dhcp.h"
#include "net.h"
#include "kmem.h"
#include "uart.h"

#define DHCP_MAGIC      0x63825363u
#define BOOTREQUEST     1
#define BOOTREPLY       2

#define DHCPDISCOVER    1
#define DHCPOFFER       2
#define DHCPREQUEST     3
#define DHCPACK         5
#define DHCPNAK         6

#define OPT_SUBNET      1
#define OPT_ROUTER      3
#define OPT_REQ_IP      50
#define OPT_LEASE       51
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_PARAM_LIST  55
#define OPT_END         255

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} dhcp_hdr_t;   /* 240 Byte; Optionen folgen direkt dahinter */

enum { ST_INIT, ST_SELECTING, ST_REQUESTING, ST_BOUND, ST_RENEWING, ST_REBINDING };

static int        g_state;
static uint32_t   g_xid;
static ip4_addr_t g_offered_ip;
static ip4_addr_t g_server_id;
static netif_t   *g_nif;
static uint64_t   g_last_tx_ms;
static int        g_bound_handler;
/* Lease-Verwaltung: T1 (Renewing, Lease/2) + T2 (Rebinding, Lease*7/8) ab Bindezeitpunkt. */
static uint64_t   g_lease_ms;
static uint64_t   g_t1_ms, g_t2_ms;
static uint64_t   g_bound_ms;

static void put_ip(uint8_t *p, ip4_addr_t ip)   /* host order -> 4 Byte network */
{
    p[0] = (uint8_t)(ip >> 24);
    p[1] = (uint8_t)(ip >> 16);
    p[2] = (uint8_t)(ip >> 8);
    p[3] = (uint8_t)ip;
}

/* Option mit gegebenem Tag suchen; liefert Zeiger auf Wert + Laenge (bounds-safe). */
static const uint8_t *find_opt(const uint8_t *data, uint16_t len, uint8_t tag,
                               uint8_t *out_len)
{
    uint16_t i = sizeof(dhcp_hdr_t);   /* Optionen beginnen bei Offset 240 */
    while (i < len) {
        uint8_t t = data[i++];
        if (t == 0) {
            continue;                   /* PAD */
        }
        if (t == OPT_END || i >= len) {
            break;
        }
        uint8_t l = data[i++];
        if ((uint32_t)i + l > len) {
            break;                      /* abgeschnitten */
        }
        if (t == tag) {
            *out_len = l;
            return &data[i];
        }
        i += l;
    }
    return 0;
}

static ip4_addr_t opt_ip(const uint8_t *data, uint16_t len, uint8_t tag,
                         ip4_addr_t fallback)
{
    uint8_t l = 0;
    const uint8_t *v = find_opt(data, len, tag, &l);
    if (v && l == 4) {
        return ((ip4_addr_t)v[0] << 24) | ((ip4_addr_t)v[1] << 16) |
               ((ip4_addr_t)v[2] << 8) | (ip4_addr_t)v[3];
    }
    return fallback;
}

static void send_msg(uint8_t type)
{
    static uint8_t buf[300];
    memset(buf, 0, sizeof(buf));

    int renew = (g_state == ST_RENEWING || g_state == ST_REBINDING);

    dhcp_hdr_t *h = (dhcp_hdr_t *)buf;
    h->op = BOOTREQUEST;
    h->htype = 1;
    h->hlen = ETH_ALEN;
    h->xid = htonl(g_xid);
    /* Renewing/Rebinding: wir HABEN schon eine IP -> ciaddr setzen, KEIN Broadcast-Flag. */
    h->flags  = renew ? 0 : htons(0x8000);
    h->ciaddr = renew ? htonl(g_nif->ip) : 0;
    memcpy(h->chaddr, g_nif->mac.b, ETH_ALEN);
    h->magic = htonl(DHCP_MAGIC);

    uint8_t *o = buf + sizeof(dhcp_hdr_t);
    *o++ = OPT_MSG_TYPE; *o++ = 1; *o++ = type;
    if (type == DHCPREQUEST && g_state == ST_REQUESTING) {
        /* Nur der INITIALE REQUEST (nach OFFER) traegt angefragte IP + Server-ID; bei
         * Renewing/Rebinding identifiziert ciaddr den Client (RFC 2131 Tab. 5). */
        *o++ = OPT_REQ_IP;    *o++ = 4; put_ip(o, g_offered_ip); o += 4;
        *o++ = OPT_SERVER_ID; *o++ = 4; put_ip(o, g_server_id);  o += 4;
    }
    *o++ = OPT_PARAM_LIST; *o++ = 4;
    *o++ = OPT_SUBNET; *o++ = OPT_ROUTER; *o++ = 6 /*DNS*/; *o++ = OPT_LEASE;
    *o++ = OPT_END;

    uint16_t len = (uint16_t)(o - buf);
    if (len < 300) {
        len = 300;   /* BOOTP-Mindestgroesse; buf ist genullt -> PAD (0)-Bytes */
    }
    /* Renewing: UNICAST an den Lease-Server; sonst (Discover/initialer Request/Rebinding)
     * Broadcast. */
    ip4_addr_t dst = (g_state == ST_RENEWING) ? g_server_id : IP_BROADCAST;
    udp_send(g_nif, dst, 67, 68, buf, len);
    g_last_tx_ms = net_now_ms();
}

/* Lease aus einem DHCPACK uebernehmen: IP/Maske/Gateway + Lease-Zeit -> T1/T2 + Bindezeit. */
static void apply_lease(netif_t *nif, const dhcp_hdr_t *h, const uint8_t *data, uint16_t len)
{
    nif->ip      = ntohl(h->yiaddr);
    nif->netmask = opt_ip(data, len, OPT_SUBNET, IP4(255, 255, 255, 0));
    nif->gateway = opt_ip(data, len, OPT_ROUTER, g_server_id);

    uint8_t ll = 0;
    const uint8_t *lv = find_opt(data, len, OPT_LEASE, &ll);
    uint32_t lease_s = (lv && ll == 4) ? (((uint32_t)lv[0] << 24) | ((uint32_t)lv[1] << 16) |
                                          ((uint32_t)lv[2] << 8) | (uint32_t)lv[3])
                                       : 3600;   /* Default 1 h, falls Server keine Lease nennt */
    if (lease_s == 0) {
        lease_s = 3600;
    }
    g_lease_ms = (uint64_t)lease_s * 1000;
    g_t1_ms    = g_lease_ms / 2;          /* Renewing nach der Haelfte */
    g_t2_ms    = g_lease_ms * 7 / 8;      /* Rebinding nach 7/8 */
    g_bound_ms = net_now_ms();
    g_state    = ST_BOUND;
}

static void dhcp_input(netif_t *nif, ip4_addr_t src, uint16_t sport,
                       const uint8_t *data, uint16_t len)
{
    (void)sport;
    if (len < sizeof(dhcp_hdr_t)) {
        return;
    }
    const dhcp_hdr_t *h = (const dhcp_hdr_t *)data;
    if (h->op != BOOTREPLY || ntohl(h->xid) != g_xid ||
        ntohl(h->magic) != DHCP_MAGIC) {
        return;
    }

    uint8_t l = 0;
    const uint8_t *mt = find_opt(data, len, OPT_MSG_TYPE, &l);
    if (!mt || l < 1) {
        return;
    }

    if (mt[0] == DHCPOFFER && g_state == ST_SELECTING) {
        g_offered_ip = ntohl(h->yiaddr);
        g_server_id = opt_ip(data, len, OPT_SERVER_ID, src);
        g_state = ST_REQUESTING;
        send_msg(DHCPREQUEST);
    } else if (mt[0] == DHCPACK &&
               (g_state == ST_REQUESTING || g_state == ST_RENEWING || g_state == ST_REBINDING)) {
        int was_renew = (g_state != ST_REQUESTING);
        /* Server-ID aktualisieren (bei Rebinding kann ein anderer Server antworten). */
        g_server_id = opt_ip(data, len, OPT_SERVER_ID, g_server_id);
        apply_lease(nif, h, data, len);

        uart_puts(was_renew ? "[dhcp] Lease erneuert: IP " : "[dhcp] Lease: IP ");
        net_print_ip(nif->ip);
        uart_puts(" Maske ");
        net_print_ip(nif->netmask);
        uart_puts(" GW ");
        net_print_ip(nif->gateway);
        uart_puts(" (Lease ");
        uart_putdec((unsigned)(g_lease_ms / 1000));
        uart_puts("s)\n");
    } else if (mt[0] == DHCPNAK && g_state != ST_INIT && g_state != ST_BOUND) {
        /* Renewal/Aushandlung abgelehnt -> frische Transaktion (xid) + Lease verwerfen. */
        nif->ip = 0; nif->netmask = 0; nif->gateway = 0;
        g_lease_ms = 0;
        g_xid = (uint32_t)net_now_ms() ^ 0x52504900u;
        g_state = ST_SELECTING;
        send_msg(DHCPDISCOVER);
    }
}

void dhcp_start(netif_t *nif)
{
    net_enter();
    g_nif = nif;
    nif->ip = 0;
    nif->netmask = 0;
    nif->gateway = 0;
    g_xid = (uint32_t)net_now_ms() ^ 0x52504900u;   /* "RPI" + Zeit */
    g_state = ST_SELECTING;

    if (!g_bound_handler) {
        udp_bind(68, dhcp_input);
        g_bound_handler = 1;
    }
    uart_puts("[dhcp] DISCOVER...\n");
    send_msg(DHCPDISCOVER);
    net_leave();
}

/* Big-Net-Lock-Wrapper (T1.11) -- der Rumpf hat viele fruehe returns, daher hier
 * einmal net_enter/net_leave klammern statt vor jedem return. */
static void dhcp_tick_inner(netif_t *nif);
void dhcp_tick(netif_t *nif)
{
    net_enter();
    dhcp_tick_inner(nif);
    net_leave();
}

static void dhcp_tick_inner(netif_t *nif)
{
    uint64_t now = net_now_ms();
    if (g_state == ST_INIT) {
        return;
    }
    if (g_state == ST_BOUND) {
        if (!g_lease_ms) {
            return;                         /* keine Lease-Zeit bekannt -> nicht erneuern */
        }
        uint64_t age = now - g_bound_ms;
        if (age >= g_lease_ms) {            /* Lease abgelaufen -> komplett neu aushandeln */
            nif->ip = 0; nif->netmask = 0; nif->gateway = 0; g_lease_ms = 0;
            g_xid = (uint32_t)now ^ 0x52504900u;
            g_state = ST_SELECTING;
            uart_puts("[dhcp] Lease abgelaufen -> DISCOVER\n");
            send_msg(DHCPDISCOVER);
        } else if (age >= g_t2_ms) {
            g_state = ST_REBINDING;
            g_xid = (uint32_t)now ^ 0x52504900u;   /* frische Transaktion je Erneuerung (RFC 2131) */
            uart_puts("[dhcp] T2 -> Rebinding (Broadcast)\n");
            send_msg(DHCPREQUEST);
        } else if (age >= g_t1_ms) {
            g_state = ST_RENEWING;
            g_xid = (uint32_t)now ^ 0x52504900u;
            uart_puts("[dhcp] T1 -> Renewing (Unicast)\n");
            send_msg(DHCPREQUEST);
        }
        return;
    }
    /* RENEWING/REBINDING: scheitert die Erneuerung (Server antwortet nicht), zeitbasiert
     * eskalieren statt ewig denselben REQUEST zu wiederholen (RFC 2131): bei T2 von Renewing
     * auf Rebinding (Broadcast) wechseln, bei Lease-Ablauf die IP aufgeben + neu DISCOVERn. */
    if (g_state == ST_RENEWING || g_state == ST_REBINDING) {
        uint64_t age = now - g_bound_ms;
        if (g_lease_ms && age >= g_lease_ms) {
            nif->ip = 0; nif->netmask = 0; nif->gateway = 0; g_lease_ms = 0;
            g_xid = (uint32_t)now ^ 0x52504900u;
            g_state = ST_SELECTING;
            uart_puts("[dhcp] Lease abgelaufen (Erneuerung gescheitert) -> DISCOVER\n");
            send_msg(DHCPDISCOVER);
            return;
        }
        if (g_state == ST_RENEWING && g_lease_ms && age >= g_t2_ms) {
            g_state = ST_REBINDING;
            g_xid = (uint32_t)now ^ 0x52504900u;
            uart_puts("[dhcp] T2 -> Rebinding (Broadcast)\n");
            send_msg(DHCPREQUEST);
            return;
        }
    }
    /* SELECTING/REQUESTING/RENEWING/REBINDING: Retransmit nach 1 s ohne Fortschritt. */
    if (now - g_last_tx_ms >= 1000) {
        send_msg(g_state == ST_SELECTING ? DHCPDISCOVER : DHCPREQUEST);
    }
}

/* Sofortige Erneuerung auf Anforderung (wie der T1-Uebergang, unabhaengig von der Zeit).
 * No-op, wenn nicht gebunden. */
void dhcp_renew(netif_t *nif)
{
    (void)nif;
    net_enter();
    if (g_state == ST_BOUND) {
        g_state = ST_RENEWING;
        g_xid = (uint32_t)net_now_ms() ^ 0x52504900u;   /* frische Transaktion je Erneuerung */
        uart_puts("[dhcp] Renew (manuell) -> Renewing (Unicast)\n");
        send_msg(DHCPREQUEST);
    }
    net_leave();
}

int dhcp_bound(void)
{
    net_enter();
    int b = (g_state == ST_BOUND);
    net_leave();
    return b;
}
