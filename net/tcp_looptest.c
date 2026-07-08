/*
 * net/tcp_looptest.c  --  End-to-End-Conformance-Test des TCP-Stacks ueber eine
 * in-guest Loopback-"Leitung".
 *
 * Warum: QEMU/SLIRP und der Host-TCP-Stack ordnen Segmente NIE um und kuendigen ein
 * grosses (65535) Fenster an -> Out-of-Order-Reassembly (Empfang) und die Sende-
 * Flusskontrolle gegen ein KLEINES Peer-Fenster sind per Host-Interop NICHT ausloesbar.
 * Dieser Test baut einen boesartigen/kontrollierten "virtuellen Peer", der rohe TCP-
 * Segmente mit frei waehlbarer Reihenfolge und Fenstergroesse erzeugt und ueber den
 * ECHTEN Pfad (eth_input -> ip_input -> tcp_input ... tcp_output -> netif->transmit)
 * mit dem Stack spricht. Eine eigene Loopback-netif faengt die Antworten des Stacks ab.
 *
 * Geprueft:
 *   1) Out-of-Order-Empfang  -> in-order Zustellung an die App + schrumpfendes Fenster.
 *   2) Sendeseitige Flusskontrolle gegen ein 4-Byte-Peer-Fenster (Sliding-Window).
 *
 * Laeuft EINMAL beim Boot des virt-Harness, bevor DHCP/echter Verkehr startet (die PCBs
 * werden am Ende jeder Szene per RST sofort freigegeben).
 */
#include <stdint.h>
#include "net.h"
#include "tcp.h"
#include "kmem.h"
#include "uart.h"

/* TCP-Header (Spiegel von tcp.c, ohne Optionen). */
typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} ltcp_hdr_t;

#define LF_FIN 0x01
#define LF_SYN 0x02
#define LF_RST 0x04
#define LF_PSH 0x08
#define LF_ACK 0x10

#define LO_GUEST_IP  IP4(10, 0, 9, 1)
#define LO_PEER_IP   IP4(10, 0, 9, 2)
#define LO_PEER_PORT 40000
#define LO_TEST_PORT 7777

static netif_t s_lo;
static const uint8_t s_peer_mac[ETH_ALEN] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x99 };

/* Capture-Ring fuer die vom Stack ueber s_lo gesendeten Frames. */
#define CAP_MAX 24
static struct { uint16_t len; uint8_t buf[NET_FRAME_MAX]; } s_cap[CAP_MAX];
static int s_cap_head, s_cap_tail;
static uint8_t s_parsebuf[NET_FRAME_MAX];

/* App-Handler-Aufzeichnung: die IN REIHENFOLGE zugestellten Empfangsbytes. */
static uint8_t  s_recv[128];
static uint16_t s_recv_len;

struct seg {
    uint32_t seq, ack;
    uint16_t window, sport, dport, dlen;
    uint8_t  flags;
    uint8_t  sack_perm;            /* SACK-Permitted-Option im Segment gesehen */
    uint8_t  sack_n;               /* Anzahl geparster SACK-Bloecke */
    uint32_t sack[2 * 3];          /* bis zu 3 Bloecke: [left,right) Paare */
    uint8_t  has_ts;               /* Timestamps-Option vorhanden */
    uint32_t ts_val, ts_ecr;       /* TSval / TSecr des Guests */
    uint8_t  has_wscale;           /* Window-Scale-Option vorhanden (SYN/SYN-ACK) */
    uint8_t  wscale;               /* angekuendigter Shift des Guests */
    const uint8_t *data;
};

static int lo_transmit(netif_t *nif, const uint8_t *frame, uint16_t len)
{
    (void)nif;
    int nxt = (s_cap_head + 1) % CAP_MAX;
    if (nxt != s_cap_tail && len <= NET_FRAME_MAX) {
        memcpy(s_cap[s_cap_head].buf, frame, len);
        s_cap[s_cap_head].len = len;
        s_cap_head = nxt;
    }
    return len;
}

/* TCP-Pruefsumme wie in tcp.c (Pseudo-Header host-order src/dst + Segment). */
static uint16_t lo_tcp_csum(ip4_addr_t src, ip4_addr_t dst, const uint8_t *seg, uint16_t len)
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

/* Ein Segment vom Peer an den Stack injizieren (volles Ethernet-Frame -> eth_input), mit frei
 * waehlbaren TCP-Ports (fuer den Active-Open-Test, bei dem der Guest der Opener ist und die
 * Ports umgekehrt liegen). optlen MUSS ein Vielfaches von 4 sein. */
static void peer_xmit(uint16_t sport, uint16_t dport, uint8_t flags, uint32_t seq, uint32_t ack,
                      uint16_t window, const uint8_t *data, uint16_t dlen,
                      const uint8_t *opt, uint8_t optlen)
{
    static uint8_t frame[NET_FRAME_MAX];
    eth_hdr_t *eh = (eth_hdr_t *)frame;
    memcpy(eh->dst, s_lo.mac.b, ETH_ALEN);
    memcpy(eh->src, s_peer_mac, ETH_ALEN);
    eh->ethertype = htons(ETHERTYPE_IPV4);

    uint16_t thdr   = (uint16_t)(sizeof(ltcp_hdr_t) + optlen);   /* TCP-Header inkl. Optionen */
    uint16_t tcplen = (uint16_t)(thdr + dlen);
    ip_hdr_t *ih = (ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    ih->ver_ihl = 0x45; ih->dscp_ecn = 0;
    ih->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + tcplen));
    ih->id = 0; ih->flags_frag = htons(0x4000); ih->ttl = 64; ih->proto = IPPROTO_TCP;
    ih->checksum = 0; ih->src = htonl(LO_PEER_IP); ih->dst = htonl(LO_GUEST_IP);
    ih->checksum = htons(inet_checksum(ih, sizeof(ip_hdr_t)));

    ltcp_hdr_t *th = (ltcp_hdr_t *)((uint8_t *)ih + sizeof(ip_hdr_t));
    th->src_port = htons(sport); th->dst_port = htons(dport);
    th->seq = htonl(seq); th->ack = htonl(ack);
    th->data_off = (uint8_t)((thdr / 4) << 4);
    th->flags = flags; th->window = htons(window); th->checksum = 0; th->urgent = 0;
    if (optlen && opt) {
        memcpy((uint8_t *)th + sizeof(ltcp_hdr_t), opt, optlen);
    }
    if (dlen && data) {
        memcpy((uint8_t *)th + thdr, data, dlen);
    }
    th->checksum = htons(lo_tcp_csum(LO_PEER_IP, LO_GUEST_IP, (uint8_t *)th, tcplen));

    eth_input(&s_lo, frame, (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + tcplen));
}

/* Passive-Open-Default: Peer (LO_PEER_PORT) -> Guest-Listener (LO_TEST_PORT). */
static void peer_send_opt(uint8_t flags, uint32_t seq, uint32_t ack, uint16_t window,
                          const uint8_t *data, uint16_t dlen,
                          const uint8_t *opt, uint8_t optlen)
{
    peer_xmit(LO_PEER_PORT, LO_TEST_PORT, flags, seq, ack, window, data, dlen, opt, optlen);
}

static void peer_send(uint8_t flags, uint32_t seq, uint32_t ack, uint16_t window,
                      const uint8_t *data, uint16_t dlen)
{
    peer_send_opt(flags, seq, ack, window, data, dlen, 0, 0);
}

/* Timestamps-Option {NOP NOP TS(10)} in o schreiben (12 Byte, 4-Byte-ausgerichtet). */
static void build_peer_ts(uint8_t *o, uint32_t tsval, uint32_t tsecr)
{
    o[0] = 1; o[1] = 1; o[2] = 8; o[3] = 10;
    o[4] = (uint8_t)(tsval >> 24); o[5] = (uint8_t)(tsval >> 16);
    o[6] = (uint8_t)(tsval >> 8);  o[7] = (uint8_t)tsval;
    o[8] = (uint8_t)(tsecr >> 24); o[9] = (uint8_t)(tsecr >> 16);
    o[10] = (uint8_t)(tsecr >> 8); o[11] = (uint8_t)tsecr;
}

