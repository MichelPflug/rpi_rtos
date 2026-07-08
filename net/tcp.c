/*
 * net/tcp.c  --  Minimaler TCP (Server + Client)
 *
 * Umfang: LISTEN + aktiver Open (tcp_connect), Drei-Wege-Handshake, In-Order-Daten
 * mit kumulativem ACK, Retransmit unbestaetigter Daten (RTO), sendeseitige
 * Flusskontrolle (Peer-Empfangsfenster + Zero-Window-Persist), aktiver + passiver
 * Close, RST fuer unbekannte Segmente. Bewusst NICHT: Out-of-Order-Reassembly,
 * Fenster-Skalierung, SACK, Nagle/Delayed-ACK-Feinheiten.
 *
 * Sequenzraum: SYN und FIN belegen je 1. sndbuf haelt die unbestaetigten Bytes
 * [snd_una, snd_una+sndlen); davon sind [snd_una, snd_nxt) im Flug und
 * [snd_nxt, snd_una+sndlen) noch ungesendet (vom Peer-Fenster gebremst). Ein per
 * tcp_close angefordertes FIN wird erst gesendet, wenn snd_nxt == snd_una+sndlen
 * (Puffer geleert) -- so liegt seine Sequenznummer nie VOR ungesendeten Daten.
 */
#include <stdint.h>
#include "tcp.h"
#include "net.h"
#include "kmem.h"
#include "uart.h"

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_MSS      1460
#define TCP_SNDBUF   2048          /* Sendepuffer (begrenzt zugleich das angekuendigte Empfangsfenster) */
#define TCP_RCVBUF   2048          /* Empfangs-Reassembly-Puffer (out-of-order Segmente bis zur Luecke) */
#define TCP_MAX_PCB  4
#define TCP_MAX_LISTEN 4
#define TCP_RTO_MS   500           /* Anfangs-RTO vor der ersten RTT-Messung */
#define TCP_RTO_MIN  200           /* untere RTO-Schranke (adaptiv, Jacobson) */
#define TCP_RTO_MAX  60000         /* obere RTO-Schranke */
#define TCP_MAX_RTX  8
#define TCP_IDLE_MS  30000         /* Idle-ESTABLISHED nach 30 s ernten */
#define TCP_KEEPIDLE_MS  10000     /* Keepalive: Untaetigkeit vor der ersten Probe (RFC 1122) */
#define TCP_KEEPINTVL_MS 2000      /* Keepalive: Abstand zwischen Proben */
#define TCP_KEEPCNT  4             /* Keepalive: unbeantwortete Proben -> Peer gilt als tot */
#define TCP_SWS_MIN  ((TCP_MSS < (TCP_RCVBUF / 2)) ? TCP_MSS : (TCP_RCVBUF / 2))
                                   /* Silly-Window-Schwelle = min(MSS, RCVBUF/2), RFC 1122 4.2.3.3 */
#define TCP_ACK_DELAY_MS 200       /* Delayed-ACK: max. Wartezeit vor einem reinen ACK (RFC 1122) */
#define TCP_TIME_WAIT_MS 2000      /* verkuerztes 2MSL fuer den aktiven Close */
#define TCP_RCV_WSCALE 0           /* Shift, den WIR ankuendigen: rcvbuf (2 KiB) < 64 KiB -> 0 noetig
                                    * (RFC 7323); Senden der Option aktiviert das LESEN der skalierten
                                    * Peer-Fenster (snd_wscale) -- der beobachtbare Nutzen fuer uns. */
#define TCP_IW       (2 * TCP_MSS)  /* Initial Congestion Window (RFC 5681 3.1: 2..4 SMSS) */
#define TCP_INIT_SSTHRESH 65535    /* ssthresh anfangs hoch -> Slow Start bis zum ersten Verlust */

enum { ST_CLOSED, ST_SYN_SENT, ST_SYN_RCVD, ST_ESTABLISHED, ST_CLOSE_WAIT,
       ST_LAST_ACK, ST_FIN_WAIT_1, ST_FIN_WAIT_2, ST_TIME_WAIT };

typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;   /* 20 Byte (ohne Optionen) */

struct tcp_pcb {
    int         used;
    int         state;
    netif_t    *nif;
    ip4_addr_t  remote_ip;
    uint16_t    remote_port, local_port;
    uint32_t    iss;
    uint32_t    snd_una, snd_nxt;
    uint32_t    snd_wnd;            /* zuletzt vom Peer angekuendigtes Empfangsfenster (Bytes) */
    uint32_t    max_snd_wnd;        /* groesstes je vom Peer angekuendigtes Fenster (Sender-SWS) */
    uint32_t    snd_wl1, snd_wl2;   /* seq/ack des Segments, das snd_wnd zuletzt aktualisierte (RFC 793 3.7) */
    uint32_t    rcv_adv;            /* zuletzt angekuendigte rechte Fensterkante (Receiver-SWS, RFC 1122) */
    uint32_t    rcv_nxt;
    uint8_t     rcvbuf[TCP_RCVBUF]; /* Reassembly: empfangene Bytes ab rcv_nxt */
    uint8_t     rcvmark[TCP_RCVBUF];/* 1 = entsprechendes Byte vorhanden (auch out-of-order) */
    uint16_t    rcv_buffered;       /* Anzahl aktuell markierter Bytes (Empfangsfenster-Buchhaltung) */
    uint8_t     sack_ok;            /* SACK ausgehandelt (RFC 2018: SACK-Permitted in beiden SYN) */
    uint8_t     ts_ok;             /* TCP-Timestamps ausgehandelt (RFC 7323) */
    uint8_t     wscale_ok;         /* Window Scaling ausgehandelt (RFC 7323: WScale in beiden SYN) */
    uint8_t     snd_wscale;        /* Shift, den der PEER ankuendigte -> auf empfangene Fenster anwenden */
    uint32_t    ts_recent;         /* zuletzt empfangener TSval (wird als TSecr zurueckgeschickt) */
    uint32_t    srtt, rttvar, rto; /* geglaettete RTT, RTT-Varianz, Retransmit-Timeout (ms, Jacobson) */
    uint8_t     ack_pending;       /* Delayed-ACK: ein reines ACK steht aus (noch nicht huckepack) */
    uint64_t    ack_due_ms;        /* Zeitpunkt, ab dem das verzoegerte ACK spaetestens raus muss */
    uint32_t    cwnd, ssthresh;    /* Congestion Window / Slow-Start-Schwelle (RFC 5681, Bytes) */
    uint8_t     dupacks;           /* Zaehler aufeinanderfolgender Dup-ACKs (Fast Retransmit ab 3) */
    uint8_t     ka_probes;         /* Keepalive: unbeantwortete Proben (Reset bei Empfang) */
    uint64_t    ka_last_ms;        /* Keepalive: Zeitpunkt der letzten Probe */
    uint32_t    fin_seq;            /* Seq unseres FIN (LAST_ACK/FIN_WAIT_1) */
    int         fin_pending;        /* Close angefordert: FIN folgt, sobald sndbuf geleert ist */
    uint8_t     sndbuf[TCP_SNDBUF];
    uint8_t     sndsack[TCP_SNDBUF]; /* SACK-Scoreboard: 1 = dieses Byte (Offset ab snd_una) vom Peer SACKt */
    uint16_t    sndlen;
    uint64_t    rtx_ms;
    uint64_t    last_ms;           /* letzte Segment-Aktivitaet (Idle-Reaping) */
    int         rtx_count;
    tcp_recv_fn handler;
    tcp_connected_fn on_connect;   /* aktiver Open: Erfolg/Fehlschlag-Callback */
};

static struct tcp_pcb s_pcbs[TCP_MAX_PCB];
static struct { uint16_t port; tcp_recv_fn fn; } s_listen[TCP_MAX_LISTEN];
static uint32_t s_isn;
static uint16_t s_ephemeral = 49152;   /* naechster lokaler Port (dynamischer Bereich) */

static uint16_t next_ephemeral(void)
{
    uint16_t p = s_ephemeral++;
    if (s_ephemeral == 0) { s_ephemeral = 49152; }   /* Wrap im 49152..65535-Bereich */
    return p;
}

/* --- Sequenz-Vergleiche (modulo 2^32) --- */
static inline int seq_gt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) > 0; }
static inline int seq_lt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) < 0; }
static inline int seq_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_geq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static uint32_t gen_isn(void)
{
    s_isn += 0x9E3779B9u;
    return (uint32_t)(net_now_ms() * 1000u) + s_isn;
}

static uint16_t tcp_checksum(ip4_addr_t src, ip4_addr_t dst,
                             const uint8_t *seg, uint16_t len)
{
    uint8_t ph[12];
    ph[0] = (uint8_t)(src >> 24); ph[1] = (uint8_t)(src >> 16);
    ph[2] = (uint8_t)(src >> 8);  ph[3] = (uint8_t)src;
    ph[4] = (uint8_t)(dst >> 24); ph[5] = (uint8_t)(dst >> 16);
    ph[6] = (uint8_t)(dst >> 8);  ph[7] = (uint8_t)dst;
    ph[8] = 0; ph[9] = IPPROTO_TCP;
    ph[10] = (uint8_t)(len >> 8); ph[11] = (uint8_t)len;
    uint32_t s = csum_partial(ph, sizeof(ph), 0);
    s = csum_partial(seg, len, s);
    return csum_fold(s);
}

/* Sucht in den TCP-Optionen eines (untrusted) Segments die SACK-Permitted-Option (kind 4).
 * Bounds-sicher: bricht bei EOL/korrupter Laenge ab. payload zeigt auf den TCP-Header. */
static int syn_has_sack_perm(const uint8_t *payload, uint8_t hlen)
{
    uint16_t i = sizeof(tcp_hdr_t);
    while (i < hlen) {
        uint8_t kind = payload[i];
        if (kind == 0) { break; }                 /* End of Option List */
        if (kind == 1) { i++; continue; }         /* NOP */
        if (i + 2 > hlen) { break; }              /* Laengen-Byte fehlt */
        uint8_t len = payload[i + 1];
        if (len < 2 || i + len > hlen) { break; } /* korrupt -> abbrechen */
        if (kind == 4 && len == 2) { return 1; }  /* SACK-Permitted */
        i += len;
    }
    return 0;
}

