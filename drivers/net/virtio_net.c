/*
 * drivers/net/virtio_net.c  --  virtio-net ueber virtio-mmio (modern, v2)
 *
 * Zielmaschine: QEMU `virt` (virtio-mmio @ 0x0a000000, 32 Slots a 0x200).
 * Es wird der MODERNE Transport (Version 2, VIRTIO_F_VERSION_1) verwendet; in
 * QEMU per `-global virtio-mmio.force-legacy=false` erzwungen.
 *
 * Wichtiger 1.0-Spezifikpunkt: Mit ausgehandeltem VIRTIO_F_VERSION_1 ist der
 * virtio_net_hdr 12 Byte lang (Feld num_buffers immer vorhanden, = 1 ohne
 * MRG_RXBUF) -- NICHT 10.
 *
 * DMA/Cache: Der virt-Harness mappt den gesamten RAM als Normal-Non-Cacheable
 * und identitaetsgemappt (VA == PA). Deshalb sind die Ringe/Buffer ohne
 * Cache-Pflege kohaerent; es genuegen dsb-Barrieren fuer die Reihenfolge.
 */
#include <stdint.h>
#include "virtio_net.h"
#include "net.h"
#include "mmio.h"
#include "aarch64.h"
#include "kmem.h"
#include "uart.h"

/* --- virt-Maschine: virtio-mmio Fenster --- */
#define VIRTIO_MMIO_BASE   0x0A000000UL
#define VIRTIO_MMIO_SLOTS  32
#define VIRTIO_MMIO_STRIDE 0x200

/* --- virtio-mmio Register-Offsets (Version 2) --- */
#define VMMIO_MAGIC            0x000
#define VMMIO_VERSION          0x004
#define VMMIO_DEVICE_ID        0x008
#define VMMIO_DEV_FEATURES     0x010
#define VMMIO_DEV_FEATURES_SEL 0x014
#define VMMIO_DRV_FEATURES     0x020
#define VMMIO_DRV_FEATURES_SEL 0x024
#define VMMIO_QUEUE_SEL        0x030
#define VMMIO_QUEUE_NUM_MAX    0x034
#define VMMIO_QUEUE_NUM        0x038
#define VMMIO_QUEUE_READY      0x044
#define VMMIO_QUEUE_NOTIFY     0x050
#define VMMIO_INT_STATUS       0x060
#define VMMIO_INT_ACK          0x064
#define VMMIO_STATUS           0x070
#define VMMIO_QUEUE_DESC_LOW   0x080
#define VMMIO_QUEUE_DESC_HIGH  0x084
#define VMMIO_QUEUE_DRV_LOW    0x090   /* avail ring ("driver area") */
#define VMMIO_QUEUE_DRV_HIGH   0x094
#define VMMIO_QUEUE_DEV_LOW    0x0A0   /* used ring ("device area") */
#define VMMIO_QUEUE_DEV_HIGH   0x0A4
#define VMMIO_CONFIG           0x100

#define VMMIO_MAGIC_VALUE      0x74726976   /* 'virt' */

/* --- Status-Bits --- */
#define VS_ACKNOWLEDGE 1
#define VS_DRIVER      2
#define VS_DRIVER_OK   4
#define VS_FEATURES_OK 8
#define VS_FAILED      128

/* --- Feature-Bits --- */
#define VIRTIO_NET_F_MAC   (1u << 5)      /* feature-Wort 0 */
#define VIRTIO_F_VERSION_1 (1u << 0)      /* feature-Wort 1 (Bit 32) */

/* --- Virtqueue-Strukturen (split) --- */
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1   /* Polling: keine Used-Buffer-IRQs anfordern */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

#define VNET_QSIZE 16
#define VNET_BUFSZ 2048
#define VNET_HDR_LEN 12          /* virtio_net_hdr bei VIRTIO_F_VERSION_1 */

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VNET_QSIZE];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VNET_QSIZE];
    uint16_t avail_event;
} __attribute__((packed));

