/*
 * include/net.h  --  Portabler TCP/IP-Stack (hardwareunabhaengig)
 *
 * Schichtung:
 *   Anwendung (UDP-Handler, ICMP-Ping)
 *      |
 *   udp.c / icmp.c
 *      |
 *   ip.c        (IPv4, Routing ueber Gateway, Internet-Checksumme)
 *      |
 *   arp.c       (Adressaufloesung, kleine Cache-Tabelle)
 *      |
 *   ethernet.c  (Frame-Bau/-Zerlegung, Dispatch nach EtherType)
 *      |
 *   netif        (treiber-agnostische Schnittstelle: ->transmit())
 *      |
 *   virtio_net.c (QEMU virt)  /  genet.c (echte Pi-4-HW)
 *
 * Alle Mehr-Byte-Felder im Netz sind big-endian; intern speichern wir IPv4 als
 * host-order uint32_t und konvertieren beim Header-Bau/-Parsen.
 */
#ifndef RPI_RTOS_NET_H
#define RPI_RTOS_NET_H

#include <stdint.h>

/* --- Byte-Order (AArch64 ist little-endian) --- */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x)
{
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* --- Adresstypen --- */
#define ETH_ALEN 6
typedef struct { uint8_t b[ETH_ALEN]; } mac_addr_t;
typedef uint32_t ip4_addr_t;   /* host order */

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                         ((uint32_t)(c) << 8)  | (uint32_t)(d))

/* --- EtherTypes / Protokolle --- */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/* --- Paket-Header (gepackt; Zugriff auf Normal-Memory, unaligned erlaubt) --- */
typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;        /* big-endian */
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype;            /* 1 = Ethernet */
    uint16_t ptype;            /* 0x0800 = IPv4 */
    uint8_t  hlen;             /* 6 */
    uint8_t  plen;             /* 4 */
    uint16_t oper;             /* 1 = request, 2 = reply */
    uint8_t  sha[ETH_ALEN];    /* sender hardware addr */
    uint8_t  spa[4];           /* sender protocol addr */
    uint8_t  tha[ETH_ALEN];    /* target hardware addr */
    uint8_t  tpa[4];           /* target protocol addr */
} arp_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;          /* 0x45 = IPv4, IHL=5 (20 Byte) */
    uint8_t  dscp_ecn;
    uint16_t total_len;        /* big-endian */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;              /* big-endian */
    uint32_t dst;              /* big-endian */
} ip_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;             /* 8 = echo request, 0 = echo reply */
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;              /* UDP-Header + Daten */
    uint16_t checksum;
} udp_hdr_t;

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

/* --- Netzwerk-Interface (treiber-agnostisch) --- */
struct netif;
typedef struct netif {
    mac_addr_t  mac;
    ip4_addr_t  ip;            /* host order */
    ip4_addr_t  netmask;       /* host order */
    ip4_addr_t  gateway;       /* host order */
    /* Treiber-Callback: ein vollstaendiges Ethernet-Frame senden. */
    int   (*transmit)(struct netif *nif, const uint8_t *frame, uint16_t len);
    void   *driver;            /* treiberprivat */
} netif_t;

/* --- Internet-Checksumme (RFC 1071) --- */
uint32_t csum_partial(const void *data, uint32_t len, uint32_t init);
uint16_t csum_fold(uint32_t sum);
static inline uint16_t inet_checksum(const void *data, uint32_t len)
{
    return csum_fold(csum_partial(data, len, 0));
}

/* --- Big-Net-Lock (T1.11): reentrant, IRQ-maskiert; serialisiert den gesamten
 * Stack-Zustand fuer ein kuenftiges nebenlaeufiges Fahrmodell (real-HW GENET-IRQ /
 * zweiter Kern). Wird an JEDEM oeffentlichen Stack-Einstieg genommen; interne Aufrufe
 * (aus dem Eingangspfad heraus verriegelte Sender) reentrieren nur die Tiefe. */
void net_enter(void);
void net_leave(void);

#ifdef RTOS_SELFTEST
/* T1.11-Guardian: max. beobachtete Reentranz-Tiefe (>1 = Lock wird reentrant betreten)
 * und Bilanz-Latch (!=0 = leave ohne passenden enter). */
uint32_t net_lock_maxdepth(void);
uint32_t net_lock_unbalanced(void);
#endif

/* --- Ethernet --- */
void eth_input(netif_t *nif, uint8_t *frame, uint16_t len);
int  eth_send(netif_t *nif, const mac_addr_t *dst, uint16_t ethertype,
              const uint8_t *payload, uint16_t plen);
const mac_addr_t *eth_broadcast(void);

/* --- ARP --- */
void arp_input(netif_t *nif, uint8_t *frame, uint16_t len);
int  arp_lookup(ip4_addr_t ip, mac_addr_t *out);     /* 1 = gefunden */
void arp_request(netif_t *nif, ip4_addr_t target_ip);
void arp_cache_put(ip4_addr_t ip, const mac_addr_t *mac);

/* --- IPv4 --- */
void ip_input(netif_t *nif, uint8_t *payload, uint16_t len);
/* Sendet IPv4-Payload an dst. Loest den Next-Hop (Gateway falls off-subnet) via
 * ARP auf. Liefert plen bei Erfolg, -1 wenn die MAC noch nicht im Cache ist
 * (es wurde ein ARP-Request abgesetzt; der Aufrufer soll erneut versuchen). */
int  ip_send(netif_t *nif, ip4_addr_t dst, uint8_t proto,
             const uint8_t *payload, uint16_t plen);

/* --- ICMP --- */
void icmp_input(netif_t *nif, ip4_addr_t src, ip4_addr_t dst, uint8_t *payload,
                uint16_t len);
int  icmp_send_echo(netif_t *nif, ip4_addr_t dst, uint16_t id, uint16_t seq);

/* --- UDP --- */
typedef void (*udp_handler_t)(netif_t *nif, ip4_addr_t src_ip, uint16_t src_port,
                              const uint8_t *data, uint16_t len);
void udp_input(netif_t *nif, ip4_addr_t src, ip4_addr_t dst, uint8_t *payload,
               uint16_t len);
int  udp_send(netif_t *nif, ip4_addr_t dst, uint16_t dport, uint16_t sport,
              const uint8_t *data, uint16_t len);
int  udp_bind(uint16_t port, udp_handler_t handler);

/* --- TCP (Eingang; übrige API in tcp.h) --- */
void tcp_input(netif_t *nif, ip4_addr_t src, ip4_addr_t dst, uint8_t *payload,
               uint16_t len);

/* --- Zeit (monoton, ms; via ARM Generic Timer CNTPCT_EL0) --- */
uint64_t net_now_ms(void);

/* Test-only: Uhr fuer net_now_ms vorstellen / zuruecksetzen (RTO-Pfade synchron im
 * Loopback-Conformance-Test ausloesen). Nur im Selbsttest-Build vorhanden -- der
 * Produktions-/RC-Build (ohne RTOS_SELFTEST) kennt diese Zeit-Manipulation nicht. */
#ifdef RTOS_SELFTEST
void net_test_advance_ms(uint64_t delta);
void net_test_reset_time(void);
#endif

/* --- Hilfsausgaben (nutzen uart_*) --- */
void net_print_ip(ip4_addr_t ip);
void net_print_mac(const mac_addr_t *m);

#define IP_BROADCAST 0xFFFFFFFFu

/* Maximale Frame-Groesse, die der Stack baut (Ethernet ohne Jumbo). */
#define NET_FRAME_MAX 1600

#endif /* RPI_RTOS_NET_H */