/* Liest die SACK-Bloecke (kind 5) aus den TCP-Optionen eines (untrusted) Segments in blk
 * (Paare left,right, absolute Seq). Bounds-sicher. Liefert die Blockzahl (<= maxb). */
static int parse_sack(const uint8_t *payload, uint8_t hlen, uint32_t *blk, int maxb)
{
    int n = 0;
    uint16_t i = sizeof(tcp_hdr_t);
    while (i < hlen) {
        uint8_t kind = payload[i];
        if (kind == 0) { break; }
        if (kind == 1) { i++; continue; }
        if (i + 2 > hlen) { break; }
        uint8_t len = payload[i + 1];
        if (len < 2 || i + len > hlen) { break; }
        if (kind == 5 && len >= 10 && ((len - 2) % 8) == 0) {
            int nb = (len - 2) / 8;
            for (int b = 0; b < nb && n < maxb; b++) {
                const uint8_t *q = payload + i + 2 + b * 8;
                blk[n * 2]     = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                                 ((uint32_t)q[2] << 8)  | q[3];
                blk[n * 2 + 1] = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) |
                                 ((uint32_t)q[6] << 8)  | q[7];
                n++;
            }
        }
        i += len;
    }
    return n;
}

/* Liest die Timestamps-Option (kind 8, len 10) aus den (untrusted) TCP-Optionen: *tsval = der
 * Zeitstempel des Peers (spaeter als TSecr zurueckzuschicken), *tsecr = das Echo UNSERES
 * Zeitstempels (fuer die RTT-Messung). 1 = TS vorhanden. Bounds-sicher. */
static int parse_ts(const uint8_t *payload, uint8_t hlen, uint32_t *tsval, uint32_t *tsecr)
{
    uint16_t i = sizeof(tcp_hdr_t);
    while (i < hlen) {
        uint8_t kind = payload[i];
        if (kind == 0) { break; }
        if (kind == 1) { i++; continue; }
        if (i + 2 > hlen) { break; }
        uint8_t len = payload[i + 1];
        if (len < 2 || i + len > hlen) { break; }
        if (kind == 8 && len == 10) {
            const uint8_t *q = payload + i + 2;
            *tsval = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
            *tsecr = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
            return 1;
        }
        i += len;
    }
    return 0;
}

/* Liest die Window-Scale-Option (kind 3, len 3) aus einem (untrusted) SYN/SYN-ACK: *shift = der
 * vom Peer angekuendigte Shift, auf 14 gedeckelt (RFC 7323 2.3: groessere Werte als 14 behandeln).
 * 1 = vorhanden. Bounds-sicher (identischer Options-Walk wie parse_ts/parse_sack). */
static int parse_wscale(const uint8_t *payload, uint8_t hlen, uint8_t *shift)
{
    uint16_t i = sizeof(tcp_hdr_t);
    while (i < hlen) {
        uint8_t kind = payload[i];
        if (kind == 0) { break; }
        if (kind == 1) { i++; continue; }
        if (i + 2 > hlen) { break; }
        uint8_t len = payload[i + 1];
        if (len < 2 || i + len > hlen) { break; }
        if (kind == 3 && len == 3) {
            uint8_t s = payload[i + 2];
            *shift = (s > 14) ? 14 : s;
            return 1;
        }
        i += len;
    }
    return 0;
}

/* RTT-Messung -> geglaettetes SRTT/RTTVAR + adaptives RTO (Jacobson/Karels, RFC 6298). */
static void tcp_rtt_update(struct tcp_pcb *p, uint32_t rtt)
{
    if (rtt < 1) { rtt = 1; }                     /* ms-Aufloesung: RTT=0 (Loopback) als 1 werten */
    if (rtt > TCP_RTO_MAX) { rtt = TCP_RTO_MAX; } /* boesartiges TSecr -> riesige RTT deckeln: haelt
                                                   * srtt/rttvar klein (kein uint32-Ueberlauf von
                                                   * 7*srtt bzw. 4*rttvar, kein srtt->0-Poisoning) */
    if (p->srtt == 0) {                           /* erste Messung */
        p->srtt   = rtt;
        p->rttvar = rtt / 2;
    } else {
        uint32_t d = (p->srtt > rtt) ? (p->srtt - rtt) : (rtt - p->srtt);
        p->rttvar = (3 * p->rttvar + d) / 4;      /* RTTVAR = 3/4 RTTVAR + 1/4 |SRTT-RTT| */
        p->srtt   = (7 * p->srtt + rtt) / 8;      /* SRTT   = 7/8 SRTT   + 1/8 RTT */
    }
    uint32_t rto = p->srtt + 4 * p->rttvar;
    if (rto < TCP_RTO_MIN) { rto = TCP_RTO_MIN; }
    if (rto > TCP_RTO_MAX) { rto = TCP_RTO_MAX; }
    p->rto = rto;                                 /* frisches RTO -> hebt evtl. Backoff auf */
}

/* Exponentielles Backoff nach einem Retransmit (Karn): RTO verdoppeln bis TCP_RTO_MAX. Eine
 * erfolgreiche RTT-Messung (tcp_rtt_update) setzt es danach wieder auf SRTT+4*RTTVAR. */
static void tcp_backoff(struct tcp_pcb *p)
{
    uint32_t r = (p->rto ? p->rto : TCP_RTO_MS) * 2;
    p->rto = (r > TCP_RTO_MAX) ? TCP_RTO_MAX : r;
}

/* Baut bis zu maxblk SACK-Bloecke aus dem Reassembly-Puffer: jede zusammenhaengende belegte
 * Strecke oberhalb von rcv_nxt wird ein Block [left, right). Liefert die Blockzahl. */
static int build_sack_blocks(struct tcp_pcb *p, uint32_t *blk, int maxblk)
{
    int n = 0;
    uint16_t i = 0;
    while (i < TCP_RCVBUF && n < maxblk) {
        if (!p->rcvmark[i]) { i++; continue; }
        uint16_t s = i;
        while (i < TCP_RCVBUF && p->rcvmark[i]) { i++; }
        blk[n * 2]     = p->rcv_nxt + s;          /* linke Kante (absolute Seq) */
        blk[n * 2 + 1] = p->rcv_nxt + i;          /* rechte Kante = 1 hinter dem letzten Byte */
        n++;
    }
    return n;
}

/* Ein TCP-Segment aufbauen und senden. seq explizit; ack = pcb->rcv_nxt. */
static void seg_out(struct tcp_pcb *p, uint8_t flags, uint32_t seq, int with_mss,
                    const uint8_t *data, uint16_t dlen)
{
    static uint8_t buf[NET_FRAME_MAX];
    tcp_hdr_t *th = (tcp_hdr_t *)buf;
    uint8_t hlen = sizeof(tcp_hdr_t);

    th->src_port = htons(p->local_port);
    th->dst_port = htons(p->remote_port);
    th->seq = htonl(seq);
    th->ack = htonl(p->rcv_nxt);
    th->flags = flags;
    /* Empfangsfenster = freier Reassembly-Puffer, zusaetzlich durch den freien Echo-
     * Sendepuffer gedeckelt (Backpressure): ein in-order abgeliefertes Daten-Stueck passt
     * so immer in den Echo-Sendepuffer -> kein stiller Echo-Verlust (siehe reasm_insert). */
    {
        uint16_t rwin = (p->rcv_buffered < TCP_RCVBUF)
                            ? (uint16_t)(TCP_RCVBUF - p->rcv_buffered) : 0;
        uint16_t swin = (p->sndlen < TCP_SNDBUF)
                            ? (uint16_t)(TCP_SNDBUF - p->sndlen) : 0;
        uint32_t avail    = (rwin < swin) ? rwin : swin;     /* Fenster, das wir anbieten koennten */
        uint32_t new_edge = p->rcv_nxt + avail;
        /* Receiver-SWS-Avoidance (RFC 1122 4.2.3.3): die rechte Fensterkante NICHT um einen kleinen
         * Betrag (< min(MSS, RCVBUF/2)) nach rechts schieben -- eine winzige Fensteroeffnung lockt
         * den Sender sonst zu Klein-Segmenten. Schrumpfen (Puffer fuellt sich) ist erlaubt; eine
         * grosse Oeffnung (>= Schwelle) wird sofort angekuendigt. */
        if (seq_gt(new_edge, p->rcv_adv) && (new_edge - p->rcv_adv) < TCP_SWS_MIN) {
            /* kleine Oeffnung -> alte Kante halten (weniger als avail ankuendigen). */
        } else {
            p->rcv_adv = new_edge;
        }
        uint32_t win = seq_gt(p->rcv_adv, p->rcv_nxt) ? (p->rcv_adv - p->rcv_nxt) : 0;
        th->window = htons((uint16_t)win);
    }
    th->checksum = 0;
    th->urgent = 0;

    /* TCP-Optionen aufbauen: SYN -> MSS (+SACK-Permitted) (+Timestamps); sonst (+Timestamps)
     * (+SACK-Bloecke). Timestamps (RFC 7323) auf ALLEN Segmenten, sobald ausgehandelt. */
    uint8_t *opt = buf + sizeof(tcp_hdr_t);
    int olen = 0;
    uint32_t tsval = (uint32_t)net_now_ms();
    if (with_mss) {
        opt[olen++] = 2; opt[olen++] = 4;                        /* MSS */
        opt[olen++] = (uint8_t)(TCP_MSS >> 8); opt[olen++] = (uint8_t)(TCP_MSS & 0xFF);
        if (p->sack_ok) {                                        /* SACK-Permitted nur auf SYN/SYN-ACK */
            opt[olen++] = 1; opt[olen++] = 1;                    /* 2x NOP (Ausrichtung) */
            opt[olen++] = 4; opt[olen++] = 2;                    /* SACK-Permitted (kind 4, len 2) */
        }
        if (p->wscale_ok) {                                      /* Window Scale nur auf SYN/SYN-ACK */
            opt[olen++] = 1;                                     /* NOP (Ausrichtung) */
            opt[olen++] = 3; opt[olen++] = 3;                    /* WScale (kind 3, len 3) */
            opt[olen++] = TCP_RCV_WSCALE;                        /* Shift, den WIR ankuendigen */
        }
    }
    if (p->ts_ok) {                                              /* Timestamps: TSval + TSecr */
        opt[olen++] = 1; opt[olen++] = 1;                        /* 2x NOP (8-Byte-Ausrichtung) */
        opt[olen++] = 8; opt[olen++] = 10;                       /* Timestamps (kind 8, len 10) */
        opt[olen++] = (uint8_t)(tsval >> 24); opt[olen++] = (uint8_t)(tsval >> 16);
        opt[olen++] = (uint8_t)(tsval >> 8);  opt[olen++] = (uint8_t)tsval;
        opt[olen++] = (uint8_t)(p->ts_recent >> 24); opt[olen++] = (uint8_t)(p->ts_recent >> 16);
        opt[olen++] = (uint8_t)(p->ts_recent >> 8);  opt[olen++] = (uint8_t)p->ts_recent;
    }
    if (!with_mss && p->sack_ok && (flags & TCP_ACK)) {          /* SACK-Bloecke nur auf non-SYN ACK */
        uint32_t blk[2 * 3];
        int nb = build_sack_blocks(p, blk, 3);
        if (nb > 0) {
            opt[olen++] = 1; opt[olen++] = 1;                    /* 2x NOP -> 8-Byte-Bloecke ausgerichtet */
            opt[olen++] = 5; opt[olen++] = (uint8_t)(2 + 8 * nb);/* SACK (kind 5) + Laenge */
            for (int i = 0; i < nb; i++) {
                uint32_t le = blk[i * 2], re = blk[i * 2 + 1];
                opt[olen++] = (uint8_t)(le >> 24); opt[olen++] = (uint8_t)(le >> 16);
                opt[olen++] = (uint8_t)(le >> 8);  opt[olen++] = (uint8_t)le;
                opt[olen++] = (uint8_t)(re >> 24); opt[olen++] = (uint8_t)(re >> 16);
                opt[olen++] = (uint8_t)(re >> 8);  opt[olen++] = (uint8_t)re;
            }
        }
    }
    while (olen & 3) { opt[olen++] = 1; }                        /* auf 4-Byte-Grenze NOP-auffuellen */
    hlen = (uint8_t)(sizeof(tcp_hdr_t) + olen);
    th->data_off = (uint8_t)((hlen / 4) << 4);

    if (dlen && data) {
        memcpy(buf + hlen, data, dlen);
    }
    uint16_t total = (uint16_t)(hlen + dlen);
    th->checksum = htons(tcp_checksum(p->nif->ip, p->remote_ip, buf, total));
    ip_send(p->nif, p->remote_ip, IPPROTO_TCP, buf, total);
    p->ack_pending = 0;   /* dieses Segment traegt th->ack=rcv_nxt -> ausstehendes ACK huckepack erledigt */
}

