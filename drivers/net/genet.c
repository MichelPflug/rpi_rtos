/*
 * drivers/net/genet.c  --  BCM2711 GENET v5 Gigabit-Ethernet-Treiber
 *
 * ============================ WICHTIGER HINWEIS ============================
 * QEMU `raspi4b` emuliert GENET NICHT (Zugriff auf 0xFD580000 -> External-Abort,
 * empirisch bestaetigt). Dieser Treiber ist daher REAL-HW-ONLY und in QEMU
 * nicht verifizierbar -- nur per Code-Review gegen den Linux-bcmgenet-Treiber
 * (GENET_V5) und das BCM2711-Datenblatt. Der portable Stack darueber (net/)
 * ist hingegen ueber virtio-net auf QEMU `virt` real verifiziert; GENET muss
 * lediglich dieselbe netif->transmit/Poll-Semantik bereitstellen.
 *
 * Bewusste Vereinfachungen ggue. Linux:
 *   - reines Polling, KEINE Interrupts;
 *   - genau EIN DMA-Ring (Default-Ring 16), keine Prioritaets-Queues;
 *   - kein RX-Status-Block (RBUF_64B_EN aus), kein 2-Byte-Align;
 *   - feste MAC-Fallback-Adresse (echte MAC: VC-Mailbox-Tag 0x00010003 -- folgt
 *     mit dem Mailbox/HDMI-Meilenstein).
 *
 * Offene, auf echter HW zu verifizierende Punkte sind mit  >>HW<<  markiert.
 * ==========================================================================
 */
#include <stdint.h>
#include "genet.h"
#include "net.h"
#include "mmio.h"
#include "aarch64.h"
#include "kmem.h"
#include "uart.h"

#define GENET_BASE 0xFD580000UL

/* --- Registerbloecke (Offsets von GENET_BASE) --- */
#define SYS_OFF   0x0000
#define EXT_OFF   0x0080
#define RBUF_OFF  0x0300
#define UMAC_OFF  0x0800

/* SYS */
#define SYS_REV_CTRL         (SYS_OFF + 0x00)
#define SYS_PORT_CTRL        (SYS_OFF + 0x04)
#define SYS_RBUF_FLUSH_CTRL  (SYS_OFF + 0x08)
#define SYS_TBUF_FLUSH_CTRL  (SYS_OFF + 0x0C)
#define  PORT_MODE_EXT_GPHY  3

/* EXT (RGMII Out-Of-Band-Steuerung) */
#define EXT_RGMII_OOB_CTRL   (EXT_OFF + 0x0C)
#define  RGMII_LINK          (1u << 4)
#define  OOB_DISABLE         (1u << 5)
#define  RGMII_MODE_EN       (1u << 6)
#define  ID_MODE_DIS         (1u << 16)

/* RBUF */
#define RBUF_CTRL            (RBUF_OFF + 0x00)
#define  RBUF_64B_EN         (1u << 0)
#define  RBUF_ALIGN_2B       (1u << 1)
#define RBUF_TBUF_SIZE_CTRL  (RBUF_OFF + 0xB4)

/* UMAC */
#define UMAC_CMD             (UMAC_OFF + 0x008)
#define  CMD_TX_EN           (1u << 0)
#define  CMD_RX_EN           (1u << 1)
#define  CMD_SPEED_SHIFT     2
#define  CMD_SPEED_MASK      3
#define  CMD_SPEED_10        0
#define  CMD_SPEED_100       1
#define  CMD_SPEED_1000      2
#define  CMD_HD_EN           (1u << 10)
#define  CMD_SW_RESET        (1u << 13)
#define UMAC_MAC0            (UMAC_OFF + 0x00C)
#define UMAC_MAC1            (UMAC_OFF + 0x010)
#define UMAC_MAX_FRAME_LEN   (UMAC_OFF + 0x014)
#define UMAC_MIB_CTRL        (UMAC_OFF + 0x580)
#define  MIB_RESET_RX        (1u << 0)
#define  MIB_RESET_RUNT      (1u << 1)
#define  MIB_RESET_TX        (1u << 2)
#define UMAC_MDIO_CMD        (UMAC_OFF + 0x614)
#define  MDIO_START_BUSY     (1u << 29)
#define  MDIO_READ_FAIL      (1u << 28)
#define  MDIO_RD             (2u << 26)
#define  MDIO_WR             (1u << 26)
#define  MDIO_PMD_SHIFT      21
#define  MDIO_REG_SHIFT      16