/* Naechstes vom Stack gesendetes TCP-Segment holen + parsen. 1 = ok, 0 = Ring leer. */
static int cap_next(struct seg *out)
{
    while (s_cap_tail != s_cap_head) {
        uint16_t len = s_cap[s_cap_tail].len;
        memcpy(s_parsebuf, s_cap[s_cap_tail].buf, len);
        s_cap_tail = (s_cap_tail + 1) % CAP_MAX;
        if (len < sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(ltcp_hdr_t)) {
            continue;
        }
        eth_hdr_t *eh = (eth_hdr_t *)s_parsebuf;
        if (ntohs(eh->ethertype) != ETHERTYPE_IPV4) {
            continue;
        }
        ip_hdr_t *ih = (ip_hdr_t *)(s_parsebuf + sizeof(eth_hdr_t));
        if (ih->proto != IPPROTO_TCP) {
            continue;
        }
        uint8_t ihl = (uint8_t)((ih->ver_ihl & 0x0F) * 4);
        ltcp_hdr_t *th = (ltcp_hdr_t *)(s_parsebuf + sizeof(eth_hdr_t) + ihl);
        uint8_t thl = (uint8_t)((th->data_off >> 4) * 4);
        uint16_t iptot = ntohs(ih->total_len);
        out->sport  = ntohs(th->src_port); out->dport = ntohs(th->dst_port);
        out->seq    = ntohl(th->seq);      out->ack   = ntohl(th->ack);
        out->flags  = th->flags;           out->window = ntohs(th->window);
        out->dlen   = (uint16_t)(iptot - ihl - thl);
        out->data   = s_parsebuf + sizeof(eth_hdr_t) + ihl + thl;

        /* TCP-Optionen parsen: SACK-Permitted (kind 4) + SACK-Bloecke (kind 5) + Timestamps (kind 8). */
        out->sack_perm = 0; out->sack_n = 0; out->has_ts = 0; out->ts_val = 0; out->ts_ecr = 0;
        out->has_wscale = 0; out->wscale = 0;
        const uint8_t *o = (const uint8_t *)th + sizeof(ltcp_hdr_t);
        int ol = (int)thl - (int)sizeof(ltcp_hdr_t), j = 0;
        while (j < ol) {
            uint8_t k = o[j];
            if (k == 0) { break; }
            if (k == 1) { j++; continue; }
            if (j + 2 > ol) { break; }
            uint8_t l = o[j + 1];
            if (l < 2 || j + l > ol) { break; }
            if (k == 4 && l == 2) {
                out->sack_perm = 1;
            } else if (k == 3 && l == 3) {
                out->has_wscale = 1;
                out->wscale = o[j + 2];
            } else if (k == 8 && l == 10) {
                const uint8_t *q = o + j + 2;
                out->has_ts = 1;
                out->ts_val = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
                out->ts_ecr = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
            } else if (k == 5) {
                int nb = (l - 2) / 8;
                for (int b = 0; b < nb && out->sack_n < 3; b++) {
                    const uint8_t *q = o + j + 2 + b * 8;
                    out->sack[out->sack_n * 2] =
                        ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
                    out->sack[out->sack_n * 2 + 1] =
                        ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
                    out->sack_n++;
                }
            }
            j += l;
        }
        return 1;
    }
    return 0;
}

static void cap_drain(void) { struct seg s; while (cap_next(&s)) { } }

/* Letzter vom Listener-Handler gesehener PCB -- fuer white-box Zugriffe (Congestion Control). */
static tcp_pcb_t *s_lo_pcb;

/* App-Handler: zeichnet die in-order zugestellten Bytes auf UND echot sie zurueck. */
static void lo_handler(tcp_pcb_t *pcb, const uint8_t *data, uint16_t len)
{
    s_lo_pcb = pcb;
    for (uint16_t i = 0; i < len && s_recv_len < sizeof(s_recv); i++) {
        s_recv[s_recv_len++] = data[i];
    }
    tcp_write(pcb, data, len);   /* Echo (Sendepfad) */
}

/* Active-Open-Test: der Guest verbindet sich (tcp_connect). Bei ESTABLISHED schreibt der Client
 * sofort 1500 Bytes -> das (vom Peer angekuendigte) Sendefenster drosselt den Ausgang. Die Groesse
 * MUSS >= eff_mss sein: sonst wuerde Nagle den Rest halten und ein faelschlich aufgeblaehtes Fenster
 * (Bug) wuerde KEINEN beobachtbaren Burst erzeugen (der Guardian waere blind). */
static int s_ao_conn;                 /* Ergebnis von on_connect (-1 = noch nicht gerufen) */
static uint8_t s_ao_payload[1500];
static void ao_conn(tcp_pcb_t *pcb, int ok)
{
    s_ao_conn = ok;
    if (ok) { tcp_write(pcb, s_ao_payload, sizeof(s_ao_payload)); }
}
static void ao_recv(tcp_pcb_t *pcb, const uint8_t *data, uint16_t len)
{
    (void)pcb; (void)data; (void)len;
}

/* Wie expect_pcb_freed, aber mit frei waehlbaren Ports (Active-Open: umgekehrte Ports). */
static int expect_pcb_freed_ports(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack)
{
    struct seg s;
    cap_drain();
    peer_xmit(sport, dport, LF_ACK, seq, ack, 0, 0, 0, 0, 0);
    if (!cap_next(&s)) { return 0; }
    return (s.flags & LF_RST) ? 1 : 0;
}

/* Drei-Wege-Handshake: Peer als aktiver Opener. Liefert die Guest-ISN (G) in *g_isn,
 * sodass danach gilt: Guest rcv_nxt = peer_isn+1, Guest snd_una = G+1. 1 = ok. */
static int do_handshake(uint32_t peer_isn, uint16_t peer_win, uint32_t *g_isn, uint16_t *synack_win)
{
    struct seg s;
    cap_drain();
    peer_send(LF_SYN, peer_isn, 0, peer_win, 0, 0);
    if (!cap_next(&s)) { return 0; }
    if (!((s.flags & LF_SYN) && (s.flags & LF_ACK) && s.ack == peer_isn + 1)) { return 0; }
    *g_isn = s.seq;
    if (synack_win) { *synack_win = s.window; }
    peer_send(LF_ACK, peer_isn + 1, s.seq + 1, peer_win, 0, 0);
    cap_drain();                 /* evtl. reiner ACK ohne Daten */
    return 1;
}

/* Prueft, dass der PCB des Test-4-Tupels WIRKLICH freigegeben ist: ein Streu-ACK auf eine
 * nicht (mehr) existente Verbindung muss vom Stack mit einem RST beantwortet werden
 * (find_pcb-Fehlschlag -> send_rst). Existierte der PCB noch, wuerde das ACK still
 * geschluckt. So faengt der Test ein Ignorieren des Teardown-RST / einen PCB-Leak ab. */
static int expect_pcb_freed(uint32_t seq, uint32_t ack)
{
    struct seg s;
    cap_drain();
    peer_send(LF_ACK, seq, ack, 0, 0, 0);
    if (!cap_next(&s)) { return 0; }
    return (s.flags & LF_RST) ? 1 : 0;
}

/* Szene 1: Out-of-Order-Empfang -> in-order Zustellung + schrumpfendes Fenster. */
static int scene_out_of_order(void)
{
    struct seg s;
    uint32_t P = 1000, G = 0;
    uint16_t w0 = 0;
    s_recv_len = 0;
    if (!do_handshake(P, 8192, &G, &w0)) { return 0; }
    if (w0 == 0) { return 0; }

    /* "WORLD" @ P+6 zuerst (Luecke [P+1, P+6)). */
    peer_send(LF_ACK | LF_PSH, P + 6, G + 1, 8192, (const uint8_t *)"WORLD", 5);
    /* Erwartung: Dup-ACK (ack==P+1, noch nichts in-order) + Fenster um 5 geschrumpft. */
    if (!cap_next(&s)) { return 0; }
    if (s.ack != P + 1) { return 0; }
    if (s.window != (uint16_t)(w0 - 5)) { return 0; }   /* rcv_buffered=5 -> Fenster -5 */
    if (s_recv_len != 0) { return 0; }                  /* App hat noch NICHTS bekommen */
    cap_drain();

    /* "HELLO" @ P+1 fuellt die Luecke -> "HELLOWORLD" wird IN REIHENFOLGE zugestellt. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"HELLO", 5);
    if (s_recv_len != 10) { return 0; }
    if (memcmp(s_recv, "HELLOWORLD", 10) != 0) { return 0; }
    /* Guest bestaetigt jetzt kumulativ bis P+11 (das erste Segment -- Echo oder ACK -- traegt
     * ack==rcv_nxt==P+11). Das Fenster ist hier NICHT mehr w0, da der Echo-Schreibvorgang den
     * Sendepuffer fuellt (Empfangsfenster = min(rcv-frei, snd-frei)). */
    if (!cap_next(&s)) { return 0; }
    if (s.ack != P + 11) { return 0; }
    cap_drain();

    peer_send(LF_RST, P + 11, 0, 0, 0, 0);              /* Teardown: seq==rcv_nxt -> PCB frei */
    if (!expect_pcb_freed(P + 11, G + 1)) { return 0; } /* RST MUSS den PCB wirklich freigeben */
    return 1;
}