/* Delayed-ACK (RFC 1122): ein reines ACK nicht sofort schicken, sondern vormerken -- ein bald
 * folgendes ausgehendes Segment (Echo) traegt das ACK huckepack; sonst flusht tcp_tick es nach
 * TCP_ACK_DELAY_MS. Spart das redundante reine ACK nach jedem empfangenen Segment. */
static void tcp_ack_schedule(struct tcp_pcb *p)
{
    if (!p->ack_pending) {
        p->ack_pending = 1;
        p->ack_due_ms  = net_now_ms() + TCP_ACK_DELAY_MS;
    }
}

/* RST fuer ein Segment ohne zugehoerige Verbindung. */
static void send_rst(netif_t *nif, ip4_addr_t src, ip4_addr_t dst,
                     uint16_t sport, uint16_t dport, uint8_t flags,
                     uint32_t seg_seq, uint32_t seg_ack, uint16_t datalen)
{
    /* static (nicht auf dem Stack): struct tcp_pcb traegt jetzt 2x TCP_RCVBUF Puffer --
     * eine Stack-Kopie waere ~6 KB. Net laeuft single-threaded im Poll-Loop, daher ok. */
    static struct tcp_pcb tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.nif = nif;
    tmp.remote_ip = src;
    tmp.local_port = dport;
    tmp.remote_port = sport;

    uint8_t rflags;
    uint32_t seq;
    if (flags & TCP_ACK) {
        seq = seg_ack;
        tmp.rcv_nxt = 0;
        rflags = TCP_RST;
    } else {
        seq = 0;
        tmp.rcv_nxt = seg_seq + datalen +
                      ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
        rflags = TCP_RST | TCP_ACK;
    }
    (void)dst;
    seg_out(&tmp, rflags, seq, 0, 0, 0);
}

static struct tcp_pcb *find_pcb(ip4_addr_t rip, uint16_t rport, uint16_t lport)
{
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        if (s_pcbs[i].used && s_pcbs[i].remote_ip == rip &&
            s_pcbs[i].remote_port == rport && s_pcbs[i].local_port == lport) {
            return &s_pcbs[i];
        }
    }
    return 0;
}

static struct tcp_pcb *alloc_pcb(void)
{
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        if (!s_pcbs[i].used) {
            memset(&s_pcbs[i], 0, sizeof(s_pcbs[i]));
            s_pcbs[i].used = 1;
            return &s_pcbs[i];
        }
    }
    return 0;
}

static tcp_recv_fn find_listener(uint16_t port)
{
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (s_listen[i].port == port && s_listen[i].fn) {
            return s_listen[i].fn;
        }
    }
    return 0;
}

int tcp_listen(uint16_t port, tcp_recv_fn handler)
{
    net_enter();
    int r = -1;
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (s_listen[i].port == 0) {
            s_listen[i].port = port;
            s_listen[i].fn = handler;
            r = 0;
            break;
        }
    }
    net_leave();
    return r;
}

/* Listener wieder freigeben (Port-Slot zurueckgeben). 0 = entfernt, -1 = nicht gefunden. */
int tcp_unlisten(uint16_t port)
{
    net_enter();
    int r = -1;
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (s_listen[i].port == port && s_listen[i].fn) {
            s_listen[i].port = 0;
            s_listen[i].fn = 0;
            r = 0;
            break;
        }
    }
    net_leave();
    return r;
}

tcp_pcb_t *tcp_connect(netif_t *nif, ip4_addr_t rip, uint16_t rport,
                       tcp_connected_fn on_conn, tcp_recv_fn on_recv)
{
    net_enter();
    struct tcp_pcb *p = alloc_pcb();
    if (!p) {
        net_leave();
        return 0;                              /* PCB-Pool voll */
    }
    p->nif         = nif;
    p->remote_ip   = rip;
    p->remote_port = rport;
    p->local_port  = next_ephemeral();
    p->handler     = on_recv;
    p->on_connect  = on_conn;
    p->iss         = gen_isn();
    p->snd_una     = p->iss;
    p->rcv_nxt     = 0;                         /* erst nach SYN-ACK bekannt */
    p->state       = ST_SYN_SENT;
    p->sack_ok     = 1;                         /* SACK aktiv anbieten (SYN-ACK bestaetigt/widerruft) */
    p->ts_ok       = 1;                         /* Timestamps aktiv anbieten */
    p->wscale_ok   = 1;                         /* Window Scaling aktiv anbieten (SYN-ACK bestaetigt/widerruft) */
    p->cwnd        = TCP_IW;                     /* Congestion Control: Initial Window (RFC 5681) */
    p->ssthresh    = TCP_INIT_SSTHRESH;
    p->rto         = TCP_RTO_MS;                /* Anfangs-RTO bis zur ersten RTT-Messung */
    p->last_ms     = net_now_ms();
    seg_out(p, TCP_SYN, p->iss, 1 /* MSS */, 0, 0);   /* aktives SYN (MSS + SACK-Perm + TS + WScale) */
    p->snd_nxt     = p->iss + 1;                /* SYN belegt 1 */
    p->rtx_ms      = net_now_ms();
    p->rtx_count   = 0;
    net_leave();
    return p;
}

/* Gepufferte, noch UNGESENDETE Daten senden, soweit das vom Peer angekuendigte
 * Empfangsfenster (snd_wnd) es zulaesst -- echte Sende-Backpressure. sndbuf haelt die
 * unbestaetigten Bytes [snd_una, snd_una+sndlen); davon sind [snd_una, snd_nxt) im Flug
 * (gesendet, unbestaetigt) und [snd_nxt, snd_una+sndlen) noch ungesendet. Wir senden nur
 * so viel, dass die im Flug befindlichen Bytes das Peer-Fenster nicht ueberschreiten. */
/* Effektive Daten-MSS: laesst Platz fuer die SACK-Optionen, die seg_out auf einem ACK
 * anhaengt, wenn SACK ausgehandelt ist UND out-of-order Daten gepuffert sind. Sonst wuerden
 * Daten + bis zu 28 Byte SACK-Option die per MSS versprochene MTU sprengen (RFC 6691: MSS
 * zaehlt nur Daten). Worst case = 3 SACK-Bloecke (NOP NOP + kind/len + 3*8) = 28 Byte. */
static uint16_t tcp_eff_mss(const struct tcp_pcb *p)
{
    uint16_t opt = 0;
    if (p->ts_ok)                              { opt += 12; }   /* NOP NOP + TS(10) */
    if (p->sack_ok && p->rcv_buffered > 0)     { opt += 28; }   /* NOP NOP + SACK (worst case 3 Bloecke) */
    return (uint16_t)(TCP_MSS - opt);
}

