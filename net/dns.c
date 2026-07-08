/*
 * net/dns.c  --  Minimaler DNS-Client (A-Record-Resolver ueber UDP)
 *
 * Baut eine Standard-Query (RD=1) fuer einen A-Record, sendet sie an server:53 von
 * einem festen lokalen Port, und parst die Antwort robust (Bounds-geprueft, inkl.
 * Namens-Kompressionszeiger). Genau eine Anfrage zur Zeit; dns_tick() macht
 * Timeout/Retransmit. Keine Daten werden aus dem Netz heraus ungeprueft indiziert.
 */
#include <stdint.h>
#include "net.h"
#include "dns.h"
#include "uart.h"

#define DNS_PORT         53
#define DNS_CLIENT_PORT  5300       /* fester lokaler Port (eine Anfrage zur Zeit) */
#define DNS_TIMEOUT_MS   1000
#define DNS_MAX_RTX      3
#define DNS_NAME_MAX     255        /* voller DNS-Name (nicht das 63er-Label-Limit) */

static struct {
    int           active;
    uint16_t      id;
    ip4_addr_t    server;
    char          name[DNS_NAME_MAX + 1];
    dns_result_fn cb;
    uint64_t      sent_ms;
    int           retries;
    netif_t      *nif;
} s_q;

static int s_bound;                 /* udp_bind nur einmal */

/* QNAME enkodieren: "example.com" -> 07 'example' 03 'com' 00. Laenge oder -1. */
static int encode_qname(const char *name, uint8_t *out, int max)
{
    int o = 0;
    const char *p = name;
    while (*p) {
        int len = 0;
        while (p[len] && p[len] != '.') {
            len++;
        }
        if (len == 0 || len > 63 || o + len + 1 >= max) {
            return -1;
        }
        out[o++] = (uint8_t)len;
        for (int i = 0; i < len; i++) {
            out[o++] = (uint8_t)p[i];
        }
        p += len;
        if (*p == '.') {
            p++;
        }
    }
    if (o >= max) {
        return -1;
    }
    out[o++] = 0;                   /* Root-Label */
    return o;
}

static void finish(int ok, ip4_addr_t ip)
{
    if (!s_q.active) {
        return;
    }
    s_q.active = 0;
    if (s_q.cb) {
        s_q.cb(s_q.name, ip, ok);
    }
}