/* Szene 2: Sendeseitige Flusskontrolle gegen ein 4-Byte-Peer-Fenster (Sliding-Window). */
static int scene_send_window(void)
{
    struct seg s;
    uint32_t P = 5000, G = 0;
    uint8_t  echo_buf[16];
    uint16_t echo_n = 0;
    s_recv_len = 0;
    if (!do_handshake(P, 4, &G, 0)) { return 0; }       /* Peer kuendigt Fenster=4 an */

    /* Peer schickt 10 Bytes -> Guest echot 10 (in-order an die App), aber unser 4-Byte-Fenster
     * drosselt den Sendepfad. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 4, (const uint8_t *)"ABCDEFGHIJ", 10);
    if (s_recv_len != 10 || memcmp(s_recv, "ABCDEFGHIJ", 10) != 0) { return 0; }

    /* Erste Echo-Sendung einsammeln: JEDES Segment <=4, das erste GENAU 4 (Fenster voll genutzt). */
    int first = -1;
    while (cap_next(&s)) {
        if (s.dlen == 0) { continue; }
        if (s.dlen > 4) { return 0; }                   /* NIE mehr als das Peer-Fenster senden */
        if (first < 0) { first = (int)s.dlen; }
        for (uint16_t i = 0; i < s.dlen && echo_n < sizeof(echo_buf); i++) {
            echo_buf[echo_n++] = s.data[i];
        }
    }
    if (first != 4) { return 0; }                       /* genau 4 -> volle, nicht zu knappe Nutzung */

    /* IN-FLIGHT-Schranke: ein reines Fenster-Update, das die 4 in-Flight-Bytes NICHT bestaetigt
     * (ack unveraendert = G+1), darf KEIN weiteres Segment ausloesen -- inflight(4) fuellt das
     * Fenster(4) schon. Faengt eine 'usable = snd_wnd'-Regression (ohne inflight-Abzug) ab, die
     * der voll-bestaetigende Drain-Loop sonst maskiert (dort ist inflight stets 0). */
    peer_send(LF_ACK, P + 11, G + 1, 4, 0, 0);
    while (cap_next(&s)) {
        if (s.dlen > 0) { return 0; }                   /* es darf NICHTS Neues gesendet werden */
    }

    /* Fenster schrittweise oeffnen (je 4 bestaetigen) -> Sliding-Window zieht den Rest nach,
     * jedes Segment weiterhin <=4. */
    for (int round = 0; round < 6 && echo_n < 10; round++) {
        peer_send(LF_ACK, P + 11, G + 1 + echo_n, 4, 0, 0);
        while (cap_next(&s)) {
            if (s.dlen == 0) { continue; }
            if (s.dlen > 4) { return 0; }
            for (uint16_t i = 0; i < s.dlen && echo_n < sizeof(echo_buf); i++) {
                echo_buf[echo_n++] = s.data[i];
            }
        }
    }
    if (echo_n != 10) { return 0; }                     /* am Ende alle 10 Bytes echot ... */
    if (memcmp(echo_buf, "ABCDEFGHIJ", 10) != 0) { return 0; }   /* ... mit korrektem Inhalt auf der Leitung */

    peer_send(LF_RST, P + 11, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 11, G + 1 + echo_n)) { return 0; }
    return 1;
}

/* Szene 3: Out-of-Order-Daten BEI/HINTER der FIN-Sequenz -> das Reassembly schiebt rcv_nxt
 * ueber die FIN-Position; der Stack MUSS das FIN dennoch akzeptieren (passiver Close). Pinnt
 * den FIN-Overshoot-Fix ((rcv_nxt-fin_pos) <= TCP_RCVBUF) -- eine strikte '=='-Pruefung liesse
 * die Verbindung haengen (per Host-Interop NICHT ausloesbar, da nichts umordnet). */
static int scene_fin_overshoot(void)
{
    struct seg s;
    uint32_t P = 2000, G = 0;
    s_recv_len = 0;
    if (!do_handshake(P, 8192, &G, 0)) { return 0; }

    /* "CD" @ P+3 zuerst -> Luecke [P+1, P+3); rcv_nxt bleibt P+1, nichts zugestellt. */
    peer_send(LF_ACK | LF_PSH, P + 3, G + 1, 8192, (const uint8_t *)"CD", 2);
    if (!cap_next(&s)) { return 0; }
    if (s.ack != P + 1 || s_recv_len != 0) { return 0; }
    cap_drain();

    /* "AB"+FIN @ P+1: FIN-Seq = P+3, aber das Reassembly liefert "ABCD" -> rcv_nxt = P+5
     * (ueberschreitet fin_pos um 2). Erwartung: App bekam "ABCD"; Guest akzeptiert das FIN
     * (CLOSE_WAIT -> eigenes FIN -> LAST_ACK): ein FIN-Segment + ein ACK mit ack==P+5. */
    peer_send(LF_ACK | LF_FIN, P + 1, G + 1, 8192, (const uint8_t *)"AB", 2);
    if (s_recv_len != 4 || memcmp(s_recv, "ABCD", 4) != 0) { return 0; }
    int saw_fin = 0, saw_ack_p5 = 0;
    while (cap_next(&s)) {
        if (s.flags & LF_FIN) { saw_fin = 1; }
        if (s.ack == P + 5)   { saw_ack_p5 = 1; }
    }
    if (!saw_fin || !saw_ack_p5) { return 0; }          /* Overshoot-FIN wurde akzeptiert */

    /* Echo (4 Bytes G+1..G+4) + Guest-FIN (G+5) bestaetigen -> LAST_ACK abgeschlossen, PCB frei. */
    peer_send(LF_ACK, P + 5, G + 6, 8192, 0, 0);
    if (!expect_pcb_freed(P + 5, G + 6)) { return 0; }
    return 1;
}

/* Szene 4: Zero-Window-Persist. Peer kuendigt Fenster=0 an -> der Guest kann seine Echo-Daten
 * NICHT senden und muss nach dem RTO eine 1-Byte-Persist-Probe schicken (sonst verhungert die
 * Verbindung). Die Zeit wird per net_test_advance_ms ueber den RTO gestellt + tcp_tick getrieben
 * (synchroner Test, kein 500ms-Realtime-Warten). Per Host-Interop NICHT moeglich (SLIRP: 65535). */
static int scene_persist(void)
{
    struct seg s;
    uint32_t P = 3000, G = 0;
    s_recv_len = 0;
    if (!do_handshake(P, 0, &G, 0)) { return 0; }       /* Peer-Fenster = 0 */

    /* 2 Bytes -> Guest will echoen, aber Fenster=0 -> nichts gesendet (Persist armiert). */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 0, (const uint8_t *)"HI", 2);
    if (s_recv_len != 2 || memcmp(s_recv, "HI", 2) != 0) { return 0; }
    while (cap_next(&s)) { if (s.dlen > 0) { return 0; } }   /* bei Fenster 0: KEIN Echo-Datensegment */

    /* Uhr ueber den RTO stellen + tcp_tick -> erwartet eine 1-Byte-Persist-Probe ("H" @ G+1). */
    net_test_advance_ms(600);
    tcp_tick(&s_lo);
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 1 || s.seq != G + 1 || s.data[0] != 'H') { return 0; }
    cap_drain();

    /* Persist-Robustheit: ein LEBENDER 0-Fenster-Peer (ACKt jede Probe mit Fenster 0, OHNE
     * snd_una zu bewegen) darf NIE wegen TCP_MAX_RTX(8) reseten -- Persist zaehlt nicht gegen
     * das Retransmit-Limit. >8 Zyklen treiben und pruefen, dass weiter sondiert wird (faengt
     * eine Regression, die Persist faelschlich gegen TCP_MAX_RTX zaehlen liesse). */
    for (int k = 0; k < 11; k++) {
        peer_send(LF_ACK, P + 3, G + 1, 0, 0, 0);       /* Probe NICHT bestaetigen, Fenster bleibt 0 */
        /* Eine reine 0-Fenster-Persist-ACK ist Flusskontrolle, KEIN Verlustsignal -> sie darf weder
         * als Dup-ACK zaehlen noch ein Datensegment (spurious Fast Retransmit) ausloesen. */
        while (cap_next(&s)) { if (s.dlen > 0) { return 0; } }
        net_test_advance_ms(600);
        tcp_tick(&s_lo);
        if (!cap_next(&s)) { return 0; }                /* PCB lebt -> Probe wird weiter gesendet */
        if (s.dlen != 1 || s.seq != G + 1 || s.data[0] != 'H') { return 0; }
        cap_drain();
    }

    /* Congestion State DARF von den 0-Fenster-Persist-ACKs NICHT beruehrt worden sein: ssthresh
     * bleibt der Initialwert, cwnd das Initial Window -- pinnt den Dup-ACK-Fix (ohne ihn zaehlte
     * jede Probe-ACK als Dup-ACK -> spurious Fast Retransmit + ssthresh-Kollaps auf 2 SMSS). */
    if (tcp_dbg_ssthresh(s_lo_pcb) != 65535) { return 0; }   /* TCP_INIT_SSTHRESH */
    if (tcp_dbg_cwnd(s_lo_pcb) != 2 * 1460) { return 0; }    /* TCP_IW = 2 SMSS */

    /* Fenster oeffnen + Probe-Byte bestaetigen -> der Rest ("I") fliesst. */
    peer_send(LF_ACK, P + 3, G + 2, 8192, 0, 0);
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 1 || s.data[0] != 'I') { return 0; }
    cap_drain();

    peer_send(LF_RST, P + 3, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 3, G + 3)) { return 0; }
    return 1;
}

/* Szene 5: Retransmit nach RTO. Peer schickt Daten, der Guest echot (Fenster offen), aber der
 * Peer bestaetigt NICHT -> nach dem RTO muss der Guest die unbestaetigten Daten ERNEUT senden. */