/* Peer-Empfangsfenster uebernehmen -- aber nur aus einem NEUEREN Segment (RFC 793 3.7):
 * seq fortgeschritten, ODER gleiche seq + fortgeschrittenes ack. So schrumpft ein altes/
 * dupliziertes/umgeordnetes ACK das Sendefenster NICHT (sonst drosselt eine veraltete
 * Fensterangabe den Sendepfad bis zum naechsten frischen ACK). */
static void update_snd_wnd(struct tcp_pcb *p, uint32_t seg_seq, uint32_t seg_ack, uint32_t win)
{
    if (seq_gt(seg_seq, p->snd_wl1) ||
        (seg_seq == p->snd_wl1 && seq_geq(seg_ack, p->snd_wl2))) {
        p->snd_wnd = win;
        p->snd_wl1 = seg_seq;
        p->snd_wl2 = seg_ack;
        if (win > p->max_snd_wnd) { p->max_snd_wnd = win; }   /* groesstes Fenster (Sender-SWS) */
    }
}

/* Sendet ein durch tcp_close angefordertes FIN, sobald ALLE gepufferten Daten gesendet
 * wurden (snd_nxt == snd_una + sndlen). Das FIN liegt damit hinter dem letzten Datenbyte;
 * sonst wuerde seine Sequenznummer mit fenster-gebremsten, noch ungesendeten Daten
 * kollidieren (stille Daten-Truncation + Sequenz-Inkohaerenz beim Close).
 * ESTABLISHED -> FIN_WAIT_1 (aktiver Close), CLOSE_WAIT -> LAST_ACK (passiver Close). */
static void tcp_try_fin(struct tcp_pcb *p)
{
    if (!p->fin_pending) {
        return;
    }
    if (p->snd_nxt != p->snd_una + p->sndlen) {
        return;                                  /* Sendepuffer noch nicht geleert */
    }
    p->fin_seq     = p->snd_nxt;
    seg_out(p, TCP_FIN | TCP_ACK, p->snd_nxt, 0, 0, 0);
    p->snd_nxt    += 1;                           /* FIN belegt 1 */
    p->fin_pending = 0;
    p->state       = (p->state == ST_CLOSE_WAIT) ? ST_LAST_ACK : ST_FIN_WAIT_1;
    p->rtx_ms      = net_now_ms();
    p->rtx_count   = 0;
}

static void tcp_output(struct tcp_pcb *p)
{
    if (p->state != ST_ESTABLISHED && p->state != ST_CLOSE_WAIT) {
        return;
    }
    uint32_t inflight = p->snd_nxt - p->snd_una;
    if (inflight > p->sndlen) {
        return;                                 /* Schutz (sollte nicht vorkommen) */
    }
    uint16_t unsent = (uint16_t)(p->sndlen - inflight);
    /* Sendefenster = min(Peer-Empfangsfenster, Congestion Window) minus Bytes im Flug (RFC 5681):
     * das Netz (cwnd) UND der Empfaenger (snd_wnd) begrenzen gemeinsam, wie viel unterwegs sein darf. */
    uint32_t cwin   = (p->cwnd < p->snd_wnd) ? p->cwnd : p->snd_wnd;
    uint32_t usable = (cwin > inflight) ? (cwin - inflight) : 0;
    uint16_t mss = tcp_eff_mss(p);                  /* Daten-MSS abzgl. evtl. SACK-Optionen */
    int sent_any = 0;
    while (unsent > 0 && usable > 0) {
        uint32_t infl  = p->snd_nxt - p->snd_una;
        /* Wie viel koennten wir jetzt senden (ungesendet, MSS-, fenster-begrenzt)? */
        uint16_t chunk = unsent;
        if (chunk > mss)    { chunk = mss; }
        if (chunk > usable) { chunk = (uint16_t)usable; }
        /* Nagle (RFC 896) + Sender-SWS-Avoidance (RFC 1122 4.2.3.4): ein kleines Segment
         * zurueckhalten, solange unbestaetigte Daten im Flug sind -- "klein" = weniger als
         * min(eff_mss, groesstes je angekuendigtes Peer-Fenster / 2). So wartet der Sender auf ein
         * volles Segment ODER ein halb geoeffnetes Fenster, statt bei jeder winzigen Fenster-
         * oeffnung ein Klein-Segment zu dribbeln. inflight>0 macht es deadlock-frei (ein ACK kommt
         * und triggert die Neubewertung); bei anstehendem Close (fin_pending) trotzdem flushen. */
        uint32_t sws = (mss < p->max_snd_wnd / 2) ? mss : p->max_snd_wnd / 2;
        if (chunk < sws && infl > 0 && !p->fin_pending) {
            break;
        }
        uint16_t off = (uint16_t)(p->snd_nxt - p->snd_una);     /* Offset im sndbuf */
        seg_out(p, TCP_ACK | TCP_PSH, p->snd_nxt, 0, p->sndbuf + off, chunk);
        p->snd_nxt += chunk;
        unsent     = (uint16_t)(unsent - chunk);
        usable    -= chunk;
        sent_any = 1;
    }
    if (sent_any) {
        p->rtx_ms = net_now_ms();
        p->rtx_count = 0;
    }
    tcp_try_fin(p);          /* angefordertes FIN nachreichen, falls der Puffer nun geleert ist */
}

int tcp_write(struct tcp_pcb *p, const uint8_t *data, uint16_t len)
{
    net_enter();   /* app- UND callback-getrieben (z.B. tcp_echo aus tcp_input) -> reentrant */
    if (!p || (p->state != ST_ESTABLISHED && p->state != ST_CLOSE_WAIT)) {
        net_leave();
        return -1;
    }
    if (len == 0) {
        net_leave();
        return 0;
    }
    if (p->sndlen + len > TCP_SNDBUF) {
        len = (uint16_t)(TCP_SNDBUF - p->sndlen);   /* Rest verwerfen (Aufrufer prueft Rueckgabe) */
    }
    if (len == 0) {
        net_leave();
        return 0;
    }
    memcpy(p->sndbuf + p->sndlen, data, len);
    p->sndlen += len;
    tcp_output(p);                                   /* senden, soweit das Peer-Fenster es zulaesst */
    if (p->snd_nxt == p->snd_una && p->sndlen > 0) {
        /* Fenster zu -> nichts gesendet; Persist-Timer setzen, damit tcp_tick nach dem RTO
         * eine Probe schickt und ein Fenster-Update provoziert (statt zu verhungern). */
        p->rtx_ms = net_now_ms();
    }
    net_leave();
    return len;
}

uint32_t tcp_snd_wnd(const struct tcp_pcb *p)
{
    net_enter();
    uint32_t w = (p && p->used) ? p->snd_wnd : 0;
    net_leave();
    return w;
}

#ifdef RTOS_SELFTEST
/* Test-Zugriff (white-box) auf die Congestion-Control-Zustaende -- der Loopback-Test prueft die
 * cwnd/ssthresh-Uebergaenge (Slow Start / RTO-Kollaps / Fast Recovery), die mit den kleinen
 * 2-KiB-Puffern nicht rein am Draht beobachtbar sind. NUR im Selbsttest kompiliert (der einzige
 * Aufrufer tcp_looptest.c baut auch nur dort) -> kein toter Debug-Export im RC-Produktions-Image. */
uint32_t tcp_dbg_cwnd(const struct tcp_pcb *p)     { return (p && p->used) ? p->cwnd : 0; }
uint32_t tcp_dbg_ssthresh(const struct tcp_pcb *p) { return (p && p->used) ? p->ssthresh : 0; }
#endif

int tcp_sndbuf_free(const struct tcp_pcb *p)
{
    net_enter();
    int r = 0;
    if (p && p->used &&
        (p->state == ST_ESTABLISHED || p->state == ST_CLOSE_WAIT)) {
        r = (p->sndlen < TCP_SNDBUF) ? (int)(TCP_SNDBUF - p->sndlen) : 0;
    }
    net_leave();
    return r;
}

void tcp_close(struct tcp_pcb *p)
{
    net_enter();   /* app- UND callback-getrieben (z.B. client_recv aus tcp_input) -> reentrant */
    if (!p || !p->used) {
        net_leave();
        return;
    }
    /* Aktiver Close nur aus ESTABLISHED. Das FIN MUSS hinter alle gepufferten Daten --
     * unter Sende-Flusskontrolle koennen noch ungesendete (fenster-gebremste) Bytes in
     * [snd_nxt, snd_una+sndlen) liegen; ein FIN bei snd_nxt wuerde deren Sequenzraum
     * ueberschreiben und sie still verwerfen. Daher Close nur vormerken: tcp_try_fin sendet
     * das FIN sofort, falls der Puffer bereits leer ist, sonst sobald tcp_output ihn geleert
     * hat (-> dann ESTABLISHED -> FIN_WAIT_1). */
    if (p->state == ST_ESTABLISHED) {
        p->fin_pending = 1;
        tcp_try_fin(p);
        if (p->fin_pending) {
            p->rtx_ms = net_now_ms();          /* RTO/Persist laeuft -> Puffer leert sich, dann FIN */
        }
    }
    /* CLOSE_WAIT->LAST_ACK macht der passive Close-Pfad bereits selbst; andere
     * Zustaende: nichts zu tun. */
    net_leave();
}

/* Empfangs-Reassembly. Traegt [seq, seq+len) in den Reassembly-Puffer ein (clippt auf das
 * Fenster [rcv_nxt, rcv_nxt+TCP_RCVBUF) -- aelteres/jenseitiges wird verworfen, der Peer
 * wiederholt), liefert dann den zusammenhaengenden Praefix ab rcv_nxt in EINEM handler()-
 * Aufruf ab und schiebt Puffer+Markierungen nach. So werden out-of-order eintreffende
 * Segmente gepuffert und erst IN REIHENFOLGE zugestellt -- der Peer muss eine bereits
 * empfangene Luecke nicht erneut als Ganzes senden. Liefert die Zahl neu in-order
 * zugestellter Bytes (= rcv_nxt-Vorschub; 0, wenn das Segment nur eine Luecke fuellt). */