/* RX-Queue 0 */
static struct virtq_desc  rx_desc[VNET_QSIZE] __attribute__((aligned(16)));
static struct virtq_avail rx_avail           __attribute__((aligned(16)));
static struct virtq_used  rx_used            __attribute__((aligned(16)));
static uint8_t            rx_buf[VNET_QSIZE][VNET_BUFSZ] __attribute__((aligned(16)));
static uint16_t           rx_last_used;
static uint16_t           rx_qsize;

/* TX-Queue 1 */
static struct virtq_desc  tx_desc[VNET_QSIZE] __attribute__((aligned(16)));
static struct virtq_avail tx_avail           __attribute__((aligned(16)));
static struct virtq_used  tx_used            __attribute__((aligned(16)));
static uint8_t            tx_buf[VNET_QSIZE][VNET_BUFSZ] __attribute__((aligned(16)));
static uint16_t           tx_qsize;

static uintptr_t vbase;
static netif_t  *g_nif;

static int virtio_net_transmit(netif_t *nif, const uint8_t *frame, uint16_t len);

/* --- Registerzugriff --- */
static inline uint32_t vreg(uint32_t off)            { return mmio_read32(vbase + off); }
static inline void     vreg_w(uint32_t off, uint32_t v) { mmio_write32(vbase + off, v); }

static void queue_addr(uint32_t lo, uint32_t hi, void *p)
{
    uint64_t a = (uint64_t)(uintptr_t)p;
    vreg_w(lo, (uint32_t)a);
    vreg_w(hi, (uint32_t)(a >> 32));
}

static int setup_queue(uint32_t q, struct virtq_desc *desc,
                       struct virtq_avail *avail, struct virtq_used *used,
                       uint16_t *qsize_out)
{
    vreg_w(VMMIO_QUEUE_SEL, q);
    if (vreg(VMMIO_QUEUE_READY) != 0) {
        vreg_w(VMMIO_QUEUE_READY, 0);
    }
    uint32_t maxn = vreg(VMMIO_QUEUE_NUM_MAX);
    if (maxn == 0) {
        return -1;
    }
    uint16_t n = (VNET_QSIZE <= maxn) ? VNET_QSIZE : (uint16_t)maxn;

    memset(desc, 0, sizeof(struct virtq_desc) * VNET_QSIZE);
    memset(avail, 0, sizeof(*avail));
    memset(used, 0, sizeof(*used));
    /* Reines Polling: Geraet soll keine Used-Buffer-Interrupts anfordern. */
    avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

    vreg_w(VMMIO_QUEUE_NUM, n);
    queue_addr(VMMIO_QUEUE_DESC_LOW, VMMIO_QUEUE_DESC_HIGH, desc);
    queue_addr(VMMIO_QUEUE_DRV_LOW, VMMIO_QUEUE_DRV_HIGH, avail);
    queue_addr(VMMIO_QUEUE_DEV_LOW, VMMIO_QUEUE_DEV_HIGH, used);
    dsb_sy();
    vreg_w(VMMIO_QUEUE_READY, 1);

    *qsize_out = n;
    return 0;
}