/* --- DMA-Layout (GENET v5: rdma@0x2000, tdma@0x4000, words_per_bd=3) --- */
#define TOTAL_DESC      256
#define DMA_DESC_WORDS  3
#define DMA_DESC_SIZE   (DMA_DESC_WORDS * 4)        /* 12 Byte */
#define RDMA_DESC_OFF   0x2000
#define TDMA_DESC_OFF   0x4000
#define DMA_RING_SIZE   0x40
#define DESC_INDEX      16                          /* Default-Ring */

/* Ring-/Global-Register liegen HINTER dem Deskriptor-RAM. */
#define RDMA_REG_OFF    (RDMA_DESC_OFF + TOTAL_DESC * DMA_DESC_SIZE)   /* 0x2C00 */
#define TDMA_REG_OFF    (TDMA_DESC_OFF + TOTAL_DESC * DMA_DESC_SIZE)   /* 0x4C00 */
#define RDMA_RING_OFF   (RDMA_REG_OFF + DMA_RING_SIZE * DESC_INDEX)    /* 0x3000 */
#define TDMA_RING_OFF   (TDMA_REG_OFF + DMA_RING_SIZE * DESC_INDEX)    /* 0x5000 */
#define RDMA_GLOBAL_OFF (RDMA_REG_OFF + DMA_RING_SIZE * (DESC_INDEX+1))/* 0x3040 */
#define TDMA_GLOBAL_OFF (TDMA_REG_OFF + DMA_RING_SIZE * (DESC_INDEX+1))/* 0x5040 */

/* Deskriptor-Wort-Offsets */
#define DMA_DESC_LENGTH_STATUS 0x00
#define DMA_DESC_ADDRESS_LO    0x04
#define DMA_DESC_ADDRESS_HI    0x08

/* Length/Status-Bits */
#define DMA_BUFLENGTH_SHIFT 16
#define DMA_BUFLENGTH_MASK  0x0FFF
#define DMA_OWN             (1u << 15)
#define DMA_EOP             (1u << 14)
#define DMA_SOP             (1u << 13)
#define DMA_WRAP            (1u << 12)
#define DMA_TX_APPEND_CRC   (1u << 6)
#define DMA_TX_QTAG_SHIFT   7
#define DMA_TX_QTAG_MASK    0x3F
#define DMA_RX_LEN_SHIFT    16
#define DMA_RX_LEN_MASK     0x0FFF
/* RX-Fehlerbits (untere 16 Bit des Length/Status-Worts) */
#define DMA_RX_OV           (1u << 0)
#define DMA_RX_CRC_ERROR    (1u << 1)
#define DMA_RX_RXER         (1u << 2)
#define DMA_RX_NO           (1u << 3)
#define DMA_RX_LG           (1u << 4)

/* Per-Ring-Registeroffsets (innerhalb des 0x40-Blocks) */
#define RING_WRITE_PTR      0x00
#define RING_PROD_INDEX     0x08   /* RX: HW schreibt; TX: nicht genutzt */
#define RING_CONS_INDEX     0x0C   /* RX: Treiber; TX: nicht genutzt */
#define TDMA_CONS_INDEX     0x08   /* TX: HW schreibt (verbraucht)     */
#define TDMA_PROD_INDEX     0x0C   /* TX: Treiber (eingereicht)        */
#define DMA_RING_BUF_SIZE   0x10
#define DMA_START_ADDR      0x14
#define DMA_START_ADDR_HI   0x18
#define DMA_END_ADDR        0x1C
#define DMA_END_ADDR_HI     0x20
#define DMA_MBUF_DONE_THRESH 0x24
#define RING_READ_PTR       0x2C

/* Globale DMA-Register (innerhalb des Global-Blocks) */
#define DMA_RING_CFG        0x00
#define DMA_CTRL            0x04
#define DMA_STATUS          0x08
#define DMA_SCB_BURST_SIZE  0x0C
#define  DMA_EN             (1u << 0)
#define  DMA_RING_BUF_EN_SHIFT 1
#define  DMA_RING_SIZE_SHIFT 16
#define  DMA_DISABLED       (1u << 0)   /* DMA_STATUS: Engine quiesziert */

/* --- Treiber-Konfiguration --- */
#define GENET_NDESC 64            /* genutzte Deskriptoren je Ring (Teil der 256) */
#define GENET_BUFSZ 2048
#define CACHE_LINE  64