static int reasm_insert(struct tcp_pcb *p, uint32_t seq, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint32_t s = seq + i;
        if (seq_lt(s, p->rcv_nxt)) {
            continue;                                   /* schon zugestellt (alt/Dup) */
        }
        if (seq_geq(s, p->rcv_nxt + TCP_RCVBUF)) {
            break;                                      /* jenseits des Fensters -> verwerfen */
        }
        uint32_t off = s - p->rcv_nxt;                  /* 0 .. TCP_RCVBUF-1 */
        if (!p->rcvmark[off]) {
            p->rcvmark[off] = 1;
            p->rcv_buffered++;
        }
        p->rcvbuf[off] = data ? data[i] : 0;
    }
    uint16_t run = 0;
    while (run < TCP_RCVBUF && p->rcvmark[run]) {
        run++;                                          /* zusammenhaengender Praefix ab rcv_nxt */
    }
    if (run == 0) {
        return 0;                                       /* nur eine Luecke gefuellt, nichts in-order */
    }
    /* rcv_nxt/rcv_buffered UND die Markierungen VOR dem Handler aktualisieren: ein vom Handler
     * (Echo) gesendetes Segment traegt dann das korrekte kumulative ACK UND konsistente SACK-
     * Bloecke -- die gerade abgelieferten Marken sind weg, verbleibende Out-of-Order-Marken liegen
     * am korrekten Offset zur NEUEN rcv_nxt. Der rcvbuf-INHALT bleibt bis NACH dem Handler bei
     * [0..run) (build_sack_blocks liest nur rcvmark, nicht rcvbuf). */
    p->rcv_nxt     += run;
    p->rcv_buffered = (uint16_t)(p->rcv_buffered - run);
    memmove(p->rcvmark, p->rcvmark + run, TCP_RCVBUF - run);
    memset(p->rcvmark + (TCP_RCVBUF - run), 0, run);
    if (p->handler) {
        p->handler(p, p->rcvbuf, run);                  /* in-order Daten an die App (Echo) */
    }
    memmove(p->rcvbuf, p->rcvbuf + run, TCP_RCVBUF - run);
    return (int)run;
}

