/*
 * drivers/sd/sd.c  --  EMMC2/SDHCI-Treiber fuer BCM2711 (Polling, 1-Bit, PIO)
 *
 * Der microSD-Slot des Pi4 haengt auf echter HW am EMMC2-Controller
 * (0xFE340000). QEMU raspi4b legt die SD-Karte dagegen an den legacy-EMMC
 * (0xFE300000). Beide sind SDHCI-Standard-Hosts mit identischem Registersatz.
 * Wir proben daher beim Init das "Card Inserted"-Bit beider Controller und
 * nutzen den, in dem tatsaechlich eine Karte steckt -> funktioniert in QEMU
 * und auf echter HW.
 *
 * Initialisierung + Lesen einzelner 512-Byte-Bloecke per Polling (kein DMA/IRQ).
 */
#include <stdint.h>
#include "mmio.h"
#include "aarch64.h"
#include "uart.h"
#include "sd.h"

#define EMMC2_BASE   0xFE340000UL   /* echte Pi4-microSD */
#define EMMC_LEGACY  0xFE300000UL   /* QEMU raspi4b-SD */

/* Register-Offsets (SDHCI-Standard). */
#define O_ARG2        0x00
#define O_BLKSIZECNT  0x04
#define O_ARG1        0x08
#define O_CMDTM       0x0C
#define O_RESP0       0x10
#define O_DATA        0x20
#define O_STATUS      0x24
#define O_CONTROL0    0x28
#define O_CONTROL1    0x2C
#define O_INTERRUPT   0x30
#define O_IRPT_MASK   0x34
#define O_IRPT_EN     0x38
#define O_CAPABILITIES 0x40
#define O_SLOTISR_VER 0xFC

/* STATUS (Present State) */
#define SR_CMD_INHIBIT     0x00000001
#define SR_DAT_INHIBIT     0x00000002
#define SR_CARD_INSERTED   0x00010000
/* CONTROL1 */
#define C1_CLK_INTLEN  0x00000001
#define C1_CLK_STABLE  0x00000002
#define C1_CLK_EN      0x00000004
#define C1_TOUNIT_MAX  0x000E0000
#define C1_SRST_HC     0x01000000
#define C1_SRST_CMD    0x02000000
#define C1_SRST_DATA   0x04000000
/* INTERRUPT */
#define INT_CMD_DONE   0x00000001
#define INT_DATA_DONE  0x00000002
#define INT_WRITE_RDY  0x00000010
#define INT_READ_RDY   0x00000020
#define INT_ERROR_MASK 0x017E8000

/* CMDTM-Flags */
#define CMD_RSPNS_136  0x00010000
#define CMD_RSPNS_48   0x00020000
#define CMD_RSPNS_48B  0x00030000
#define CMD_ISDATA     0x00200000
#define TM_DAT_READ    0x00000010
#define CMD_INDEX(n)   ((uint32_t)(n) << 24)

#define CMD_GO_IDLE        (CMD_INDEX(0))
#define CMD_ALL_SEND_CID   (CMD_INDEX(2)  | CMD_RSPNS_136)
#define CMD_SEND_REL_ADDR  (CMD_INDEX(3)  | CMD_RSPNS_48)
#define CMD_CARD_SELECT    (CMD_INDEX(7)  | CMD_RSPNS_48B)
#define CMD_SEND_IF_COND   (CMD_INDEX(8)  | CMD_RSPNS_48)
#define CMD_SET_BLOCKLEN   (CMD_INDEX(16) | CMD_RSPNS_48)
#define CMD_READ_SINGLE    (CMD_INDEX(17) | CMD_RSPNS_48 | CMD_ISDATA | TM_DAT_READ)
#define CMD_WRITE_SINGLE   (CMD_INDEX(24) | CMD_RSPNS_48 | CMD_ISDATA)  /* host->card */
#define CMD_APP_CMD        (CMD_INDEX(55) | CMD_RSPNS_48)
#define ACMD_SEND_OP_COND  (CMD_INDEX(41) | CMD_RSPNS_48)

#define SD_TIMEOUT 1000000

static uintptr_t emmc;             /* gewaehlte Controller-Basis */
static uint32_t  base_clock;       /* Basistakt in Hz (aus CAPABILITIES) */
static uint32_t  sd_rca;
static int       sd_sdhc;
static uint32_t  sd_resp0;

static inline uint32_t rd(uint32_t off)            { return mmio_read32(emmc + off); }
static inline void     wr(uint32_t off, uint32_t v){ mmio_write32(emmc + off, v); }

static void sd_delay(int n)
{
    for (volatile int i = 0; i < n; i++) {
        __asm__ volatile("nop");
    }
}

/* Echte Mikrosekunden-Verzoegerung ueber den Generic Timer (CNTPCT). */
static void sd_udelay(uint32_t us)
{
    uint64_t f     = READ_SYSREG(cntfrq_el0);
    uint64_t start = READ_SYSREG(cntpct_el0);
    uint64_t want  = (f / 1000000u) * us;
    while ((READ_SYSREG(cntpct_el0) - start) < want) {
        __asm__ volatile("nop");
    }
}