static void send_query(void)
{
    uint8_t pkt[300];
    pkt[0] = (uint8_t)(s_q.id >> 8);  pkt[1] = (uint8_t)s_q.id;
    pkt[2] = 0x01; pkt[3] = 0x00;     /* RD=1 (Rekursion gewuenscht) */
    pkt[4] = 0;    pkt[5] = 1;        /* QDCOUNT = 1 */
    pkt[6] = 0;    pkt[7] = 0;        /* ANCOUNT */
    pkt[8] = 0;    pkt[9] = 0;        /* NSCOUNT */
    pkt[10] = 0;   pkt[11] = 0;       /* ARCOUNT */
    int qn = encode_qname(s_q.name, pkt + 12, (int)sizeof(pkt) - 12 - 4);
    if (qn < 0) {
        return;   /* (durch dns_resolve vorab geprueft) -- NICHT synchron finish()en */
    }
    int o = 12 + qn;
    pkt[o++] = 0; pkt[o++] = 1;       /* QTYPE  = A */
    pkt[o++] = 0; pkt[o++] = 1;       /* QCLASS = IN */
    udp_send(s_q.nif, s_q.server, DNS_PORT, DNS_CLIENT_PORT, pkt, (uint16_t)o);
    s_q.sent_ms = net_now_ms();
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Vergleicht den DNS-Namen ab Offset o (reine Labels -- die Frage darf KEINE Kompression
 * enthalten) case-insensitiv mit `name`. Liefert den Offset NACH dem Namen oder -1 bei
 * Mismatch. Haertet gegen Antworten mit passender ID, aber anderer/gespoofter Frage. */
static int qname_eq(const uint8_t *d, int len, int o, const char *name)
{
    const char *p = name;
    while (o < len) {
        uint8_t l = d[o++];
        if (l == 0) {
            return (*p == '\0') ? o : -1;       /* Namensende: nur ok, wenn auch `name` zuende */
        }
        if (l & 0xC0) {
            return -1;                          /* Kompression/reserviert in der Frage -> ungueltig */
        }
        if (o + l > len) {
            return -1;
        }
        for (int i = 0; i < l; i++) {
            if (*p == '\0' || lc((char)d[o + i]) != lc(*p)) {
                return -1;
            }
            p++;
        }
        o += l;
        if (*p == '.') {
            p++;
        } else if (*p != '\0') {
            return -1;                          /* Label endet, aber `name` hat hier kein '.' */
        }
    }
    return -1;
}

/* Einen DNS-Namen ab Offset o ueberspringen (Labels bzw. ein Kompressionszeiger).
 * Bounds-geprueft; -1 bei Inkonsistenz. Folgt dem Zeiger NICHT (nur ueberspringen). */
static int skip_name(const uint8_t *d, int len, int o)
{
    while (o >= 0 && o < len) {
        uint8_t l = d[o];
        if (l == 0) {
            return o + 1;                       /* Ende */
        }
        if ((l & 0xC0) == 0xC0) {
            return (o + 2 <= len) ? (o + 2) : -1;   /* 2-Byte-Zeiger */
        }
        if (l & 0xC0) {
            return -1;                          /* reservierte Label-Form */
        }
        o += 1 + l;                             /* normales Label */
    }
    return -1;
}

/* Antwort-Handler (gebunden auf DNS_CLIENT_PORT). */
static void dns_input(netif_t *nif, ip4_addr_t src, uint16_t sport,
                      const uint8_t *d, uint16_t dlen)
{
    (void)nif; (void)src; (void)sport;
    int len = (int)dlen;
    if (!s_q.active || len < 12) {
        return;
    }
    uint16_t id = (uint16_t)((d[0] << 8) | d[1]);
    if (id != s_q.id) {
        return;                                 /* fremde/alte Antwort */
    }
    uint16_t flags = (uint16_t)((d[2] << 8) | d[3]);
    if (!(flags & 0x8000)) {
        return;                                 /* QR muss gesetzt sein (Antwort) */
    }
    if ((flags & 0x000F) != 0) {                /* RCODE != 0 -> NXDOMAIN etc. */
        finish(0, 0);
        return;
    }
    uint16_t qd = (uint16_t)((d[4] << 8) | d[5]);
    uint16_t an = (uint16_t)((d[6] << 8) | d[7]);

    if (qd < 1) {                               /* Antwort ohne Frage -> verwerfen */
        finish(0, 0);
        return;
    }
    int o = 12;
    /* Die ERSTE Frage MUSS unsere Anfrage echoen (QNAME + QTYPE A + QCLASS IN). Sonst
     * koennte eine fremde/gespoofte Antwort mit zufaellig passender ID akzeptiert werden. */
    o = qname_eq(d, len, o, s_q.name);
    if (o < 0 || o + 4 > len) {
        finish(0, 0);
        return;
    }
    if (((d[o] << 8) | d[o + 1]) != 1 || ((d[o + 2] << 8) | d[o + 3]) != 1) {
        finish(0, 0);                           /* QTYPE != A oder QCLASS != IN */
        return;
    }
    o += 4;
    for (int q = 1; q < qd; q++) {              /* etwaige weitere Fragen nur ueberspringen */
        o = skip_name(d, len, o);
        if (o < 0 || o + 4 > len) {
            finish(0, 0);
            return;
        }
        o += 4;
    }
    for (int a = 0; a < an; a++) {              /* Antworten durchgehen */
        o = skip_name(d, len, o);
        if (o < 0 || o + 10 > len) {
            finish(0, 0);
            return;
        }
        uint16_t type  = (uint16_t)((d[o] << 8) | d[o + 1]);
        uint16_t rdlen = (uint16_t)((d[o + 8] << 8) | d[o + 9]);
        o += 10;
        if (o + (int)rdlen > len) {
            finish(0, 0);
            return;
        }
        if (type == 1 && rdlen == 4) {          /* A-Record -> fertig */
            ip4_addr_t ip = IP4(d[o], d[o + 1], d[o + 2], d[o + 3]);
            finish(1, ip);
            return;
        }
        o += rdlen;                             /* z.B. CNAME -> weitersuchen */
    }
    finish(0, 0);                               /* keine A-Antwort gefunden */
}

/* Big-Net-Lock-Wrapper (T1.11) -- beide Funktionen haben viele fruehe returns. */
static int  dns_resolve_inner(netif_t *nif, ip4_addr_t server, const char *name, dns_result_fn cb);
static void dns_tick_inner(netif_t *nif);

int dns_resolve(netif_t *nif, ip4_addr_t server, const char *name, dns_result_fn cb)
{
    net_enter();
    int r = dns_resolve_inner(nif, server, name, cb);
    net_leave();
    return r;
}

static int dns_resolve_inner(netif_t *nif, ip4_addr_t server, const char *name, dns_result_fn cb)
{
    if (s_q.active) {
        return -1;                              /* nur eine Anfrage zur Zeit */
    }
    /* Namen VORAB pruefen (Laenge + Enkodierbarkeit), BEVOR Zustand gesetzt wird:
     * kein stilles Abschneiden langer Namen und -- entscheidend -- der cb feuert nie
     * synchron aus dns_resolve heraus (nur async via dns_tick/dns_input). Das schliesst
     * eine Reentranz-Rekursion aus, falls der cb bei Fehlschlag erneut aufloest. */
    int nl = 0;
    while (name[nl]) {
        nl++;
    }
    if (nl > DNS_NAME_MAX) {
        return -1;                              /* zu lang -> kein Truncate */
    }
    uint8_t probe[DNS_NAME_MAX + 2];
    if (encode_qname(name, probe, (int)sizeof(probe)) < 0) {
        return -1;                              /* unkodierbar (leeres Label etc.) */
    }
    if (!s_bound) {
        if (udp_bind(DNS_CLIENT_PORT, dns_input) != 0) {
            return -1;
        }
        s_bound = 1;
    }
    for (int i = 0; i <= nl; i++) {             /* inkl. Nullterminator */
        s_q.name[i] = name[i];
    }
    s_q.active  = 1;
    s_q.server  = server;
    s_q.cb      = cb;
    s_q.nif     = nif;
    s_q.retries = 0;
    /* Transaktions-ID: monotone Zeit gemischt -- ausreichend gegen versehentliche
     * Kollisionen (im SLIRP-Netz ist kein Off-Path-Angreifer relevant). */
    s_q.id = (uint16_t)(net_now_ms() * 2654435761u) ^ 0xA53Cu;
    send_query();
    return 0;
}

void dns_tick(netif_t *nif)
{
    net_enter();
    dns_tick_inner(nif);
    net_leave();
}

static void dns_tick_inner(netif_t *nif)
{
    (void)nif;
    if (!s_q.active) {
        return;
    }
    if (net_now_ms() - s_q.sent_ms < DNS_TIMEOUT_MS) {
        return;
    }
    if (++s_q.retries > DNS_MAX_RTX) {
        finish(0, 0);                           /* aufgeben */
        return;
    }
    send_query();
}