int virtio_net_init(netif_t *nif)
{
    /* 1) virtio-net-Slot suchen. */
    vbase = 0;
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        uintptr_t b = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (mmio_read32(b + VMMIO_MAGIC) != VMMIO_MAGIC_VALUE) {
            continue;
        }
        if (mmio_read32(b + VMMIO_DEVICE_ID) == 1) {     /* 1 = network */
            vbase = b;
            uart_puts("[virtio-net] gefunden @");
            uart_puthex(b);
            uart_puts(" version=");
            uart_putdec(mmio_read32(b + VMMIO_VERSION));
            uart_puts("\n");
            break;
        }
    }
    if (!vbase) {
        uart_puts("[virtio-net] kein Geraet gefunden\n");
        return -1;
    }
    if (vreg(VMMIO_VERSION) != 2) {
        uart_puts("[virtio-net] FEHLER: kein moderner Transport (Version != 2)\n");
        return -1;
    }

    /* 2) Reset + ACK/DRIVER. */
    vreg_w(VMMIO_STATUS, 0);
    dsb_sy();
    vreg_w(VMMIO_STATUS, VS_ACKNOWLEDGE);
    vreg_w(VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER);

    /* 3) Feature-Negotiation. */
    vreg_w(VMMIO_DEV_FEATURES_SEL, 0);
    uint32_t f0 = vreg(VMMIO_DEV_FEATURES);
    vreg_w(VMMIO_DEV_FEATURES_SEL, 1);
    uint32_t f1 = vreg(VMMIO_DEV_FEATURES);

    if (!(f1 & VIRTIO_F_VERSION_1)) {
        uart_puts("[virtio-net] FEHLER: Geraet bietet kein VIRTIO_F_VERSION_1\n");
        vreg_w(VMMIO_STATUS, VS_FAILED);
        return -1;
    }
    int have_mac = (f0 & VIRTIO_NET_F_MAC) != 0;

    uint32_t d0 = have_mac ? VIRTIO_NET_F_MAC : 0;
    uint32_t d1 = VIRTIO_F_VERSION_1;
    vreg_w(VMMIO_DRV_FEATURES_SEL, 0);
    vreg_w(VMMIO_DRV_FEATURES, d0);
    vreg_w(VMMIO_DRV_FEATURES_SEL, 1);
    vreg_w(VMMIO_DRV_FEATURES, d1);

    vreg_w(VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK);
    if (!(vreg(VMMIO_STATUS) & VS_FEATURES_OK)) {
        uart_puts("[virtio-net] FEHLER: FEATURES_OK nicht akzeptiert\n");
        vreg_w(VMMIO_STATUS, VS_FAILED);
        return -1;
    }

    /* 4) Queues einrichten (0 = RX, 1 = TX). */
    if (setup_queue(0, rx_desc, &rx_avail, &rx_used, &rx_qsize) != 0 ||
        setup_queue(1, tx_desc, &tx_avail, &tx_used, &tx_qsize) != 0) {
        uart_puts("[virtio-net] FEHLER: Queue-Setup\n");
        vreg_w(VMMIO_STATUS, VS_FAILED);
        return -1;
    }

    /* 5) MAC lesen (aus Config-Space, falls ausgehandelt). */
    if (have_mac) {
        for (int i = 0; i < ETH_ALEN; i++) {
            nif->mac.b[i] = mmio_read8(vbase + VMMIO_CONFIG + i);
        }
    } else {
        static const uint8_t fallback[ETH_ALEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
        memcpy(nif->mac.b, fallback, ETH_ALEN);
    }

    /* 6) Alle RX-Buffer pre-posten (Deskriptor i besitzt fest rx_buf[i]). */
    for (uint16_t i = 0; i < rx_qsize; i++) {
        rx_desc[i].addr = (uint64_t)(uintptr_t)rx_buf[i];
        rx_desc[i].len = VNET_BUFSZ;
        rx_desc[i].flags = VIRTQ_DESC_F_WRITE;   /* Geraet schreibt hinein */
        rx_desc[i].next = 0;
        rx_avail.ring[i] = i;
    }
    dsb_sy();
    rx_avail.idx = rx_qsize;
    rx_last_used = 0;
    dsb_sy();

    /* 7) DRIVER_OK -> Geraet ist scharf. */
    vreg_w(VMMIO_STATUS, VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);
    dsb_sy();
    vreg_w(VMMIO_QUEUE_NOTIFY, 0);   /* RX-Queue: Buffer bereit */

    nif->transmit = virtio_net_transmit;
    g_nif = nif;

    uart_puts("[virtio-net] MAC ");
    net_print_mac(&nif->mac);
    uart_puts(" RX-Q=");
    uart_putdec(rx_qsize);
    uart_puts(" TX-Q=");
    uart_putdec(tx_qsize);
    uart_puts("\n");
    return 0;
}