void tcp_input(netif_t *nif, ip4_addr_t src, ip4_addr_t dst, uint8_t *payload,
               uint16_t len)
{
    if (len < sizeof(tcp_hdr_t)) {
        return;
    }
    tcp_hdr_t *th = (tcp_hdr_t *)payload;
    uint8_t hlen = (uint8_t)((th->data_off >> 4) * 4);
    if (hlen < sizeof(tcp_hdr_t) || hlen > len) {
        return;
    }
    if (tcp_checksum(src, dst, payload, len) != 0) {
        return;
    }

    uint16_t sport = ntohs(th->src_port);
    uint16_t dport = ntohs(th->dst_port);
    uint32_t seg_seq = ntohl(th->seq);
    uint32_t seg_ack = ntohl(th->ack);
    uint8_t  flags = th->flags;
    uint16_t datalen = (uint16_t)(len - hlen);
    uint8_t *dataptr = payload + hlen;

    struct tcp_pcb *p = find_pcb(src, sport, dport);

    /* Reconnect: ein reines SYN auf eine bestehende SERVER-Instanz verwirft diese
     * (alter PCB wird frei, neue Verbindung entsteht im !p-Pfad). NUR fuer Server-
     * PCBs (kein on_connect) -- ein aktiver Client-PCB (SYN_SENT/ESTABLISHED) darf
     * durch ein reines SYN (simultaneous open / Stray-/Spoof-SYN auf den ephemeren
     * Port) NICHT still und ohne on_connect-Callback verworfen werden. */
    if (p && !p->on_connect && (flags & TCP_SYN) && !(flags & TCP_ACK)) {
        p->used = 0;
        p = 0;
    }

    if (!p) {
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            tcp_recv_fn h = find_listener(dport);
            if (!h) {
                send_rst(nif, src, dst, sport, dport, flags, seg_seq, seg_ack, datalen);
                return;
            }
            p = alloc_pcb();
            if (!p) {
                return;   /* keine Ressourcen -> Client wiederholt SYN */
            }
            p->nif = nif;
            p->remote_ip = src;
            p->remote_port = sport;
            p->local_port = dport;
            p->handler = h;
            p->iss = gen_isn();
            p->snd_una = p->iss;
            p->snd_wnd = ntohs(th->window);    /* Empfangsfenster des Peers (SYN) */
            p->max_snd_wnd = p->snd_wnd;       /* Sender-SWS: groesstes gesehenes Fenster */
            p->snd_wl1 = seg_seq; p->snd_wl2 = seg_ack;   /* Fenster-Update-Basis (RFC 793 3.7) */
            p->rcv_nxt = seg_seq + 1;          /* SYN belegt 1 */
            p->rcv_adv = p->rcv_nxt;           /* Receiver-SWS: rechte Kante wird beim SYN-ACK gesetzt */
            p->sack_ok = (uint8_t)syn_has_sack_perm(payload, hlen);  /* nur wenn der Peer es anbot */
            p->cwnd    = TCP_IW;               /* Congestion Control: Initial Window (RFC 5681) */
            p->ssthresh = TCP_INIT_SSTHRESH;
            p->rto     = TCP_RTO_MS;
            uint32_t sv = 0, se = 0;           /* Timestamps: nur wenn der Peer sie im SYN anbot */
            if (parse_ts(payload, hlen, &sv, &se)) { p->ts_ok = 1; p->ts_recent = sv; }
            uint8_t ws = 0;                    /* Window Scaling: nur wenn der Peer es im SYN anbot */
            if (parse_wscale(payload, hlen, &ws)) { p->wscale_ok = 1; p->snd_wscale = ws; }
            p->state = ST_SYN_RCVD;
            p->last_ms = net_now_ms();
            seg_out(p, TCP_SYN | TCP_ACK, p->iss, 1, 0, 0);   /* SYN-ACK: MSS (+SACK-Perm) (+TS) (+WScale) */
            p->snd_nxt = p->iss + 1;
            p->rtx_ms = net_now_ms();
            p->rtx_count = 0;
        } else if (!(flags & TCP_RST)) {
            send_rst(nif, src, dst, sport, dport, flags, seg_seq, seg_ack, datalen);
        }
        return;
    }

    p->last_ms = net_now_ms();             /* Aktivitaet vermerken (Idle-Reaping) */
    p->ka_probes = 0;                      /* Peer lebt -> Keepalive-Probenzaehler zuruecksetzen */

    if (flags & TCP_RST) {
        if (p->state == ST_SYN_SENT) {
            /* RFC 793: ein RST in SYN_SENT nur akzeptieren, wenn es unser SYN
             * bestaetigt (ACK gesetzt UND seg_ack == iss+1) -> Connection Refused.
             * Ein blindes/gespooftes RST ohne passendes ACK wird verworfen. */
            if ((flags & TCP_ACK) && seg_ack == p->iss + 1) {
                if (p->on_connect) { p->on_connect(p, 0); }
                p->used = 0;
            }
            return;
        }
        /* Nur in-window RST (seq == rcv_nxt) honorieren (RFC-5961-naehe). */
        if (seg_seq == p->rcv_nxt) {
            p->used = 0;
        }
        return;
    }

    /* PAWS (RFC 7323 5): in synchronisierten Zustaenden ein Segment mit einem AELTEREN Timestamp
     * (SEG.TSval < TS.Recent, mod-2^32 vorzeichenbehaftet) als alt/dupliziert verwerfen -- aber ein
     * aktuelles ACK schicken, damit der Peer resynchronisiert (RST wurde oben schon behandelt).
     * Schuetzt gegen wrapped sequences + alte Duplikate mit zufaellig passender Sequenznummer.
     * Die 24-Tage-Ausnahme (RFC 7323 5.5) ist NICHT implementiert und praktisch irrelevant: eine
     * ruhende Verbindung wird nach TCP_IDLE_MS (30 s) geerntet, und jedes in-order Datenbyte frischt
     * ts_recent weit innerhalb des ~24,8-Tage-TSval-Wraps auf. Der schmale Rest (eine >24,8 Tage
     * DURCHGEHEND aktive Verbindung, die NUR reine ACKs/OOO/Persist ohne in-order Daten traegt) ist
     * eine bewusste Vereinfachung. */
    if (p->ts_ok && p->state >= ST_ESTABLISHED) {
        uint32_t pv = 0, pe = 0;
        if (parse_ts(payload, hlen, &pv, &pe) && seq_lt(pv, p->ts_recent)) {
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);
            return;
        }
    }

    /* --- Aktiver Open: SYN-ACK auf unser SYN -> ESTABLISHED --- */
    if (p->state == ST_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            if (seg_ack != p->iss + 1) {       /* falsches ACK -> RST + verwerfen */
                send_rst(nif, src, dst, sport, dport, flags, seg_seq, seg_ack, datalen);
                if (p->on_connect) { p->on_connect(p, 0); }
                p->used = 0;
                return;
            }
            p->snd_una = seg_ack;              /* = iss + 1 (unser SYN bestaetigt) */
            p->snd_wnd = ntohs(th->window);    /* initiales Peer-Empfangsfenster (SYN-ACK) */
            p->max_snd_wnd = p->snd_wnd;       /* Sender-SWS: groesstes gesehenes Fenster */
            p->snd_wl1 = seg_seq; p->snd_wl2 = seg_ack;   /* Fenster-Update-Basis (RFC 793 3.7) */
            p->rcv_nxt = seg_seq + 1;          /* SYN des Peers belegt 1 */
            p->rcv_adv = p->rcv_nxt;           /* Receiver-SWS: rechte Kante (Handshake-ACK setzt sie) */
            /* SACK/Timestamps gelten nur, wenn WIR sie anboten UND der Peer sie im SYN-ACK bestaetigt. */
            p->sack_ok = (uint8_t)(p->sack_ok && syn_has_sack_perm(payload, hlen));
            uint32_t sv = 0, se = 0;
            if (p->ts_ok && parse_ts(payload, hlen, &sv, &se)) { p->ts_recent = sv; }
            else { p->ts_ok = 0; }
            /* Window Scaling gilt nur, wenn WIR es anboten UND der Peer es im SYN-ACK bestaetigt. */
            uint8_t ws = 0;
            if (p->wscale_ok && parse_wscale(payload, hlen, &ws)) { p->snd_wscale = ws; }
            else { p->wscale_ok = 0; p->snd_wscale = 0; }
            p->state   = ST_ESTABLISHED;
            p->rtx_count = 0;
            p->rtx_ms  = net_now_ms();
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);   /* Handshake-ACK */
            if (p->on_connect) { p->on_connect(p, 1); }
        }
        /* reines SYN (simultaneous open) wird nicht unterstuetzt -> ignorieren */
        return;
    }

    /* --- ACK verarbeiten --- */
    if (flags & TCP_ACK) {
        uint32_t ts_v = 0, ts_e = 0;
        int have_ts = p->ts_ok ? parse_ts(payload, hlen, &ts_v, &ts_e) : 0;
        /* TS.Recent nur aktualisieren, wenn das Segment das naechste erwartete Byte (rcv_nxt =
         * Last.ACK.sent) abdeckt UND der Timestamp nicht aelter ist (RFC 7323 4.3). Ein reines
         * oder ZUKUENFTIGES (out-of-order) Segment aktualisiert TS.Recent NICHT -- sonst wuerde ein
         * spaeter eintreffender Luecken-Fueller mit kleinerem TSval faelschlich von PAWS verworfen. */
        if (have_ts) {
            uint32_t seglen = (uint32_t)datalen
                              + ((flags & TCP_SYN) ? 1u : 0u) + ((flags & TCP_FIN) ? 1u : 0u);
            if (seq_leq(seg_seq, p->rcv_nxt) && seq_lt(p->rcv_nxt, seg_seq + seglen)
                && seq_geq(ts_v, p->ts_recent)) {
                p->ts_recent = ts_v;
            }
        }
        if (seq_gt(seg_ack, p->snd_una) && seq_leq(seg_ack, p->snd_nxt)) {
            uint32_t adv = seg_ack - p->snd_una;
            uint32_t drop = (adv < p->sndlen) ? adv : p->sndlen;
            if (drop) {
                memmove(p->sndbuf,  p->sndbuf  + drop, p->sndlen - drop);
                memmove(p->sndsack, p->sndsack + drop, TCP_SNDBUF - drop);  /* Scoreboard mitschieben */
                memset(p->sndsack + (TCP_SNDBUF - drop), 0, drop);
                p->sndlen = (uint16_t)(p->sndlen - drop);
            }
            p->snd_una = seg_ack;
            p->rtx_count = 0;
            p->rtx_ms = net_now_ms();
            /* RTT aus dem echoten Timestamp messen -> adaptives RTO (nur bei neuem kum. ACK und
             * gueltigem TSecr; TSecr==0 = der Peer hatte noch nichts zu echoen). */
            if (have_ts && ts_e != 0) { tcp_rtt_update(p, (uint32_t)net_now_ms() - ts_e); }
            /* Congestion Control (RFC 5681): das cwnd bei neuem kumulativem ACK vergroessern -- NUR
             * in der Datenphase (nicht beim Handshake-ACK, der nur das SYN bestaetigt; das laeuft
             * hier noch als SYN_RCVD, die ESTABLISHED-Transition folgt weiter unten). */
            if (p->state == ST_ESTABLISHED || p->state == ST_CLOSE_WAIT) {
                if (p->dupacks >= 3) {
                    p->cwnd = p->ssthresh;                     /* Fast Recovery beenden (deflate) */
                } else if (p->cwnd < p->ssthresh) {
                    p->cwnd += (adv < TCP_MSS) ? adv : TCP_MSS; /* Slow Start: ~verdoppeln pro RTT */
                } else {
                    uint32_t inc = (uint32_t)TCP_MSS * TCP_MSS / p->cwnd;   /* Congestion Avoidance */
                    p->cwnd += inc ? inc : 1;                   /* ~+1 SMSS pro RTT */
                }
            }
            p->dupacks = 0;
        } else if ((p->state == ST_ESTABLISHED || p->state == ST_CLOSE_WAIT) &&
                   seg_ack == p->snd_una && p->snd_una != p->snd_nxt &&
                   datalen == 0 && !(flags & (TCP_SYN | TCP_FIN)) &&
                   p->snd_wnd != 0 &&
                   ((uint32_t)ntohs(th->window) << p->snd_wscale) == p->snd_wnd) {
            /* Duplicate ACK (RFC 5681 3.2): der Peer bestaetigt nichts Neues, hat aber ein weiteres
             * Segment gesehen -> ein fruehes Segment fehlt. Ab dem 3. Dup-ACK das aelteste
             * unbestaetigte Segment SOFORT erneut senden (Fast Retransmit, ohne aufs RTO zu warten)
             * und in Fast Recovery gehen -- statt einen ganzen RTO (500ms) zu verlieren.
             * ZUSATZBEDINGUNGEN (RFC 5681 2 (e)): (1) snd_wnd != 0 -- eine Zero-Window-Persist-Probe
             * wird vom Peer mit ack==snd_una beantwortet; das ist FLUSSKONTROLLE, kein Verlustsignal
             * -> nicht als Dup-ACK zaehlen (sonst spurious Fast Retransmit waehrend Persist). (2) das
             * angekuendigte Fenster ist UNVERAENDERT -- ein reines Fenster-Update (gleiches ack, neues
             * Fenster) ist ebenfalls kein Dup-ACK. update_snd_wnd laeuft SPAETER, daher haelt
             * p->snd_wnd hier noch das zuletzt aufgezeichnete Fenster. */
            if (++p->dupacks == 3) {
                uint32_t inflight = p->snd_nxt - p->snd_una;
                uint32_t half = inflight / 2;
                p->ssthresh = (half > 2u * TCP_MSS) ? half : 2u * TCP_MSS;
                uint32_t lim = (inflight < p->sndlen) ? inflight : p->sndlen;
                uint16_t run = 0;
                while (run < (uint16_t)lim && !p->sndsack[run]) { run++; }  /* SACKte Bytes ueberspringen */
                if (run == 0) { run = (uint16_t)lim; }
                uint16_t emss  = tcp_eff_mss(p);
                uint16_t chunk = (run > emss) ? emss : run;
                if (chunk > 0) {
                    seg_out(p, TCP_ACK | TCP_PSH, p->snd_una, 0, p->sndbuf, chunk);
                    p->rtx_ms = net_now_ms();              /* RTO-Timer neu starten (retransmittiert) */
                }
                p->cwnd = p->ssthresh + 3u * TCP_MSS;      /* Fast Recovery: fuer die 3 Dup-ACKs aufblaehen */
            } else if (p->dupacks > 3) {
                p->cwnd += TCP_MSS;                        /* jedes weitere Dup-ACK -> 1 SMSS mehr */
            }
        }
        /* SACK-Sender: vom Peer angekuendigte Bloecke im Scoreboard markieren, damit der
         * Retransmit sie ueberspringt (nur die Luecken erneut senden, RFC 2018). */
        if (p->sack_ok) {
            uint32_t sb[2 * 4];
            int sn = parse_sack(payload, hlen, sb, 4);
            uint32_t hi = p->snd_una + p->sndlen;
            for (int b = 0; b < sn; b++) {
                uint32_t le = sb[b * 2], re = sb[b * 2 + 1];
                /* BEIDE Kanten in das Fenster [snd_una, hi] clippen. Nur eine Seite zu clippen
                 * reicht NICHT: ein boesartiger Block (le weit vor, re per Wrap hinter snd_una)
                 * liesse sonst seq_lt(le,re) mit ~2^31 Abstand wahr -> ~2 Mrd. Schleifen-
                 * durchlaeufe = Hang des single-threaded Netz-Pfads (Remote-DoS). Nach den 4
                 * Clamps sind le,re in [snd_una, hi] -> die Offsets liegen in [0, sndlen]. */
                if (seq_lt(le, p->snd_una)) { le = p->snd_una; }
                if (seq_gt(le, hi))         { le = hi; }
                if (seq_lt(re, p->snd_una)) { re = p->snd_una; }
                if (seq_gt(re, hi))         { re = hi; }
                uint32_t o_lo = le - p->snd_una, o_hi = re - p->snd_una;
                for (uint32_t off = o_lo; off < o_hi; off++) {
                    p->sndsack[off] = 1;                           /* off < sndlen <= TCP_SNDBUF */
                }
            }
        }
        if (p->state == ST_SYN_RCVD && seq_geq(p->snd_una, p->iss + 1)) {
            p->state = ST_ESTABLISHED;
        }
        if (p->state == ST_LAST_ACK && seq_geq(p->snd_una, p->fin_seq + 1)) {
            p->used = 0;
            return;
        }
        if (p->state == ST_FIN_WAIT_1 && seq_geq(p->snd_una, p->fin_seq + 1)) {
            p->state = ST_FIN_WAIT_2;          /* unser FIN bestaetigt */
        }
        /* Peer-Empfangsfenster aktualisieren (nur aus einem neueren Segment) + ggf. neu
         * freigegebene gepufferte Daten senden (Sliding-Window: ACKs bestaetigen Daten UND
         * oeffnen das Fenster weiter). */
        if (p->state == ST_ESTABLISHED || p->state == ST_CLOSE_WAIT) {
            /* Fenster nach dem Handshake mit dem vom Peer angekuendigten Shift skalieren
             * (RFC 7323): ntohs(window) << snd_wscale. NICHT von einem SYN-tragenden Segment:
             * das Fenster eines (retransmittierten) SYN-ACK ist laut RFC 7323 2.3 UNSKALIERT und
             * traegt keine neue Info -- es hier zu skalieren blaehte snd_wnd um 2^snd_wscale auf
             * (Out-of-Window-Burst, bis das naechste echte Segment es korrigiert). */
            if (!(flags & TCP_SYN)) {
                update_snd_wnd(p, seg_seq, seg_ack, (uint32_t)ntohs(th->window) << p->snd_wscale);
            }
            tcp_output(p);
        }
    }

    /* Retransmittiertes SYN-ACK im ESTABLISHED (unser Handshake-ACK ging verloren):
     * erneut mit einem reinen ACK beantworten -- unabhaengig von ausstehenden Daten,
     * damit ein erst verzoegert schreibender Client nicht haengt. */
    if ((flags & TCP_SYN) && (flags & TCP_ACK) && p->state == ST_ESTABLISHED) {
        seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);
    }

    /* --- Daten + FIN (nur ESTABLISHED nimmt neue Daten an) --- */
    if (p->state == ST_ESTABLISHED) {
        if (datalen > 0) {
            /* In-order UND out-of-order Segmente ueber den Reassembly-Puffer: in-order Praefix
             * wird sofort zugestellt (rcv_nxt vor, Handler echot), Luecken-fueller gepuffert.
             * ACK-Politik: in-order -> Delayed-ACK (der Echo-Handler traegt das ACK meist
             * huckepack; nur wenn er nichts sendet, flusht tcp_tick nach 200ms); out-of-order
             * (Luecke) -> SOFORT reines (Dup-)ACK mit SACK-Bloecken (schnelles Retransmit). */
            uint32_t snd_before = p->snd_nxt;
            int delivered = reasm_insert(p, seg_seq, dataptr, datalen);
            if (delivered > 0) {
                if (p->snd_nxt == snd_before) { tcp_ack_schedule(p); }  /* kein Echo -> ACK verzoegern */
            } else {
                seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);               /* OOO -> sofort Dup-ACK */
            }
        }
        /* Der recv-Handler oben darf tcp_close() aufrufen (-> FIN vorgemerkt / FIN_WAIT_1).
         * Dann darf der folgende passive-Close-FIN-Block NICHT mehr laufen, sonst wuerde
         * bei einem koaleszenten data+FIN-Segment ein ZWEITES FIN gesendet und fin_seq/
         * State korrumpiert. fin_pending faengt auch den fenster-gebremsten Fall ab, in dem
         * der aktive Close noch in ESTABLISHED haengt. Ein solches Peer-FIN wird spaeter
         * ueber den FIN_WAIT-Zweig sauber abgeschlossen. */
        if (!p->used || p->state != ST_ESTABLISHED || p->fin_pending) {
            return;
        }
        /* FIN akzeptieren, sobald rcv_nxt seine Sequenz erreicht hat. Strikte Gleichheit
         * reicht NICHT: ein boesartiger Peer kann out-of-order Bytes BEI/HINTER der FIN-Seq
         * puffern, sodass das Reassembly rcv_nxt ueber fin_pos hinausschiebt -- dann bliebe
         * der FIN bei '==' ewig unkonsumiert (State-Desync bis zum Idle-Reaper). Die
         * unsigned-Differenz (rcv_nxt - fin_pos) <= TCP_RCVBUF deckt erreicht + (fenster-
         * begrenztes) Ueberschreiten ab und verwirft zugleich Luecken-/Zukunfts-/Wrap-FINs. */
        uint32_t fin_pos = seg_seq + datalen;
        if ((flags & TCP_FIN) && (uint32_t)(p->rcv_nxt - fin_pos) <= TCP_RCVBUF) {
            if (fin_pos == p->rcv_nxt) {
                p->rcv_nxt += 1;                            /* FIN belegt die naechste Sequenznummer */
            }                                              /* sonst: kumulatives ACK deckt sie schon ab */
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);
            p->state = ST_CLOSE_WAIT;
            /* Passiver Close: eigenes FIN erst HINTER alle (ggf. fenster-gebremsten) Echo-Daten.
             * tcp_try_fin sendet es sofort, falls der Sendepuffer schon leer ist (-> LAST_ACK),
             * sonst sobald tcp_output ihn geleert hat. */
            p->fin_pending = 1;
            tcp_try_fin(p);
            if (p->fin_pending) {
                p->rtx_ms = net_now_ms();      /* RTO/Persist laeuft -> Puffer leert sich, dann FIN */
            }
        }
    } else if (p->state == ST_FIN_WAIT_1 || p->state == ST_FIN_WAIT_2) {
        /* Aktiver Close (Half-Close): wir haben FIN gesendet, koennen aber noch
         * Daten empfangen, bis auch der Peer FIN schickt. Reassembly wie in ESTABLISHED
         * (der Handler darf hier nicht mehr senden -- tcp_write liefert -1 -- aber lesen). */
        if (datalen > 0) {
            reasm_insert(p, seg_seq, dataptr, datalen);
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);
        }
        /* Wie im ESTABLISHED-Zweig: FIN bei erreichter ODER (fenster-begrenzt) ueberschrittener
         * Sequenz akzeptieren -- sonst bliebe FIN_WAIT_2 bei boesartigem Overshoot haengen. */
        uint32_t fin_pos = seg_seq + datalen;
        if ((flags & TCP_FIN) && (uint32_t)(p->rcv_nxt - fin_pos) <= TCP_RCVBUF) {
            if (fin_pos == p->rcv_nxt) {
                p->rcv_nxt += 1;                            /* FIN des Peers belegt die naechste Sequenznummer */
            }
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);
            /* Beide Seiten haben FIN getauscht -> TIME_WAIT (verkuerztes 2MSL). */
            p->state  = ST_TIME_WAIT;
            p->rtx_ms = net_now_ms();
        }
    } else if (p->state == ST_TIME_WAIT) {
        /* Retransmittiertes Peer-FIN re-ACKen + 2MSL-Timer auffrischen. */
        if (flags & TCP_FIN) {
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);
            p->rtx_ms = net_now_ms();
        }
    } else if (p->state == ST_CLOSE_WAIT || p->state == ST_LAST_ACK) {
        if (datalen > 0 || (flags & TCP_FIN)) {
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);       /* Retransmit re-ACKen */
        }
    }
}