static int scene_retransmit(void)
{
    struct seg s;
    uint32_t P = 4000, G = 0;
    s_recv_len = 0;
    if (!do_handshake(P, 8192, &G, 0)) { return 0; }

    /* "RTX" -> Guest echot 3 Bytes @ G+1, aber der Peer ACKt sie NICHT. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"RTX", 3);
    if (s_recv_len != 3 || memcmp(s_recv, "RTX", 3) != 0) { return 0; }
    int saw_echo = 0;
    while (cap_next(&s)) {
        if (s.dlen > 0) {
            saw_echo = 1;
            if (s.dlen != 3 || s.seq != G + 1) { return 0; }
        }
    }
    if (!saw_echo) { return 0; }

    /* Uhr ueber den RTO stellen + tcp_tick -> die unbestaetigten 3 Bytes werden ERNEUT gesendet. */
    net_test_advance_ms(600);
    tcp_tick(&s_lo);
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 3 || s.seq != G + 1 || memcmp(s.data, "RTX", 3) != 0) { return 0; }
    cap_drain();

    /* Jetzt bestaetigen -> Teardown. */
    peer_send(LF_ACK, P + 4, G + 4, 8192, 0, 0);
    cap_drain();
    peer_send(LF_RST, P + 4, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 4, G + 4)) { return 0; }
    return 1;
}

/* Szene 6: SACK (RFC 2018). Aushandlung (SACK-Permitted in beiden SYN) + empfangsseitige
 * SACK-Block-Ankuendigung: out-of-order Daten -> der Dup-ACK traegt einen SACK-Block der
 * gepufferten Strecke; Luecke gefuellt -> Bloecke verschwinden. Per Host-Interop nicht
 * ausloesbar (kein Reordering). */
static int scene_sack(void)
{
    struct seg s;
    uint32_t P = 6000, G = 0;
    static const uint8_t syn_sackperm[4] = { 4, 2, 1, 1 };   /* SACK-Permitted (kind 4 len 2) + NOP NOP */
    s_recv_len = 0;
    cap_drain();

    /* Handshake MIT SACK-Permitted -> Guest muss es im SYN-ACK bestaetigen. */
    peer_send_opt(LF_SYN, P, 0, 8192, 0, 0, syn_sackperm, 4);
    if (!cap_next(&s)) { return 0; }
    if (!((s.flags & LF_SYN) && (s.flags & LF_ACK) && s.ack == P + 1)) { return 0; }
    if (!s.sack_perm) { return 0; }                          /* SACK ausgehandelt */
    G = s.seq;
    peer_send(LF_ACK, P + 1, G + 1, 8192, 0, 0);
    cap_drain();

    /* Out-of-order "XYZ" @ P+4 (Luecke [P+1, P+4)) -> Dup-ACK (ack==P+1) mit SACK [P+4, P+7). */
    peer_send(LF_ACK | LF_PSH, P + 4, G + 1, 8192, (const uint8_t *)"XYZ", 3);
    if (!cap_next(&s)) { return 0; }
    if (s.ack != P + 1) { return 0; }                        /* kumulatives ACK haengt an der Luecke */
    if (s.sack_n != 1) { return 0; }                         /* genau ein gepufferter Block */
    if (s.sack[0] != P + 4 || s.sack[1] != P + 7) { return 0; }  /* deckt die out-of-order Strecke */
    if (s_recv_len != 0) { return 0; }                       /* noch nichts in-order zugestellt */
    cap_drain();

    /* Luecke fuellen "ABC" @ P+1 -> "ABCXYZ" in-order, ACK P+7, KEINE SACK-Bloecke mehr. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"ABC", 3);
    if (s_recv_len != 6 || memcmp(s_recv, "ABCXYZ", 6) != 0) { return 0; }
    if (!cap_next(&s)) { return 0; }
    if (s.ack != P + 7 || s.sack_n != 0) { return 0; }       /* Luecke zu -> keine SACK-Bloecke mehr */
    cap_drain();

    peer_send(LF_RST, P + 7, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 7, G + 1)) { return 0; }
    return 1;
}

/* Szene 7: SACK-Optionen auf einem Daten-Segment duerfen die MTU nicht sprengen (RFC 6691).
 * Ein OOO-Block bleibt gepuffert, waehrend ein grosses (1460 B) in-order Segment geechot wird ->
 * der Sendepfad MUSS die Daten so kuerzen, dass Daten + SACK-Option <= MTU bleiben. */
static int scene_sack_mss(void)
{
    struct seg s;
    uint32_t P = 7000, G = 0;
    static const uint8_t syn_sackperm[4] = { 4, 2, 1, 1 };
    static uint8_t big[1460];
    for (int i = 0; i < 1460; i++) { big[i] = (uint8_t)('A' + (i % 26)); }
    s_recv_len = 0;
    cap_drain();

    peer_send_opt(LF_SYN, P, 0, 8192, 0, 0, syn_sackperm, 4);
    if (!cap_next(&s)) { return 0; }
    if (!((s.flags & LF_SYN) && (s.flags & LF_ACK) && s.sack_perm)) { return 0; }
    G = s.seq;
    peer_send(LF_ACK, P + 1, G + 1, 8192, 0, 0);
    cap_drain();

    /* OOO 1 Byte weit oben -> ein gepufferter SACK-Block (Luecke davor bleibt offen). */
    peer_send(LF_ACK | LF_PSH, P + 1 + 2000, G + 1, 8192, (const uint8_t *)"Q", 1);
    cap_drain();

    /* Grosses (1460 B) in-order Segment fuellt die vordere Strecke -> Echo, waehrend der OOO-Block
     * noch gepuffert ist. Erstes Echo-Segment MUSS SACK tragen UND <= TCP_MSS-28 (1432) bleiben
     * (sonst Daten + SACK-Option > 1500-MTU). */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, big, 1460);
    int got = 0;
    while (cap_next(&s)) {
        if (s.dlen > 0 && !got) {
            got = 1;
            if (s.sack_n < 1) { return 0; }          /* OOO-Block noch gepuffert -> SACK vorhanden */
            if (s.dlen > 1460 - 28) { return 0; }    /* Daten um die SACK-Optionsbreite gekuerzt */
        }
    }
    if (!got) { return 0; }

    peer_send(LF_RST, P + 1461, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 1461, G + 1)) { return 0; }
    return 1;
}

/* Szene 8: SACK-Sender (eingehende SACK-Bloecke auswerten -> selektiver Retransmit). Der Guest
 * echot 12 Bytes in EINEM Segment @ G+1 (Nagle laesst es zu, da inflight==0); der Peer SACKt die
 * MITTE [G+5, G+9), bestaetigt aber kumulativ nichts (Luecke bei G+1). Nach dem RTO MUSS der
 * Retransmit nur die erste Luecke ("AAAA"@G+1) senden und die SACKte Mitte ueberspringen. */
static int scene_sack_sender(void)
{
    struct seg s;
    uint32_t P = 8000, G = 0;
    static const uint8_t syn_sackperm[4] = { 4, 2, 1, 1 };
    s_recv_len = 0;
    cap_drain();

    peer_send_opt(LF_SYN, P, 0, 8192, 0, 0, syn_sackperm, 4);
    if (!cap_next(&s)) { return 0; }
    if (!((s.flags & LF_SYN) && (s.flags & LF_ACK) && s.sack_perm)) { return 0; }
    G = s.seq;
    peer_send(LF_ACK, P + 1, G + 1, 8192, 0, 0);
    cap_drain();

    /* 12 Bytes in-order -> Guest echot sie als EIN Segment @ G+1 (inflight==0); Peer ACKt NICHT. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"AAAABBBBCCCC", 12);
    cap_drain();

    /* SACK die MITTE [G+5, G+9) (= "BBBB", Bytes 5..8 des Echos); kumulatives ACK bleibt G+1. */
    uint8_t sackopt[12];
    uint32_t le = G + 5, re = G + 9;
    sackopt[0] = 1; sackopt[1] = 1; sackopt[2] = 5; sackopt[3] = 10;
    sackopt[4] = (uint8_t)(le >> 24); sackopt[5] = (uint8_t)(le >> 16);
    sackopt[6] = (uint8_t)(le >> 8);  sackopt[7] = (uint8_t)le;
    sackopt[8] = (uint8_t)(re >> 24); sackopt[9] = (uint8_t)(re >> 16);
    sackopt[10] = (uint8_t)(re >> 8); sackopt[11] = (uint8_t)re;
    peer_send_opt(LF_ACK, P + 13, G + 1, 8192, 0, 0, sackopt, 12);
    cap_drain();

    /* RTO -> Retransmit ueberspringt die SACKte Mitte: nur "AAAA"@G+1 (4 B), NICHT 12 B ab snd_una. */
    net_test_advance_ms(600);
    tcp_tick(&s_lo);
    if (!cap_next(&s)) { return 0; }
    if (s.seq != G + 1 || s.dlen != 4 || memcmp(s.data, "AAAA", 4) != 0) { return 0; }
    cap_drain();

    peer_send(LF_RST, P + 13, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 13, G + 1)) { return 0; }
    return 1;
}

/* Szene 9: boesartiger SACK-Block (Sicherheit). Ein Block mit le weit VOR und re per Seq-Wrap
 * HINTER snd_una darf die Markier-Schleife NICHT ~2^31x laufen lassen (Remote-DoS). Beweis:
 * der Guest verarbeitet danach ein normales Segment weiter (er haengt nicht). */