/* Soft-Reset der CMD- und/oder DATA-Leitung (nach einem Kommando-/Datenfehler
 * noetig, sonst bleibt CMD_INHIBIT/DAT_INHIBIT gesetzt und der Host verklemmt). */
static void sd_reset_line(uint32_t srst)
{
    wr(O_CONTROL1, rd(O_CONTROL1) | srst);
    for (int t = 0; t < SD_TIMEOUT; t++) {
        if ((rd(O_CONTROL1) & srst) == 0) {
            break;
        }
    }
}

static int sd_wait_status(uint32_t mask)
{
    for (int t = 0; t < SD_TIMEOUT; t++) {
        if ((rd(O_STATUS) & mask) == 0) {
            return 0;
        }
    }
    return -1;
}

static int sd_wait_interrupt(uint32_t mask)
{
    for (int t = 0; t < SD_TIMEOUT; t++) {
        uint32_t r = rd(O_INTERRUPT);
        if (r & INT_ERROR_MASK) {
            wr(O_INTERRUPT, r);
            return -2;
        }
        if (r & mask) {
            wr(O_INTERRUPT, mask);
            return 0;
        }
    }
    return -1;
}

static int sd_command(uint32_t cmd, uint32_t arg)
{
    if (sd_wait_status(SR_CMD_INHIBIT)) {
        sd_reset_line(C1_SRST_CMD | C1_SRST_DATA);
        return -1;
    }
    if (cmd & CMD_ISDATA) {
        if (sd_wait_status(SR_DAT_INHIBIT)) {
            sd_reset_line(C1_SRST_CMD | C1_SRST_DATA);
            return -1;
        }
    }
    wr(O_INTERRUPT, rd(O_INTERRUPT));     /* anstehende Interrupts loeschen */
    wr(O_ARG1, arg);
    wr(O_CMDTM, cmd);

    if (sd_wait_interrupt(INT_CMD_DONE)) {
        /* Fehler/Timeout: CMD-/DAT-Leitung zuruecksetzen, sonst Folge-Hang. */
        sd_reset_line(C1_SRST_CMD | C1_SRST_DATA);
        return -1;
    }
    sd_resp0 = rd(O_RESP0);
    return 0;
}

static int sd_app_command(uint32_t acmd, uint32_t arg)
{
    if (sd_command(CMD_APP_CMD, sd_rca)) {
        return -1;
    }
    return sd_command(acmd, arg);
}

static int sd_set_clock(uint32_t freq)
{
    if (sd_wait_status(SR_CMD_INHIBIT | SR_DAT_INHIBIT)) {
        return -1;
    }
    wr(O_CONTROL1, rd(O_CONTROL1) & ~C1_CLK_EN);
    sd_delay(2000);

    uint32_t div = (base_clock + (2 * freq) - 1) / (2 * freq);
    if (div < 1)     div = 1;
    if (div > 0x3FF) div = 0x3FF;

    uint32_t c1 = rd(O_CONTROL1);
    c1 &= ~0x0000FFE0;                       /* Teiler- + TOUNIT-Felder loeschen */
    c1 |= C1_CLK_INTLEN;
    c1 |= ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);
    c1 |= C1_TOUNIT_MAX;
    wr(O_CONTROL1, c1);
    sd_delay(2000);

    for (int t = 0; ; t++) {
        if (rd(O_CONTROL1) & C1_CLK_STABLE) {
            break;
        }
        if (t >= SD_TIMEOUT) {
            return -1;
        }
    }
    wr(O_CONTROL1, rd(O_CONTROL1) | C1_CLK_EN);
    sd_delay(2000);
    return 0;
}

/* Waehlt die Controller-Basis anhand des Card-Inserted-Bits. */
static int sd_select_base(void)
{
    if (mmio_read32(EMMC2_BASE + O_STATUS) & SR_CARD_INSERTED) {
        emmc = EMMC2_BASE;
    } else if (mmio_read32(EMMC_LEGACY + O_STATUS) & SR_CARD_INSERTED) {
        emmc = EMMC_LEGACY;
    } else {
        return -1;
    }
    uint32_t cap   = rd(O_CAPABILITIES);
    uint32_t mhz   = (cap >> 8) & 0xFF;       /* Basistakt in MHz (SDHCI) */
    base_clock     = mhz ? mhz * 1000000u : 41666666u;
    /* > Zu verifizieren (echte HW): Das CAPABILITIES-Basistaktfeld ist auf dem
     * BCM2711-EMMC2 defekt (Mainline-Kernel-Quirk SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN).
     * Auf echter HW muss der EMMC2-Takt ueber das Mailbox-Property-Interface
     * (GET/SET_CLOCK_RATE, EMMC-Clock-ID) ermittelt/gesetzt werden, sonst stimmt
     * der Teiler nicht. In QEMU ist der konkrete Takt ohne Belang. */
    return 0;
}