void tcp_tick(netif_t *nif)
{
    (void)nif;
    net_enter();   /* Timer-getrieben: mutiert PCB-Zustand -> unter dem Big-Net-Lock */
    uint64_t now = net_now_ms();

    for (int i = 0; i < TCP_MAX_PCB; i++) {
        struct tcp_pcb *p = &s_pcbs[i];
        if (!p->used) {
            continue;
        }

        /* Blunt-Reaper: halb-geschlossene Zustaende (CLOSE_WAIT/FIN_WAIT_2) und eine ESTABLISHED-
         * Verbindung mit AUSSTEHENDEN Daten (Persist/Retransmit gegen einen toten Peer) nach
         * Inaktivitaet ernten. Eine UNTAETIGE (sndlen==0) ESTABLISHED-Verbindung uebernimmt
         * stattdessen Keepalive (s.u.) -- ein lebender, stiller Peer bleibt so erhalten. */
        if ((p->state == ST_CLOSE_WAIT || p->state == ST_FIN_WAIT_2 ||
             (p->state == ST_ESTABLISHED && p->sndlen > 0)) &&
            now - p->last_ms > TCP_IDLE_MS) {
            p->used = 0;
            continue;
        }
        /* Keepalive (RFC 1122 4.2.3.6): eine untaetige ESTABLISHED-Verbindung nach TCP_KEEPIDLE mit
         * Proben sondieren -- ein ACK bei snd_una-1 (Byte, das der Peer schon bestaetigt hat, ohne
         * Daten). Ein lebender Peer antwortet mit einem Dup-ACK -> tcp_input setzt last_ms/ka_probes
         * zurueck, die Verbindung bleibt. Nach TCP_KEEPCNT unbeantworteten Proben gilt der Peer als
         * tot und der PCB wird freigegeben. */
        if (p->state == ST_ESTABLISHED && p->sndlen == 0 &&
            now - p->last_ms > TCP_KEEPIDLE_MS &&
            now - p->ka_last_ms >= TCP_KEEPINTVL_MS) {
            if (p->ka_probes >= TCP_KEEPCNT) {
                p->used = 0;
                continue;
            }
            seg_out(p, TCP_ACK, p->snd_una - 1, 0, 0, 0);   /* Keepalive-Probe (seq = snd_una-1) */
            p->ka_probes++;
            p->ka_last_ms = now;
        }
        /* TIME_WAIT nach verkuerztem 2MSL freigeben. */
        if (p->state == ST_TIME_WAIT && now - p->rtx_ms > TCP_TIME_WAIT_MS) {
            p->used = 0;
            continue;
        }

        uint32_t rto = p->rto ? p->rto : TCP_RTO_MS;   /* adaptives RTO (Jacobson), Fallback 500ms */
        int rtx_due = (now - p->rtx_ms >= rto);
        /* Delayed-ACK flushen, sobald faellig -- ABER nur, wenn kein Retransmit ansteht, der das
         * ACK ohnehin huckepack traegt (sonst redundantes reines ACK vor dem Retransmit). */
        if (p->ack_pending && now >= p->ack_due_ms && !rtx_due) {
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);   /* setzt ack_pending=0 */
        }
        if (!rtx_due) {
            continue;
        }

        if (p->state == ST_SYN_SENT) {
            /* Aktives SYN wiederholen; nach zu vielen Versuchen Connect-Fehlschlag. */
            if (++p->rtx_count > TCP_MAX_RTX) {
                if (p->on_connect) { p->on_connect(p, 0); }
                p->used = 0;
                continue;
            }
            seg_out(p, TCP_SYN, p->iss, 1, 0, 0);
            tcp_backoff(p);
            p->rtx_ms = now;
        } else if (p->state == ST_SYN_RCVD) {
            if (++p->rtx_count > TCP_MAX_RTX) { p->used = 0; continue; }
            seg_out(p, TCP_SYN | TCP_ACK, p->iss, 1, 0, 0);
            tcp_backoff(p);
            p->rtx_ms = now;
        } else if ((p->state == ST_ESTABLISHED || p->state == ST_CLOSE_WAIT ||
                    p->state == ST_LAST_ACK) && p->sndlen > 0) {
            uint32_t inflight = p->snd_nxt - p->snd_una;
            /* Persist (Fenster zu) NICHT gegen TCP_MAX_RTX abbrechen: die Probe laeuft, solange
             * der Peer lebt (er ACKt sie mit Fenster 0, ohne snd_una zu bewegen -> rtx_count
             * wuerde sonst hochzaehlen und die Verbindung faelschlich reseten). Ein toter Peer
             * wird stattdessen vom Idle-Reaper (TCP_IDLE_MS) geerntet. LAST_ACK ist ausgenommen
             * (vom Reaper nicht erfasst) -> dort muss das FIN irgendwann aufgeben. */
            int persisting = (p->snd_wnd == 0 && p->state != ST_LAST_ACK);
            if (!persisting) {
                if (++p->rtx_count > TCP_MAX_RTX) { p->used = 0; continue; }
            }
            if (inflight > 0) {
                /* aelteste im-Flug-Daten erneut senden (NICHT ungesendete jenseits des Fensters).
                 * Laenge nie ueber die gepufferten Daten hinaus: in LAST_ACK zaehlt snd_nxt das
                 * FIN mit (inflight == sndlen+1), sndbuf haelt aber nur sndlen Bytes -> ohne
                 * min(inflight,sndlen) ein 1-Byte-Ueberlesen + Senden der FIN-Sequenz als Daten. */
                uint32_t lim   = (inflight < p->sndlen) ? inflight : p->sndlen;
                /* SACK-Sender: ab snd_una (immer eine Luecke) nur die erste NICHT-SACKte Strecke
                 * erneut senden -- bereits vom Peer SACKte Bytes ueberspringen (RFC 2018). */
                uint16_t run = 0;
                while (run < (uint16_t)lim && !p->sndsack[run]) { run++; }
                if (run == 0) { run = (uint16_t)lim; }        /* Fallback: snd_una-Byte SACKt (untypisch) */
                uint16_t emss  = tcp_eff_mss(p);              /* Platz fuer SACK-Optionen lassen */
                uint16_t chunk = (run > emss) ? emss : run;
                seg_out(p, TCP_ACK | TCP_PSH, p->snd_una, 0, p->sndbuf, chunk);
                if (!persisting) {
                    tcp_backoff(p);                           /* echter Retransmit (kein Persist) -> RTO-Backoff */
                    /* RTO-Verlust (RFC 5681 3.1): ssthresh = max(FlightSize/2, 2 SMSS), cwnd = 1 SMSS
                     * -> zurueck in Slow Start. dupacks zuruecksetzen (kein Fast-Recovery-Zustand mehr). */
                    uint32_t half = inflight / 2;
                    p->ssthresh = (half > 2u * TCP_MSS) ? half : 2u * TCP_MSS;
                    p->cwnd     = TCP_MSS;
                    p->dupacks  = 0;
                }
            } else if (persisting) {
                /* Fenster zu, nichts im Flug: 1-Byte-Persist-Probe (naechstes gepuffertes Byte)
                 * -> provoziert ein Fenster-Update vom Peer; snd_nxt zaehlt mit. */
                seg_out(p, TCP_ACK | TCP_PSH, p->snd_nxt, 0, p->sndbuf, 1);
                p->snd_nxt += 1;
            } else {
                /* Fenster offen, nichts im Flug: ungesendete Daten nachholen. */
                tcp_output(p);
            }
            p->rtx_ms = now;
        } else if (p->state == ST_LAST_ACK) {       /* sndlen == 0 -> nur FIN */
            if (++p->rtx_count > TCP_MAX_RTX) { p->used = 0; continue; }
            seg_out(p, TCP_FIN | TCP_ACK, p->fin_seq, 0, 0, 0);
            tcp_backoff(p);
            p->rtx_ms = now;
        } else if (p->state == ST_FIN_WAIT_1) {
            /* Aktiver Close: unbestaetigte Daten zuerst, sonst unser FIN wiederholen. */
            if (++p->rtx_count > TCP_MAX_RTX) { p->used = 0; continue; }
            if (p->sndlen > 0) {
                uint16_t emss  = tcp_eff_mss(p);              /* Platz fuer SACK-Optionen lassen */
                uint16_t chunk = (p->sndlen > emss) ? emss : p->sndlen;
                seg_out(p, TCP_ACK | TCP_PSH, p->snd_una, 0, p->sndbuf, chunk);
            } else {
                seg_out(p, TCP_FIN | TCP_ACK, p->fin_seq, 0, 0, 0);
            }
            tcp_backoff(p);
            p->rtx_ms = now;
        }
        /* War ein Retransmit faellig (rtx_due), aber kein Zweig hat gesendet (z.B. ESTABLISHED
         * mit sndlen==0 bei einer reinen Sink-App) UND ein Delayed-ACK ist ueberfaellig -> es
         * jetzt flushen (sonst bliebe es haengen; ein sendender Zweig haette es huckepack getragen). */
        if (p->used && p->ack_pending && now >= p->ack_due_ms) {
            seg_out(p, TCP_ACK, p->snd_nxt, 0, 0, 0);
        }
    }
    net_leave();
}