static int scene_sack_evil(void)
{
    struct seg s;
    uint32_t P = 9000, G = 0;
    static const uint8_t syn_sackperm[4] = { 4, 2, 1, 1 };
    s_recv_len = 0;
    cap_drain();

    peer_send_opt(LF_SYN, P, 0, 8192, 0, 0, syn_sackperm, 4);
    if (!cap_next(&s)) { return 0; }
    if (!((s.flags & LF_SYN) && (s.flags & LF_ACK) && s.sack_perm)) { return 0; }
    G = s.seq;
    peer_send(LF_ACK, P + 1, G + 1, 8192, 0, 0);
    cap_drain();

    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"AAAA", 4);   /* In-Flight-Daten */
    cap_drain();

    /* Boesartiger Block: le = snd_una+0x7FFFFFFF, re = snd_una+0xFFFFF800 (= snd_una-2048 per Wrap). */
    uint32_t le = (G + 1) + 0x7FFFFFFFu, re = (G + 1) + 0xFFFFF800u;
    uint8_t opt[12];
    opt[0] = 1; opt[1] = 1; opt[2] = 5; opt[3] = 10;
    opt[4] = (uint8_t)(le >> 24); opt[5] = (uint8_t)(le >> 16);
    opt[6] = (uint8_t)(le >> 8);  opt[7] = (uint8_t)le;
    opt[8] = (uint8_t)(re >> 24); opt[9] = (uint8_t)(re >> 16);
    opt[10] = (uint8_t)(re >> 8); opt[11] = (uint8_t)re;
    peer_send_opt(LF_ACK, P + 5, G + 1, 8192, 0, 0, opt, 12);
    cap_drain();

    /* Guest lebt noch: AAAA bestaetigen (inflight->0, damit Nagle das naechste Echo zulaesst),
     * dann ein weiteres in-order Segment -> wird normal geechot (@ G+5). */
    peer_send(LF_ACK, P + 5, G + 5, 8192, 0, 0);
    cap_drain();
    peer_send(LF_ACK | LF_PSH, P + 5, G + 5, 8192, (const uint8_t *)"BBBB", 4);
    int got = 0;
    while (cap_next(&s)) {
        if (s.dlen == 4 && s.seq == G + 5 && memcmp(s.data, "BBBB", 4) == 0) { got = 1; }
    }
    if (!got) { return 0; }   /* kein Echo -> Guest haengt am boesartigen Block */

    peer_send(LF_RST, P + 9, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 9, G + 1)) { return 0; }
    return 1;
}

/* Szene 10: TCP-Timestamps (RFC 7323) + adaptives RTO (Jacobson). Aushandlung; eine gemessene
 * RTT von 300ms muss das RTO auf ~900ms (SRTT+4*RTTVAR) heben -- NICHT bei den fixen 500ms
 * bleiben. Beweis: bei +600ms feuert KEIN Retransmit, erst bei +1000ms (RTO liegt dazwischen). */
static int scene_rto(void)
{
    struct seg s;
    uint32_t P = 10000, G = 0;
    /* SYN: SACK-Permitted {4,2} + NOP NOP + Timestamps(TSval=0x1000,TSecr=0) + NOP NOP (16 B). */
    uint8_t syn[16] = { 4, 2, 1, 1, 8, 10, 0x00, 0x00, 0x10, 0x00, 0, 0, 0, 0, 1, 1 };
    uint8_t o[12];
    s_recv_len = 0;
    cap_drain();

    peer_send_opt(LF_SYN, P, 0, 8192, 0, 0, syn, 16);
    if (!cap_next(&s)) { return 0; }
    if (!((s.flags & LF_SYN) && (s.flags & LF_ACK) && s.has_ts)) { return 0; }   /* TS ausgehandelt */
    G = s.seq;
    build_peer_ts(o, 0x1001, 0);   /* Handshake-ACK: TSecr=0 -> Guest misst hier KEINE RTT */
    peer_send_opt(LF_ACK, P + 1, G + 1, 8192, 0, 0, o, 12);
    cap_drain();

    /* Daten mit TS -> Guest echot mit SEINEM TSval T1; das lesen wir aus dem Echo. */
    build_peer_ts(o, 0x2000, s.ts_val);
    peer_send_opt(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"AAAA", 4, o, 12);
    uint32_t t1 = 0; int got = 0;
    while (cap_next(&s)) { if (s.dlen == 4 && s.has_ts) { t1 = s.ts_val; got = 1; } }
    if (!got) { return 0; }

    /* +300ms, dann Echo bestaetigen mit TSecr=T1 -> Guest misst RTT=300 -> RTO ~900. */
    net_test_advance_ms(300);
    build_peer_ts(o, 0x2001, t1);
    peer_send_opt(LF_ACK, P + 5, G + 5, 8192, 0, 0, o, 12);
    cap_drain();

    /* Neue Daten -> Echo (startet den RTO-Timer). +600 < RTO(900): KEIN Retransmit; +400 mehr: JA. */
    build_peer_ts(o, 0x2002, t1);
    peer_send_opt(LF_ACK | LF_PSH, P + 5, G + 5, 8192, (const uint8_t *)"BBBB", 4, o, 12);
    cap_drain();
    net_test_advance_ms(600);
    tcp_tick(&s_lo);
    while (cap_next(&s)) { if (s.dlen > 0) { return 0; } }   /* RTO adaptiv (>600) -> noch kein Retransmit */
    net_test_advance_ms(400);
    tcp_tick(&s_lo);
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 4 || s.seq != G + 5) { return 0; }        /* jetzt (>~900) Retransmit "BBBB"@G+5 */
    cap_drain();

    peer_send(LF_RST, P + 9, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 9, G + 1)) { return 0; }
    return 1;
}

/* Szene 11: Nagle (RFC 896) + Delayed-ACK (RFC 1122). (a) Delayed-ACK: empfangene Daten -> das
 * Echo traegt das ACK huckepack, KEIN separates reines ACK. (b) Nagle: ein kleines Echo wird
 * zurueckgehalten, solange unbestaetigte Daten unterwegs sind, und erst nach dem ACK gesendet. */
static int scene_nagle(void)
{
    struct seg s;
    uint32_t P = 11000, G = 0;
    s_recv_len = 0;
    if (!do_handshake(P, 8192, &G, 0)) { return 0; }

    /* Delayed-ACK: "XY" -> Guest echot "XY"@G+1; das ACK reist HUCKEPACK -> GENAU EIN Segment. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"XY", 2);
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 2 || s.seq != G + 1 || s.ack != P + 3) { return 0; }   /* Echo traegt das ACK */
    while (cap_next(&s)) { return 0; }                                   /* KEIN zweites (reines) ACK */

    /* Nagle: "Z" (1 Byte) waehrend "XY" unbestaetigt -> Echo ZURUECKGEHALTEN (nichts gesendet). */
    peer_send(LF_ACK | LF_PSH, P + 3, G + 1, 8192, (const uint8_t *)"Z", 1);
    while (cap_next(&s)) { if (s.dlen > 0) { return 0; } }               /* Nagle haelt das kleine Echo */

    /* "XY" bestaetigen -> inflight=0 -> das zurueckgehaltene "Z" wird JETZT gesendet. */
    peer_send(LF_ACK, P + 4, G + 3, 8192, 0, 0);
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 1 || s.seq != G + 3 || s.data[0] != 'Z') { return 0; }
    cap_drain();

    peer_send(LF_RST, P + 4, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 4, G + 3)) { return 0; }
    return 1;
}

/* Szene 12: Delayed-ACK-TIMER-Flush isoliert. Peer-Fenster 0 -> das Echo ist blockiert (kein
 * huckepack-ACK); die Uhr wird UEBER die ACK-Verzoegerung (200ms), aber UNTER das RTO (500ms)
 * gestellt -> NUR der Delayed-ACK-Timer feuert (kein Persist/Retransmit) -> genau EIN reines ACK.
 * Pinnt den tcp_tick-Flush, den sonst kein Szenario ausloest (Echo/Persist traegt das ACK immer). */
static int scene_dack(void)
{
    struct seg s;
    uint32_t P = 12000, G = 0;
    s_recv_len = 0;
    if (!do_handshake(P, 0, &G, 0)) { return 0; }   /* Peer-Fenster 0 */

    /* In-order "HI" -> Guest will echoen, aber Fenster 0 -> nichts gesendet -> Delayed-ACK. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 0, (const uint8_t *)"HI", 2);
    while (cap_next(&s)) { if (s.dlen > 0) { return 0; } }   /* kein Echo, kein sofortiges ACK */

    /* Uhr > ACK-Delay(200) aber < RTO(500) -> nur der Delayed-ACK-Timer feuert. */
    net_test_advance_ms(250);
    tcp_tick(&s_lo);
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 0 || s.ack != P + 3) { return 0; }        /* GENAU ein reines ACK bis P+3 */
    while (cap_next(&s)) { return 0; }
    cap_drain();

    peer_send(LF_RST, P + 3, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 3, G + 1)) { return 0; }
    return 1;
}

/* Szene 13: Window Scaling (RFC 7323). Der Peer bietet WScale=8 im SYN an -> der Guest MUSS es im
 * SYN-ACK bestaetigen (WScale-Option mit UNSEREM Shift TCP_RCV_WSCALE=0). Danach kuendigt der Peer
 * ein RAW-Fenster von 2 an, das MIT Shift 8 zu 512 skaliert. Ein 100-Byte-Echo passt nur ins
 * SKALIERTE Fenster: ohne Skalierung (Bug) haette der Sendepfad usable=2 -> nur 2 Bytes gesendet,
 * den Rest von Nagle gehalten. Pinnt, dass empfangene Fenster nach dem Handshake mit snd_wscale
 * skaliert gelesen werden (der einzige beobachtbare Nutzen von WScale bei unserem 2-KiB-Puffer). */