int sd_init(void)
{
    sd_rca  = 0;
    sd_sdhc = 0;

    if (sd_select_base() != 0) {
        uart_puts("    [sd] keine Karte gefunden (EMMC2/legacy)\n");
        return -1;
    }
    uart_puts(emmc == EMMC2_BASE ? "    [sd] Controller: EMMC2 (0xFE340000)\n"
                                 : "    [sd] Controller: legacy EMMC (0xFE300000)\n");

    /* Host-Controller-Reset. */
    wr(O_CONTROL0, 0);
    wr(O_CONTROL1, rd(O_CONTROL1) | C1_SRST_HC);
    for (int t = 0; ; t++) {
        if ((rd(O_CONTROL1) & C1_SRST_HC) == 0) {
            break;
        }
        if (t >= SD_TIMEOUT) {
            uart_puts("    [sd] Reset-Timeout\n");
            return -1;
        }
    }

    wr(O_CONTROL0, 0x00000F00);            /* Bus-Power 3.3V */
    sd_udelay(1000);                       /* Power-Ramp abwarten (echte HW) */
    wr(O_IRPT_EN, 0);
    wr(O_IRPT_MASK, 0xFFFFFFFF);

    if (sd_set_clock(400000)) {
        uart_puts("    [sd] Takt-Init fehlgeschlagen\n");
        return -1;
    }
    sd_udelay(200);                        /* >= 74 Takte @400 kHz vor CMD0 */

    if (sd_command(CMD_GO_IDLE, 0)) {
        uart_puts("    [sd] CMD0 fehlgeschlagen\n");
        return -1;
    }
    sd_command(CMD_SEND_IF_COND, 0x000001AA);

    int ready = 0;
    for (int t = 0; t < 1000; t++) {        /* bis ~1 s: reale Karten brauchen das */
        if (sd_app_command(ACMD_SEND_OP_COND, 0x51FF8000) == 0 &&
            (sd_resp0 & 0x80000000)) {
            sd_sdhc = (sd_resp0 & 0x40000000) ? 1 : 0;
            ready = 1;
            break;
        }
        sd_udelay(1000);
    }
    if (!ready) {
        uart_puts("    [sd] ACMD41-Timeout\n");
        return -1;
    }

    if (sd_command(CMD_ALL_SEND_CID, 0)) {
        uart_puts("    [sd] CMD2 fehlgeschlagen\n");
        return -1;
    }
    if (sd_command(CMD_SEND_REL_ADDR, 0)) {
        uart_puts("    [sd] CMD3 fehlgeschlagen\n");
        return -1;
    }
    sd_rca = sd_resp0 & 0xFFFF0000;

    if (sd_set_clock(25000000)) {
        return -1;
    }
    if (sd_command(CMD_CARD_SELECT, sd_rca)) {
        uart_puts("    [sd] CMD7 fehlgeschlagen\n");
        return -1;
    }
    sd_command(CMD_SET_BLOCKLEN, 512);

    uart_puts(sd_sdhc ? "    [sd] Karte bereit (SDHC, Block-Adressierung)\n"
                      : "    [sd] Karte bereit (SDSC, Byte-Adressierung)\n");
    return 0;
}

int sd_read_block(uint64_t lba, void *buf)
{
    if (sd_wait_status(SR_DAT_INHIBIT)) {
        return -1;
    }
    wr(O_BLKSIZECNT, (1u << 16) | 512u);
    uint32_t arg = sd_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);

    if (sd_command(CMD_READ_SINGLE, arg)) {
        return -1;
    }
    if (sd_wait_interrupt(INT_READ_RDY)) {
        sd_reset_line(C1_SRST_CMD | C1_SRST_DATA);
        return -1;
    }

    uint32_t *p = (uint32_t *)buf;
    for (int i = 0; i < 128; i++) {
        p[i] = rd(O_DATA);
    }

    if (sd_wait_interrupt(INT_DATA_DONE)) {
        sd_reset_line(C1_SRST_CMD | C1_SRST_DATA);   /* Datenphasen-Fehler: Leitung loesen */
        return -1;
    }
    return 0;
}

int sd_write_block(uint64_t lba, const void *buf)
{
    if (sd_wait_status(SR_DAT_INHIBIT)) {
        return -1;
    }
    wr(O_BLKSIZECNT, (1u << 16) | 512u);
    uint32_t arg = sd_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);

    if (sd_command(CMD_WRITE_SINGLE, arg)) {
        return -1;
    }
    if (sd_wait_interrupt(INT_WRITE_RDY)) {
        sd_reset_line(C1_SRST_CMD | C1_SRST_DATA);
        return -1;
    }

    const uint32_t *p = (const uint32_t *)buf;
    for (int i = 0; i < 128; i++) {
        wr(O_DATA, p[i]);
    }

    if (sd_wait_interrupt(INT_DATA_DONE)) {
        sd_reset_line(C1_SRST_CMD | C1_SRST_DATA);   /* Datenphasen-Fehler: Leitung loesen */
        return -1;
    }
    return 0;
}