/* --- TX --- */
static int virtio_net_transmit(netif_t *nif, const uint8_t *frame, uint16_t len)
{
    (void)nif;
    if (len + VNET_HDR_LEN > VNET_BUFSZ) {
        return -1;
    }

    /* Auf einen freien Slot warten (in-flight < qsize). Bei in-flight < qsize
     * ist der Slot an Position (avail.idx % qsize) garantiert abgearbeitet. */
    dsb_sy();
    uint32_t tx_to = 1000000;
    while ((uint16_t)(tx_avail.idx - tx_used.idx) >= tx_qsize) {
        if (!--tx_to) {
            /* Geraet ruecked used.idx nicht vor (gestallt/fehlerhaft) -> Frame verwerfen statt
             * den Kern dauerhaft zu blockieren. Konsistent mit genet_transmit (Timeout -> -1). */
            uart_puts("[virtio-net] WARN: TX-Timeout -- Frame verworfen\n");
            return -1;
        }
        dsb_sy();   /* used.idx vom Geraet neu lesen */
    }

    uint16_t slot = (uint16_t)(tx_avail.idx % tx_qsize);
    uint8_t *buf = tx_buf[slot];

    memset(buf, 0, VNET_HDR_LEN);                 /* virtio_net_hdr = 0 */
    memcpy(buf + VNET_HDR_LEN, frame, len);

    tx_desc[slot].addr = (uint64_t)(uintptr_t)buf;
    tx_desc[slot].len = (uint32_t)len + VNET_HDR_LEN;
    tx_desc[slot].flags = 0;                       /* nur lesen (Geraet liest) */
    tx_desc[slot].next = 0;

    tx_avail.ring[tx_avail.idx % tx_qsize] = slot;
    dsb_sy();
    tx_avail.idx++;
    dsb_sy();
    vreg_w(VMMIO_QUEUE_NOTIFY, 1);
    return len;
}

/* --- RX --- */
void virtio_net_poll(void)
{
    int reposted = 0;

    dsb_sy();
    /* used.idx EINMAL snapshotten -> die Iterationszahl pro Poll ist auf <= rx_qsize begrenzt
     * (kein Livelock/Starvation des Poll-Kerns bei anhaltendem Wire-Speed-Flood); waehrend der
     * Verarbeitung neu eingetroffene Frames werden beim naechsten Poll behandelt. Wie genet_poll. */
    uint16_t rx_used_snapshot = rx_used.idx;
    dmb_sy();
    while (rx_last_used != rx_used_snapshot) {
        /* Load-Load-Barriere: used.idx wurde beobachtet -- vor dem Lesen des
         * zugehoerigen Ring-Elements (id/len) sicherstellen, dass kein
         * veraltetes Element gelesen wird (Adresse haengt nicht vom idx ab, also
         * keine implizite Datenabhaengigkeit auf schwach geordneter HW). */
        dmb_sy();
        struct virtq_used_elem *e = &rx_used.ring[rx_last_used % rx_qsize];
        uint32_t id = e->id;
        uint32_t length = e->len;

        if (id < rx_qsize && length > VNET_HDR_LEN && length <= VNET_BUFSZ) {
            uint8_t *frame = rx_buf[id] + VNET_HDR_LEN;
            uint16_t flen = (uint16_t)(length - VNET_HDR_LEN);
            eth_input(g_nif, frame, flen);
        }

        rx_last_used++;

        /* Deskriptor erneut bereitstellen. */
        if (id < rx_qsize) {
            rx_avail.ring[rx_avail.idx % rx_qsize] = (uint16_t)id;
            dsb_sy();
            rx_avail.idx++;
            reposted = 1;
        }
    }

    if (reposted) {
        dsb_sy();
        vreg_w(VMMIO_QUEUE_NOTIFY, 0);
    }
}