static uint8_t  rx_buf[GENET_NDESC][GENET_BUFSZ] __attribute__((aligned(CACHE_LINE)));
static uint8_t  tx_buf[GENET_NDESC][GENET_BUFSZ] __attribute__((aligned(CACHE_LINE)));
static uint16_t tx_prod;          /* naechster TX-Producer-Index */
static uint16_t rx_cons;          /* unser RX-Consumer-Index     */
static netif_t *g_nif;
static int      g_speed = 1000;   /* aus PHY ausgehandelt */
static int      g_fullduplex = 1;

/* --- Registerzugriff --- */
static inline uint32_t rd(uint32_t off)            { return mmio_read32(GENET_BASE + off); }
static inline void     wr(uint32_t off, uint32_t v){ mmio_write32(GENET_BASE + off, v); }

/* --- Cache-Pflege (RAM ist auf echter HW cacheable) --- */
static void cache_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end = (uintptr_t)p + n;
    for (; a < end; a += CACHE_LINE) {
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void cache_inval(void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end = (uintptr_t)p + n;
    for (; a < end; a += CACHE_LINE) {
        __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* --- MDIO / PHY --- */
/* Zweistufig wie Linux/U-Boot: erst das Kommando schreiben, dann START_BUSY per
 * Read-Modify-Write setzen (auf manchen GENET-Instanzen erforderlich). */
static int mdio_read(int phy, int reg)
{
    wr(UMAC_MDIO_CMD, MDIO_RD | (phy << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT));
    wr(UMAC_MDIO_CMD, rd(UMAC_MDIO_CMD) | MDIO_START_BUSY);
    uint32_t to = 1000000;
    while ((rd(UMAC_MDIO_CMD) & MDIO_START_BUSY) && --to) {
    }
    uint32_t v = rd(UMAC_MDIO_CMD);
    if (!to || (v & MDIO_READ_FAIL)) {
        return -1;
    }
    return (int)(v & 0xFFFF);
}

static void mdio_write(int phy, int reg, uint16_t val)
{
    wr(UMAC_MDIO_CMD, MDIO_WR | (phy << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT) | val);
    wr(UMAC_MDIO_CMD, rd(UMAC_MDIO_CMD) | MDIO_START_BUSY);
    uint32_t to = 1000000;
    while ((rd(UMAC_MDIO_CMD) & MDIO_START_BUSY) && --to) {
    }
}

/* BCM54213PE am Pi 4 sitzt auf MDIO-Adresse 1. */
#define PHY_ADDR 1
#define MII_BMCR 0x00
#define  BMCR_ANENABLE  0x1000
#define  BMCR_ANRESTART 0x0200
#define MII_BMSR 0x01
#define  BMSR_LSTATUS   0x0004
#define  BMSR_ANEGCOMPLETE 0x0020

static int phy_bringup(void)
{
    int id1 = mdio_read(PHY_ADDR, 2);
    int id2 = mdio_read(PHY_ADDR, 3);
    uart_puts("[genet] PHY-ID ");
    uart_puthex((uint32_t)((id1 << 16) | (id2 & 0xFFFF)));
    uart_puts("\n");
    if (id1 < 0) {
        return -1;   /* keine MDIO-Antwort */
    }

    /* Autonegotiation aktivieren + neu starten. */
    mdio_write(PHY_ADDR, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

    /* Auf Link warten ( >>HW<< : Timeout grosszuegig). */
    int up = 0;
    for (uint32_t i = 0; i < 5000000 && !up; i++) {
        int bmsr = mdio_read(PHY_ADDR, MII_BMSR);
        if (bmsr >= 0 && (bmsr & BMSR_LSTATUS)) {
            up = 1;
        }
    }
    if (!up) {
        uart_puts("[genet] WARN: kein Link (fahre dennoch fort)\n");
        return 0;   /* nicht hart scheitern -- evtl. spaeter Link */
    }

    /* Ausgehandelte Geschwindigkeit/Duplex aus dem BCM54xx-Aux-Status
     * (Reg 0x19, Bits [10:8] = HCD-Ergebnis) ableiten. */
    int aux = mdio_read(PHY_ADDR, 0x19);
    if (aux >= 0) {
        switch ((aux >> 8) & 0x7) {
        case 7: g_speed = 1000; g_fullduplex = 1; break;
        case 6: g_speed = 1000; g_fullduplex = 0; break;
        case 5: g_speed = 100;  g_fullduplex = 1; break;
        case 3: g_speed = 100;  g_fullduplex = 0; break;
        case 2: g_speed = 10;   g_fullduplex = 1; break;
        case 1: g_speed = 10;   g_fullduplex = 0; break;
        default: g_speed = 1000; g_fullduplex = 1; break;
        }
    }
    uart_puts("[genet] Link up, Speed=");
    uart_putdec((unsigned)g_speed);
    uart_puts(g_fullduplex ? " full\n" : " half\n");
    return 0;
}

/* --- UMAC-Reset --- */
static void umac_reset(void)
{
    /* RBUF leeren. */
    wr(SYS_RBUF_FLUSH_CTRL, 1);
    dsb_sy();
    for (volatile int i = 0; i < 1000; i++) { }
    wr(SYS_RBUF_FLUSH_CTRL, 0);
    dsb_sy();
    for (volatile int i = 0; i < 1000; i++) { }

    wr(UMAC_CMD, 0);
    wr(UMAC_CMD, CMD_SW_RESET);
    dsb_sy();
    for (volatile int i = 0; i < 1000; i++) { }
    wr(UMAC_CMD, 0);
    dsb_sy();

    /* MIB-Zaehler zuruecksetzen. */
    wr(UMAC_MIB_CTRL, MIB_RESET_RX | MIB_RESET_RUNT | MIB_RESET_TX);
    wr(UMAC_MIB_CTRL, 0);

    wr(UMAC_MAX_FRAME_LEN, GENET_BUFSZ);
}

static void set_mac(const mac_addr_t *m)
{
    wr(UMAC_MAC0, ((uint32_t)m->b[0] << 24) | ((uint32_t)m->b[1] << 16) |
                  ((uint32_t)m->b[2] << 8) | (uint32_t)m->b[3]);
    wr(UMAC_MAC1, ((uint32_t)m->b[4] << 8) | (uint32_t)m->b[5]);
}

/* --- DMA-Aufbau --- */
static void desc_write(uint32_t desc_off, int idx, uint32_t len_status, uint64_t addr)
{
    uint32_t base = desc_off + (uint32_t)idx * DMA_DESC_SIZE;
    wr(base + DMA_DESC_ADDRESS_LO, (uint32_t)addr);
    wr(base + DMA_DESC_ADDRESS_HI, (uint32_t)(addr >> 32));
    wr(base + DMA_DESC_LENGTH_STATUS, len_status);
}

static void dma_init(void)
{
    /* DMA waehrend der Konfiguration aus und auf Quiesce warten (die Firmware
     * koennte GENET-DMA aktiv hinterlassen haben -- sonst Race beim Neuaufsetzen). */
    wr(RDMA_GLOBAL_OFF + DMA_CTRL, 0);
    wr(TDMA_GLOBAL_OFF + DMA_CTRL, 0);
    dsb_sy();
    for (uint32_t to = 1000000; to; to--) {
        if ((rd(RDMA_GLOBAL_OFF + DMA_STATUS) & DMA_DISABLED) &&
            (rd(TDMA_GLOBAL_OFF + DMA_STATUS) & DMA_DISABLED)) {
            break;
        }
    }

    /* SCB-Burst-Groesse setzen (Linux/U-Boot: DMA_MAX_BURST_LENGTH = 0x08). */
    wr(RDMA_GLOBAL_OFF + DMA_SCB_BURST_SIZE, 0x08);
    wr(TDMA_GLOBAL_OFF + DMA_SCB_BURST_SIZE, 0x08);

    /* RX-Deskriptoren mit Puffern bestuecken. */
    for (int i = 0; i < GENET_NDESC; i++) {
        cache_inval(rx_buf[i], GENET_BUFSZ);
        desc_write(RDMA_DESC_OFF, i, 0, (uint64_t)(uintptr_t)rx_buf[i]);
    }
    rx_cons = 0;
    tx_prod = 0;

    /* RX-Ring 16 konfigurieren (Deskriptoren 0..NDESC-1). */
    wr(RDMA_RING_OFF + RING_PROD_INDEX, 0);
    wr(RDMA_RING_OFF + RING_CONS_INDEX, 0);
    wr(RDMA_RING_OFF + DMA_RING_BUF_SIZE,
       ((uint32_t)GENET_NDESC << DMA_RING_SIZE_SHIFT) | GENET_BUFSZ);
    wr(RDMA_RING_OFF + DMA_START_ADDR, 0);
    wr(RDMA_RING_OFF + DMA_START_ADDR_HI, 0);
    wr(RDMA_RING_OFF + RING_READ_PTR, 0);
    wr(RDMA_RING_OFF + RING_WRITE_PTR, 0);
    wr(RDMA_RING_OFF + DMA_END_ADDR,
       (uint32_t)(GENET_NDESC * DMA_DESC_WORDS) - 1);
    wr(RDMA_RING_OFF + DMA_END_ADDR_HI, 0);
    wr(RDMA_RING_OFF + DMA_MBUF_DONE_THRESH, 1);

    /* TX-Ring 16 konfigurieren. */
    wr(TDMA_RING_OFF + TDMA_PROD_INDEX, 0);
    wr(TDMA_RING_OFF + TDMA_CONS_INDEX, 0);
    wr(TDMA_RING_OFF + DMA_RING_BUF_SIZE,
       ((uint32_t)GENET_NDESC << DMA_RING_SIZE_SHIFT) | GENET_BUFSZ);
    wr(TDMA_RING_OFF + DMA_START_ADDR, 0);
    wr(TDMA_RING_OFF + DMA_START_ADDR_HI, 0);
    wr(TDMA_RING_OFF + RING_READ_PTR, 0);
    wr(TDMA_RING_OFF + RING_WRITE_PTR, 0);
    wr(TDMA_RING_OFF + DMA_END_ADDR,
       (uint32_t)(GENET_NDESC * DMA_DESC_WORDS) - 1);
    wr(TDMA_RING_OFF + DMA_END_ADDR_HI, 0);
    wr(TDMA_RING_OFF + DMA_MBUF_DONE_THRESH, 1);

    /* Nur Ring 16 aktivieren. */
    wr(RDMA_GLOBAL_OFF + DMA_RING_CFG, 1u << DESC_INDEX);
    wr(TDMA_GLOBAL_OFF + DMA_RING_CFG, 1u << DESC_INDEX);
    dsb_sy();

    /* DMA einschalten (global EN + Ring-16-Enable). */
    wr(TDMA_GLOBAL_OFF + DMA_CTRL,
       DMA_EN | (1u << (DMA_RING_BUF_EN_SHIFT + DESC_INDEX)));
    wr(RDMA_GLOBAL_OFF + DMA_CTRL,
       DMA_EN | (1u << (DMA_RING_BUF_EN_SHIFT + DESC_INDEX)));
    dsb_sy();
}

/* --- TX --- */
static int genet_transmit(netif_t *nif, const uint8_t *frame, uint16_t len)
{
    (void)nif;
    if (len > GENET_BUFSZ) {
        return -1;
    }
    int slot = tx_prod % GENET_NDESC;
    uint8_t *buf = tx_buf[slot];
    memcpy(buf, frame, len);
    cache_clean(buf, len);

    /* QTAG-Feld wie der Linux/U-Boot-Referenztreiber setzen (Bits [12:7]);
     * der Ring-Wrap wird ueber DMA_END_ADDR geregelt, nicht ueber DMA_WRAP. */
    uint32_t len_status = ((uint32_t)len << DMA_BUFLENGTH_SHIFT) |
                          ((uint32_t)DMA_TX_QTAG_MASK << DMA_TX_QTAG_SHIFT) |
                          DMA_SOP | DMA_EOP | DMA_TX_APPEND_CRC;
    desc_write(TDMA_DESC_OFF, slot, len_status, (uint64_t)(uintptr_t)buf);
    dsb_sy();

    tx_prod++;
    wr(TDMA_RING_OFF + TDMA_PROD_INDEX, tx_prod);

    /* Auf Verbrauch durch die HW warten ( >>HW<< : Polling mit Timeout). */
    uint32_t to = 1000000;
    while (--to) {
        if ((uint16_t)rd(TDMA_RING_OFF + TDMA_CONS_INDEX) == tx_prod) {
            return len;
        }
    }
    uart_puts("[genet] WARN: TX-Timeout\n");
    return -1;   /* konsistent mit der -1/len-Konvention (vgl. virtio_net) */
}

/* --- RX --- */
void genet_poll(void)
{
    uint16_t prod = (uint16_t)rd(RDMA_RING_OFF + RING_PROD_INDEX);

    while (rx_cons != prod) {
        int slot = rx_cons % GENET_NDESC;
        uint32_t base = RDMA_DESC_OFF + (uint32_t)slot * DMA_DESC_SIZE;
        uint32_t ls = rd(base + DMA_DESC_LENGTH_STATUS);
        uint16_t len = (uint16_t)((ls >> DMA_RX_LEN_SHIFT) & DMA_RX_LEN_MASK);
        uint32_t rxerr = ls & (DMA_RX_OV | DMA_RX_CRC_ERROR | DMA_RX_RXER |
                               DMA_RX_NO | DMA_RX_LG);

        /* Nur fehlerfreie, vollstaendige (SOP+EOP) Frames an den Stack geben. */
        if (len > 0 && len <= GENET_BUFSZ && !rxerr &&
            (ls & DMA_SOP) && (ls & DMA_EOP)) {
            cache_inval(rx_buf[slot], GENET_BUFSZ);
            /* GENET haengt die FCS (4 Byte) an; der Stack parst laengenbasiert,
             * Trailing-Bytes sind unschaedlich. */
            eth_input(g_nif, rx_buf[slot], len);
        }

        /* Puffer fuer die naechste DMA wieder bereitstellen. */
        cache_inval(rx_buf[slot], GENET_BUFSZ);
        rx_cons++;
        wr(RDMA_RING_OFF + RING_CONS_INDEX, rx_cons);
    }
}

/* --- Init --- */
int genet_init(netif_t *nif)
{
    g_nif = nif;

    uint32_t rev = rd(SYS_REV_CTRL);
    uart_puts("[genet] SYS_REV_CTRL=");
    uart_puthex(rev);
    uart_puts("\n");

    /* Port auf externes Gigabit-PHY (RGMII). */
    wr(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);

    umac_reset();

    /* MAC setzen. >>HW<< : echte MAC via VC-Mailbox-Tag 0x00010003 (folgt mit
     * dem Mailbox/HDMI-Meilenstein); bis dahin lokal verwaltete Fallback-MAC. */
    static const uint8_t fallback[ETH_ALEN] = { 0xB8, 0x27, 0xEB, 0x00, 0x00, 0x01 };
    if (nif->mac.b[0] == 0 && nif->mac.b[1] == 0 && nif->mac.b[2] == 0 &&
        nif->mac.b[3] == 0 && nif->mac.b[4] == 0 && nif->mac.b[5] == 0) {
        memcpy(nif->mac.b, fallback, ETH_ALEN);
    }
    set_mac(&nif->mac);

    /* RBUF: kein 64-Byte-Status-Block, kein 2-Byte-Align -> Frame ab Offset 0. */
    wr(RBUF_CTRL, 0);

    /* RGMII: Modus an, interne Delays vom PHY, In-Band-Link-Signalisierung nutzen.
     * OOB_DISABLE wird -- wie in Linux/U-Boot -- bewusst NICHT gesetzt (sonst
     * ignoriert der MAC die RGMII-Link-Signalisierung). ( >>HW<< : Delay-Strategie
     * (rgmii-rxid) am Pi 4 bestaetigen.) */
    wr(EXT_RGMII_OOB_CTRL, RGMII_MODE_EN | ID_MODE_DIS | RGMII_LINK);

    if (phy_bringup() != 0) {
        uart_puts("[genet] FEHLER: PHY/MDIO antwortet nicht\n");
        return -1;
    }

    /* Ausgehandelte Geschwindigkeit/Duplex aus dem PHY in UMAC_CMD uebernehmen
     * (volles 2-Bit-Speedfeld mit Maske loeschen, dann setzen). */
    uint32_t spd = (g_speed == 1000) ? CMD_SPEED_1000 :
                   (g_speed == 100)  ? CMD_SPEED_100  : CMD_SPEED_10;
    uint32_t cmd = rd(UMAC_CMD);
    cmd &= ~((uint32_t)CMD_SPEED_MASK << CMD_SPEED_SHIFT);
    cmd |= spd << CMD_SPEED_SHIFT;
    if (g_fullduplex) {
        cmd &= ~CMD_HD_EN;
    } else {
        cmd |= CMD_HD_EN;
    }
    wr(UMAC_CMD, cmd);

    dma_init();

    /* TX/RX im UMAC freigeben. */
    cmd = rd(UMAC_CMD);
    cmd |= CMD_TX_EN | CMD_RX_EN;
    wr(UMAC_CMD, cmd);
    dsb_sy();

    nif->transmit = genet_transmit;

    uart_puts("[genet] init ok, MAC ");
    net_print_mac(&nif->mac);
    uart_puts("\n");
    return 0;
}