static int scene_wscale(void)
{
    struct seg s;
    uint32_t P = 13000, G = 0;
    static const uint8_t syn_wscale[4] = { 3, 3, 8, 1 };   /* WScale (kind 3 len 3, shift 8) + NOP */
    static uint8_t data100[100];
    for (int i = 0; i < 100; i++) { data100[i] = (uint8_t)('A' + (i % 26)); }
    s_recv_len = 0;
    cap_drain();

    /* Handshake MIT WScale=8 -> Guest bestaetigt WScale (mit unserem Shift 0). */
    peer_send_opt(LF_SYN, P, 0, 8192, 0, 0, syn_wscale, 4);
    if (!cap_next(&s)) { return 0; }
    if (!((s.flags & LF_SYN) && (s.flags & LF_ACK) && s.ack == P + 1)) { return 0; }
    if (!s.has_wscale || s.wscale != 0) { return 0; }      /* WScale ausgehandelt, unser Shift 0 */
    G = s.seq;

    /* Handshake-ACK mit RAW-Fenster 2 -> skaliert 2<<8 = 512 (erst nach ESTABLISHED angewandt). */
    peer_send(LF_ACK, P + 1, G + 1, 2, 0, 0);
    cap_drain();

    /* Peer schickt 100 Bytes (Fenster raw=2 -> skaliert 512). Der Guest echot 100 in EINEM Segment:
     * usable = 512 - 0 >= 100, Nagle erlaubt es (inflight==0). OHNE Skalierung waere usable=2. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 2, data100, 100);
    if (s_recv_len != 100 || memcmp(s_recv, data100, 100) != 0) { return 0; }
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 100) { return 0; }                       /* volles Echo -> skaliertes Fenster genutzt */
    if (s.seq != G + 1 || s.ack != P + 101) { return 0; }
    while (cap_next(&s)) { if (s.dlen > 0) { return 0; } } /* kein zweites Datensegment */
    cap_drain();

    peer_send(LF_RST, P + 101, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 101, G + 1)) { return 0; }
    return 1;
}

/* Szene 14: ACTIVE OPEN (der Guest ist der Opener via tcp_connect) -- deckt den bislang von KEINER
 * Szene beruehrten aktiven Aushandlungspfad ab (WScale + SACK + Timestamps im ausgehenden SYN,
 * Bestaetigung im SYN-ACK). Pinnt zusaetzlich den RFC-7323-2.3-Fix: das Fenster eines
 * (retransmittierten) SYN-ACK darf NICHT skaliert werden. Ablauf:
 *  1) tcp_connect -> Guest-SYN traegt WScale/SACK-Perm/TS.
 *  2) Peer-SYN-ACK (WScale=7, SACK, TS, UNSKALIERTES Fenster=4) -> ESTABLISHED, Client schreibt 1500 B
 *     -> nur 4 gehen raus (Fenster 4); der Rest (>= eff_mss) ist NICHT von Nagle, sondern vom
 *     Fenster gehalten.
 *  3) Peer RETRANSMITTIERT das SYN-ACK (Fenster 4). MIT Bug wuerde 4<<7=512 einen Burst senden;
 *     der Fix laesst snd_wnd bei 4 -> KEIN weiteres Datensegment. */
static int scene_wscale_active(void)
{
    struct seg s;
    uint32_t P = 15000, G = 0;
    uint16_t gport = 0;
    uint32_t gts = 0;
    for (int i = 0; i < (int)sizeof(s_ao_payload); i++) { s_ao_payload[i] = (uint8_t)('a' + (i % 26)); }
    s_recv_len = 0; s_ao_conn = -1;
    cap_drain();

    /* (1) Guest aktiv verbinden -> ausgehendes SYN mit WScale + SACK-Perm + Timestamps. */
    tcp_pcb_t *pcb = tcp_connect(&s_lo, LO_PEER_IP, LO_PEER_PORT, ao_conn, ao_recv);
    if (!pcb) { return 0; }
    if (!cap_next(&s)) { return 0; }
    if (!(s.flags & LF_SYN) || (s.flags & LF_ACK)) { return 0; }   /* reines SYN */
    if (!s.has_wscale || s.wscale != 0) { return 0; }              /* WScale aktiv angeboten (Shift 0) */
    if (!s.sack_perm) { return 0; }                                /* SACK-Permitted aktiv angeboten */
    if (!s.has_ts) { return 0; }                                   /* Timestamps aktiv angeboten */
    gport = s.sport;                                               /* Guest-Ephemeral-Port */
    G = s.seq; gts = s.ts_val;
    cap_drain();

    /* (2) SYN-ACK vom Peer: WScale=7, SACK-Perm, TS (TSecr = Guest-TSval), Fenster UNSKALIERT = 4. */
    uint8_t opt[20]; int o = 0;
    opt[o++] = 1; opt[o++] = 3; opt[o++] = 3; opt[o++] = 7;         /* NOP + WScale=7 */
    opt[o++] = 1; opt[o++] = 1; opt[o++] = 4; opt[o++] = 2;         /* NOP NOP + SACK-Permitted */
    opt[o++] = 1; opt[o++] = 1; opt[o++] = 8; opt[o++] = 10;        /* NOP NOP + TS */
    uint32_t tv = 0x9000;
    opt[o++] = (uint8_t)(tv >> 24); opt[o++] = (uint8_t)(tv >> 16);
    opt[o++] = (uint8_t)(tv >> 8);  opt[o++] = (uint8_t)tv;
    opt[o++] = (uint8_t)(gts >> 24); opt[o++] = (uint8_t)(gts >> 16);
    opt[o++] = (uint8_t)(gts >> 8);  opt[o++] = (uint8_t)gts;       /* o == 20 */
    peer_xmit(LO_PEER_PORT, gport, LF_SYN | LF_ACK, P, G + 1, 4, 0, 0, opt, 20);

    if (s_ao_conn != 1) { return 0; }                              /* on_connect(ok) gefeuert */
    /* Erwartung: Handshake-ACK (dlen=0) + GENAU ein 4-Byte-Datensegment (Fenster 4 voll genutzt),
     * NIE mehr als 4 (unskaliertes SYN-ACK-Fenster korrekt gelesen). */
    int data_seen = 0;
    while (cap_next(&s)) {
        if (s.dlen == 0) { continue; }                             /* Handshake-ACK / re-ACK */
        if (s.dlen != 4) { return 0; }                             /* Fenster=4 -> genau 4 Byte */
        if (s.seq != G + 1) { return 0; }
        data_seen++;
    }
    if (data_seen != 1) { return 0; }
    cap_drain();

    /* (3) RETRANSMITTIERTES SYN-ACK (Fenster 4). Der Fix (SYN-Fenster nicht skalieren) laesst
     * snd_wnd bei 4 -> KEIN weiteres Datensegment. Mit dem Bug wuerde 4<<7=512 die restlichen
     * 96 Bytes als Out-of-Window-Burst senden. */
    peer_xmit(LO_PEER_PORT, gport, LF_SYN | LF_ACK, P, G + 1, 4, 0, 0, opt, 20);
    while (cap_next(&s)) {
        if (s.dlen > 0) { return 0; }                              /* KEIN Burst -> Fix aktiv */
    }
    cap_drain();

    /* Teardown: RST (seq == Guest rcv_nxt == P+1) gibt den aktiven PCB frei. */
    peer_xmit(LO_PEER_PORT, gport, LF_RST, P + 1, 0, 0, 0, 0, 0, 0);
    if (!expect_pcb_freed_ports(LO_PEER_PORT, gport, P + 1, G + 5)) { return 0; }
    return 1;
}

/* Szene 15: PAWS (RFC 7323 5). Nach Timestamps-Aushandlung wird ein Segment mit einem AELTEREN
 * TSval als TS.Recent verworfen (mit ACK zum Resync), waehrend ein Segment mit frischem TSval an
 * DERSELBEN Sequenznummer angenommen wird. Pinnt sowohl die PAWS-Pruefung als auch die
 * TS.Recent-Update-Regel (nur in-order-abdeckende Segmente aktualisieren): das SYN traegt TSval=100,
 * "AAAA" traegt 1000 -> TS.Recent MUSS auf 1000 steigen, sonst wuerde "BBBB"@500 nicht verworfen. */