/* --- White-Box-Selbsttest des Empfangs-Reassembly (in-guest). ---------------------------
 * QEMU/SLIRP ordnet Segmente NICHT um -> der Reassembly-Pfad ist per Host-Interop nicht
 * ausloesbar. Dieser Test treibt reasm_insert direkt mit out-of-order Ankuenften und prueft
 * in-order Zustellung, rcv_nxt-Vorschub, Alt-/Jenseits-Fenster-Verwerfen, Ueberlappung und
 * die rcv_buffered-Buchhaltung. Liefert 1 = bestanden. */
static uint8_t  s_st_buf[64];
static uint16_t s_st_len;
static void st_recorder(struct tcp_pcb *pcb, const uint8_t *d, uint16_t n)
{
    (void)pcb;
    for (uint16_t i = 0; i < n && s_st_len < sizeof(s_st_buf); i++) {
        s_st_buf[s_st_len++] = d[i];
    }
}

int tcp_reasm_selftest(void)
{
    static struct tcp_pcb t;            /* gross (2x TCP_RCVBUF) -> nicht auf den Stack */
    memset(&t, 0, sizeof(t));
    t.handler = st_recorder;
    t.rcv_nxt = 1000;
    s_st_len  = 0;
    int ok = 1;

    /* 1) Out-of-order: "DE" @1003 -> nur Luecke (1000..1002 fehlen), nichts zugestellt. */
    if (reasm_insert(&t, 1003, (const uint8_t *)"DE", 2) != 0) ok = 0;
    if (t.rcv_nxt != 1000 || s_st_len != 0 || t.rcv_buffered != 2) ok = 0;

    /* 2) Luecke fuellen: "ABC" @1000 -> "ABCDE" wird in-order zugestellt, rcv_nxt=1005. */
    if (reasm_insert(&t, 1000, (const uint8_t *)"ABC", 3) != 5) ok = 0;
    if (t.rcv_nxt != 1005 || t.rcv_buffered != 0) ok = 0;

    /* 3) In-order Fortsetzung: "FG" @1005 -> rcv_nxt=1007. */
    if (reasm_insert(&t, 1005, (const uint8_t *)"FG", 2) != 2) ok = 0;
    if (t.rcv_nxt != 1007) ok = 0;

    /* 4) Altes Segment "X" @1004 (< rcv_nxt) -> komplett verworfen, kein Vorschub. */
    if (reasm_insert(&t, 1004, (const uint8_t *)"X", 1) != 0) ok = 0;
    if (t.rcv_nxt != 1007) ok = 0;

    /* 5) Jenseits des Fensters (@rcv_nxt+TCP_RCVBUF) -> verworfen, keine Buchung. */
    if (reasm_insert(&t, 1007 + TCP_RCVBUF, (const uint8_t *)"Z", 1) != 0) ok = 0;
    if (t.rcv_buffered != 0) ok = 0;

    /* 6) Mehrfach-Luecke + UEBERLAPPUNG mit KONFLIKT: "jK"@1009 (Luecke 1007/08; das 'j'
     *    bei Offset 1009 ist KLEINGESCHRIEBEN), dann "HIJ"@1007 -> das ueberlappende 'J'@1009
     *    ist GROSS. Nur 'last-wins' (Store ueberschreibt markierte Bytes) liefert das grosse
     *    'J' -> die finale "...HIJK"-Pruefung faellt durch, falls je auf 'first-wins'
     *    geaendert wuerde. So ist die Overlap-Policy (klassischer Reassembly-Overlap-Vektor)
     *    fest verankert. Ergebnis: "HIJK" in-order, rcv_nxt=1011. */
    if (reasm_insert(&t, 1009, (const uint8_t *)"jK", 2) != 0) ok = 0;
    if (reasm_insert(&t, 1007, (const uint8_t *)"HIJ", 3) != 4) ok = 0;
    if (t.rcv_nxt != 1011 || t.rcv_buffered != 0) ok = 0;

    /* 7) Akzeptanz-Rand: das LETZTE Byte im Fenster (rcv_nxt+TCP_RCVBUF-1, Offset 2047) MUSS
     *    gepuffert werden (eine um 1 zu enge Fenster-Grenze wuerde es faelschlich verwerfen).
     *    run=0 (Luecke davor), aber rcv_buffered=1. Steht am Ende -> kein Cleanup noetig. */
    if (reasm_insert(&t, t.rcv_nxt + TCP_RCVBUF - 1, (const uint8_t *)"E", 1) != 0) ok = 0;
    if (t.rcv_buffered != 1) ok = 0;

    /* Gesamte in-order Zustellung muss exakt "ABCDEFGHIJK" sein (grosses 'J' = last-wins). */
    static const char exp[] = "ABCDEFGHIJK";
    if (s_st_len != 11) ok = 0;
    for (uint16_t i = 0; i < 11 && ok; i++) {
        if (s_st_buf[i] != (uint8_t)exp[i]) ok = 0;
    }
    return ok;
}