static int scene_paws(void)
{
    struct seg s;
    uint32_t P = 17000, G = 0;
    uint8_t o[12];
    s_recv_len = 0;
    cap_drain();

    /* Handshake MIT Timestamps (SYN TSval=100). */
    build_peer_ts(o, 100, 0);
    peer_send_opt(LF_SYN, P, 0, 8192, 0, 0, o, 12);
    if (!cap_next(&s)) { return 0; }
    if (!((s.flags & LF_SYN) && (s.flags & LF_ACK) && s.ack == P + 1)) { return 0; }
    if (!s.has_ts) { return 0; }                                   /* Timestamps ausgehandelt */
    G = s.seq;
    build_peer_ts(o, 101, s.ts_val);                               /* Handshake-ACK: TSecr=Guest-TSval */
    peer_send_opt(LF_ACK, P + 1, G + 1, 8192, 0, 0, o, 12);
    cap_drain();

    /* In-order "AAAA" @ P+1, TSval=1000 -> zugestellt+geechot, TS.Recent := 1000. */
    build_peer_ts(o, 1000, 200);
    peer_send_opt(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"AAAA", 4, o, 12);
    if (s_recv_len != 4 || memcmp(s_recv, "AAAA", 4) != 0) { return 0; }
    cap_drain();

    /* ALTES "BBBB" @ P+5, TSval=500 (< TS.Recent 1000) -> PAWS verwirft: KEINE Zustellung, aber
     * ein aktuelles ACK (ack == rcv_nxt == P+5) wird geschickt. */
    build_peer_ts(o, 500, 200);
    peer_send_opt(LF_ACK | LF_PSH, P + 5, G + 5, 8192, (const uint8_t *)"BBBB", 4, o, 12);
    if (s_recv_len != 4) { return 0; }                             /* "BBBB" NICHT zugestellt */
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 0 || s.ack != P + 5) { return 0; }               /* reines Resync-ACK bis P+5 */
    while (cap_next(&s)) { return 0; }
    cap_drain();

    /* FRISCHES "CCCC" @ P+5, TSval=1500 (>= TS.Recent) -> angenommen, zugestellt+geechot. */
    build_peer_ts(o, 1500, 200);
    peer_send_opt(LF_ACK | LF_PSH, P + 5, G + 5, 8192, (const uint8_t *)"CCCC", 4, o, 12);
    if (s_recv_len != 8 || memcmp(s_recv, "AAAACCCC", 8) != 0) { return 0; }
    if (!cap_next(&s)) { return 0; }
    if (s.ack != P + 9) { return 0; }                              /* kumulativ bis P+9 -> TS.Recent=1500 */
    cap_drain();

    /* TS.Recent-Update-Regel: ein ZUKUENFTIGES (out-of-order) Segment "EEEE" @ P+13 mit GROSSEM
     * TSval=3000 darf TS.Recent NICHT anheben (deckt rcv_nxt=P+9 nicht ab). Sonst wuerde der spaeter
     * eintreffende Luecken-Fueller "DDDD"@2000 von PAWS faelschlich verworfen. */
    build_peer_ts(o, 3000, 200);
    peer_send_opt(LF_ACK | LF_PSH, P + 13, G + 9, 8192, (const uint8_t *)"EEEE", 4, o, 12);
    if (s_recv_len != 8) { return 0; }                             /* OOO -> nichts Neues zugestellt */
    cap_drain();                                                    /* Dup-ACK verwerfen */

    /* Luecken-Fueller "DDDD" @ P+9, TSval=2000 (< 3000, aber >= korrektes TS.Recent 1500) ->
     * MUSS angenommen werden (TS.Recent blieb 1500) und fuellt die Luecke: "DDDDEEEE" in-order. */
    build_peer_ts(o, 2000, 200);
    peer_send_opt(LF_ACK | LF_PSH, P + 9, G + 9, 8192, (const uint8_t *)"DDDD", 4, o, 12);
    if (s_recv_len != 16 || memcmp(s_recv, "AAAACCCCDDDDEEEE", 16) != 0) { return 0; }
    cap_drain();

    peer_send(LF_RST, P + 17, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 17, G + 17)) { return 0; }
    return 1;
}

/* Szene 16: Fast Retransmit / Fast Recovery (RFC 5681 3.2). Der Guest hat ein Segment im Flug;
 * der Peer schickt 3 Dup-ACKs (ohne neue Bestaetigung) -> das fehlende Segment wird SOFORT erneut
 * gesendet (ohne aufs RTO zu warten, KEIN Uhr-Vorlauf) und cwnd geht in Fast Recovery. */
static int scene_fast_rtx(void)
{
    struct seg s;
    uint32_t P = 19000, G = 0;
    const uint32_t SMSS = 1460;
    s_recv_len = 0;
    if (!do_handshake(P, 8192, &G, 0)) { return 0; }   /* grosses Fenster: snd_wnd begrenzt nicht */

    /* "HELLO" -> Echo "HELLO"@G+1 (5 B im Flug, snd_una=G+1). Setzt s_lo_pcb. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"HELLO", 5);
    if (s_recv_len != 5 || memcmp(s_recv, "HELLO", 5) != 0) { return 0; }
    if (!cap_next(&s) || s.dlen != 5 || s.seq != G + 1) { return 0; }   /* das Echo */
    cap_drain();

    /* 3 Dup-ACKs (ack=G+1) OHNE Uhr-Vorlauf. Die ersten zwei loesen NICHTS aus; der dritte
     * triggert Fast Retransmit von "HELLO"@G+1 -- lange vor dem RTO (Uhr steht). */
    peer_send(LF_ACK, P + 6, G + 1, 8192, 0, 0);
    if (cap_next(&s)) { return 0; }                    /* Dup-ACK 1 -> kein Retransmit */
    peer_send(LF_ACK, P + 6, G + 1, 8192, 0, 0);
    if (cap_next(&s)) { return 0; }                    /* Dup-ACK 2 -> kein Retransmit */
    peer_send(LF_ACK, P + 6, G + 1, 8192, 0, 0);       /* Dup-ACK 3 -> Fast Retransmit */
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 5 || s.seq != G + 1 || memcmp(s.data, "HELLO", 5) != 0) { return 0; }
    while (cap_next(&s)) { if (s.dlen > 0) { return 0; } }   /* genau EIN Retransmit */
    cap_drain();

    /* Fast Recovery: ssthresh = max(inflight/2, 2 SMSS) = 2 SMSS (inflight=5); cwnd = ssthresh + 3 SMSS. */
    if (tcp_dbg_ssthresh(s_lo_pcb) != 2 * SMSS) { return 0; }
    if (tcp_dbg_cwnd(s_lo_pcb) != 5 * SMSS) { return 0; }

    peer_send(LF_RST, P + 6, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 6, G + 6)) { return 0; }
    return 1;
}

/* Szene 17: cwnd-Uebergaenge (RFC 5681) white-box. Initial Window nach dem Handshake, Slow-Start-
 * Wachstum bei neuem ACK, und der cwnd-Kollaps auf 1 SMSS beim RTO-Verlust. Mit den 2-KiB-Puffern
 * sind diese cwnd-Werte nicht rein am Draht sichtbar -> tcp_dbg_cwnd/ssthresh. */
static int scene_cwnd(void)
{
    struct seg s;
    uint32_t P = 21000, G = 0;
    const uint32_t SMSS = 1460;
    s_recv_len = 0;
    if (!do_handshake(P, 8192, &G, 0)) { return 0; }

    /* "AAAA" -> Echo (setzt s_lo_pcb). cwnd waechst beim SENDEN nicht (nur bei ACK) -> noch IW. */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, (const uint8_t *)"AAAA", 4);
    if (s_recv_len != 4) { return 0; }
    cap_drain();
    if (tcp_dbg_cwnd(s_lo_pcb) != 2 * SMSS) { return 0; }          /* Initial Window (2 SMSS) */

    /* Echo (G+1..G+5) bestaetigen -> Slow Start: cwnd += min(4, SMSS) = 4. */
    peer_send(LF_ACK, P + 5, G + 5, 8192, 0, 0);
    if (tcp_dbg_cwnd(s_lo_pcb) != 2 * SMSS + 4) { return 0; }      /* Slow-Start-Wachstum */

    /* "BB" -> Echo (2 B im Flug); Peer bestaetigt NICHT -> RTO -> Retransmit + cwnd-Kollaps. */
    peer_send(LF_ACK | LF_PSH, P + 5, G + 5, 8192, (const uint8_t *)"BB", 2);
    if (s_recv_len != 6) { return 0; }
    cap_drain();
    net_test_advance_ms(600);                                      /* > RTO (500ms) */
    tcp_tick(&s_lo);
    if (!cap_next(&s) || s.dlen != 2 || s.seq != G + 5) { return 0; }   /* RTO-Retransmit "BB" */
    /* RFC 5681 3.1: ssthresh = max(inflight/2, 2 SMSS) = 2 SMSS (inflight=2); cwnd = 1 SMSS. */
    if (tcp_dbg_ssthresh(s_lo_pcb) != 2 * SMSS) { return 0; }
    if (tcp_dbg_cwnd(s_lo_pcb) != SMSS) { return 0; }
    cap_drain();

    peer_send(LF_RST, P + 7, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 7, G + 7)) { return 0; }
    return 1;
}

/* Szene 18: TCP Keepalive (RFC 1122 4.2.3.6). Eine untaetige ESTABLISHED-Verbindung sendet nach
 * TCP_KEEPIDLE eine Probe (ACK @ snd_una-1, ohne Daten). Ein lebender Peer haelt die Verbindung
 * durch seine Antwort; ein schweigender Peer fuehrt nach TCP_KEEPCNT Proben zur Freigabe. */
static int scene_keepalive(void)
{
    struct seg s;
    uint32_t P = 25000, G = 0;
    const int KEEPCNT = 4;   /* == TCP_KEEPCNT in tcp.c */
    s_recv_len = 0;
    if (!do_handshake(P, 8192, &G, 0)) { return 0; }

    /* Ein LEBENDER, aber stiller Peer: MEHR als TCP_KEEPCNT Antwort-Zyklen. Jeder Zyklus MUSS
     * genau eine Keepalive-Probe (seq == snd_una-1 == G, ack==P+1, keine Daten) senden, und die
     * Verbindung MUSS ueberleben -- das pinnt den ka_probes-Reset bei Empfang: ohne ihn saehe der
     * Peer nach TCP_KEEPCNT Zyklen keine Probe mehr (PCB waere trotz Antworten freigegeben). */
    for (int k = 0; k < KEEPCNT + 2; k++) {
        net_test_advance_ms(11000);            /* > TCP_KEEPIDLE (10s) */
        tcp_tick(&s_lo);
        if (!cap_next(&s)) { return 0; }        /* Probe -> PCB lebt trotz vieler Zyklen */
        if (s.dlen != 0 || s.seq != G || s.ack != P + 1) { return 0; }
        while (cap_next(&s)) { if (s.dlen > 0) { return 0; } }   /* GENAU eine Probe pro Zyklus */
        peer_send(LF_ACK, P + 1, G + 1, 8192, 0, 0);   /* lebender Peer antwortet -> Reset */
        cap_drain();
    }

    /* Jetzt schweigt der Peer. GENAU TCP_KEEPCNT Proben gehen noch raus (der PCB lebt jeweils),
     * die (TCP_KEEPCNT+1)-te Faelligkeit gibt ihn frei -- pinnt den exakten Zaehler (eine zu
     * niedrige Schwelle liesse eine Probe fehlen, eine zu hohe eine ueberzaehlige Probe zu). */
    net_test_advance_ms(11000);                /* erste unbeantwortete Probe: Untaetigkeit > KEEPIDLE */
    for (int k = 0; k < KEEPCNT; k++) {
        if (k > 0) { net_test_advance_ms(2500); }   /* Folgeproben: > TCP_KEEPINTVL (2s) */
        tcp_tick(&s_lo);
        if (!cap_next(&s)) { return 0; }        /* Probe k+1 -> PCB lebt noch */
        if (s.dlen != 0 || s.seq != G) { return 0; }
        while (cap_next(&s)) { if (s.dlen > 0) { return 0; } }
    }
    net_test_advance_ms(2500);                  /* (TCP_KEEPCNT+1)-te Faelligkeit */
    tcp_tick(&s_lo);
    while (cap_next(&s)) { return 0; }          /* KEINE weitere Probe -> PCB wurde freigegeben */
    if (!expect_pcb_freed(P + 1, G + 1)) { return 0; }   /* PCB tot -> Streu-ACK loest RST aus */
    return 1;
}

/* Szene 19: Receiver-SWS-Avoidance (RFC 1122 4.2.3.3). Der Empfaenger darf die rechte Fensterkante
 * nicht um einen kleinen Betrag (< min(MSS, RCVBUF/2) = 1024) nach rechts schieben -- sonst lockt
 * eine winzige Fensteroeffnung den Sender zu Klein-Segmenten. Aufbau: OOO-Daten schrumpfen das
 * Fenster auf 648; eine kleine in-order Zustellung (100 B) wuerde es auf 648 oeffnen, aber SWS
 * HAELT bei 548 (Oeffnung 100 < 1024). Peer-Fenster gross, sodass das Echo abfliesst und die
 * Ankuendigung vom Empfangspuffer (nicht vom Echo-Backpressure) bestimmt wird. */
static int scene_sws_recv(void)
{
    struct seg s;
    uint32_t P = 27000, G = 0;
    uint16_t w0 = 0;
    static uint8_t ooo[1400];
    static uint8_t fill[100];
    for (int i = 0; i < 1400; i++) { ooo[i]  = (uint8_t)('a' + (i % 26)); }
    for (int i = 0; i < 100;  i++) { fill[i] = (uint8_t)('A' + (i % 26)); }
    s_recv_len = 0;
    if (!do_handshake(P, 8192, &G, &w0)) { return 0; }
    if (w0 != 2048) { return 0; }                          /* volles Fenster nach dem Handshake */

    /* OOO 1400 B @ P+601 (Luecke [P+1,P+601)) -> Dup-ACK, Fenster schrumpft auf 2048-1400=648
     * (Schrumpfen wird NICHT von SWS gehalten). */
    peer_send(LF_ACK | LF_PSH, P + 601, G + 1, 8192, ooo, 1400);
    if (!cap_next(&s)) { return 0; }
    if (s.ack != P + 1 || s.window != 648) { return 0; }
    if (s_recv_len != 0) { return 0; }                     /* noch nichts in-order zugestellt */
    cap_drain();

    /* 100 B in-order @ P+1 -> zugestellt + geechot. Die Fensteroeffnung ist nur 100 (< 1024) ->
     * Receiver-SWS HAELT die alte Kante: angekuendigt bleibt 548 (nicht 648). */
    peer_send(LF_ACK | LF_PSH, P + 1, G + 1, 8192, fill, 100);
    if (s_recv_len != 100 || memcmp(s_recv, fill, 100) != 0) { return 0; }
    if (!cap_next(&s)) { return 0; }
    if (s.dlen != 100 || s.seq != G + 1) { return 0; }
    if (s.window != 548) { return 0; }                     /* SWS: kleine Oeffnung zurueckgehalten */
    cap_drain();

    peer_send(LF_RST, P + 101, 0, 0, 0, 0);
    if (!expect_pcb_freed(P + 101, G + 101)) { return 0; }
    return 1;
}

int tcp_looptest_run(void)
{
    memset(&s_lo, 0, sizeof(s_lo));
    s_lo.ip = LO_GUEST_IP;
    s_lo.netmask = IP4(255, 255, 255, 0);
    s_lo.gateway = LO_PEER_IP;
    s_lo.mac.b[0] = 0x52; s_lo.mac.b[1] = 0x54; s_lo.mac.b[5] = 0x01;
    s_lo.transmit = lo_transmit;
    s_cap_head = s_cap_tail = 0;

    /* Der Guest antwortet an LO_PEER_IP -> ARP vorab seeden (kein ARP-Roundtrip noetig). */
    mac_addr_t pmac;
    memcpy(pmac.b, s_peer_mac, ETH_ALEN);
    arp_cache_put(LO_PEER_IP, &pmac);

    tcp_listen(LO_TEST_PORT, lo_handler);
    net_test_reset_time();        /* sauberer Uhr-Offset zu Beginn */

    int ok = 1;
    if (!scene_out_of_order())  { uart_puts("[looptest] Szene OOO FEHLER\n"); ok = 0; }
    if (!scene_send_window())   { uart_puts("[looptest] Szene Sende-Fenster FEHLER\n"); ok = 0; }
    if (!scene_fin_overshoot()) { uart_puts("[looptest] Szene FIN-Overshoot FEHLER\n"); ok = 0; }
    if (!scene_persist())       { uart_puts("[looptest] Szene Zero-Window-Persist FEHLER\n"); ok = 0; }
    if (!scene_retransmit())    { uart_puts("[looptest] Szene Retransmit FEHLER\n"); ok = 0; }
    if (!scene_sack())          { uart_puts("[looptest] Szene SACK FEHLER\n"); ok = 0; }
    if (!scene_sack_mss())      { uart_puts("[looptest] Szene SACK-MTU FEHLER\n"); ok = 0; }
    if (!scene_sack_sender())   { uart_puts("[looptest] Szene SACK-Sender FEHLER\n"); ok = 0; }
    if (!scene_sack_evil())     { uart_puts("[looptest] Szene SACK-Evil FEHLER\n"); ok = 0; }
    if (!scene_rto())           { uart_puts("[looptest] Szene RTO/Timestamps FEHLER\n"); ok = 0; }
    if (!scene_nagle())         { uart_puts("[looptest] Szene Nagle/Delayed-ACK FEHLER\n"); ok = 0; }
    if (!scene_dack())          { uart_puts("[looptest] Szene Delayed-ACK-Timer FEHLER\n"); ok = 0; }
    if (!scene_wscale())        { uart_puts("[looptest] Szene Window-Scaling FEHLER\n"); ok = 0; }
    if (!scene_wscale_active()) { uart_puts("[looptest] Szene Active-Open/WScale FEHLER\n"); ok = 0; }
    if (!scene_paws())          { uart_puts("[looptest] Szene PAWS FEHLER\n"); ok = 0; }
    if (!scene_fast_rtx())      { uart_puts("[looptest] Szene Fast-Retransmit FEHLER\n"); ok = 0; }
    if (!scene_cwnd())          { uart_puts("[looptest] Szene Congestion-Window FEHLER\n"); ok = 0; }
    if (!scene_keepalive())     { uart_puts("[looptest] Szene Keepalive FEHLER\n"); ok = 0; }
    if (!scene_sws_recv())      { uart_puts("[looptest] Szene Receiver-SWS FEHLER\n"); ok = 0; }

    net_test_reset_time();        /* Uhr-Offset zuruecksetzen VOR echtem Verkehr (DHCP etc.) */
    tcp_unlisten(LO_TEST_PORT);   /* Listener-Slot wieder freigeben (kein Residuum im Stack) */
    return ok;
}
