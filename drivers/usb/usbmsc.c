/*
 * drivers/usb/usbmsc.c  --  USB-Massenspeicher: Bulk-Only-Transport (BOT) + SCSI
 *
 * Setzt auf der HCD-vtable (usb_hc) auf statt direkt auf DWC2 -> laeuft unveraendert
 * ueber DWC2 (raspi) ODER xHCI (virt/VL805). Jedes Kommando: CBW (OUT) -> optionale
 * Datenphase (IN/OUT) -> CSW (IN). SCSI-Subset: INQUIRY, READ CAPACITY(10), READ(10),
 * WRITE(10), REQUEST SENSE. Nur 512-Byte-Sektoren.
 */
#include <stdint.h>
#include "uart.h"
#include "usb_hc.h"
#include "usbmsc.h"

/* NULL-sichere Wrapper um die aktive HCD-vtable (Semantik = frueherer DWC2-Vertrag). */
static int  hc_bulk(int dir, void *buf, int len)
{
    const usb_hc_ops_t *h = usb_hc();
    return (h && h->bulk) ? h->bulk(dir, buf, len) : -1;
}
static void hc_clear_halt(int dir)
{
    const usb_hc_ops_t *h = usb_hc();
    if (h && h->clear_halt) { h->clear_halt(dir); }
}
static void hc_bot_reset(void)
{
    const usb_hc_ops_t *h = usb_hc();
    if (h && h->bot_reset) { h->bot_reset(); }
}
static int  hc_dev_kind(void)
{
    const usb_hc_ops_t *h = usb_hc();
    return (h && h->dev_kind) ? h->dev_kind() : 0;
}

#define CBW_SIG 0x43425355u            /* "USBC" */
#define CSW_SIG 0x53425355u            /* "USBS" */

static uint8_t  g_cbw[31] __attribute__((aligned(64)));
static uint8_t  g_csw[13] __attribute__((aligned(64)));
static uint8_t  g_buf[512] __attribute__((aligned(64)));
static uint8_t  g_save[512] __attribute__((aligned(64)));   /* Sicherung fuer den Schreibtest */
static uint32_t g_tag;
static uint32_t g_sectors;             /* Kapazitaet in 512-Byte-Sektoren */

static void put32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Ein BOT-Kommando ausfuehren. dir: 1 = Daten IN, 0 = Daten OUT (oder keine Daten).
 * 0 = ok (CSW-Status pass, voller Datentransfer, Tag stimmt), <0 = Fehler.
 * Recovery: Endpoint-STALL -> CLEAR_FEATURE(HALT) + CSW lesen; Desync/Phase-Error ->
 * Bulk-Only-Mass-Storage-Reset. */
static int bot_cmd(const uint8_t *cdb, int cdblen, int dir, void *data, uint32_t dlen)
{
    uint32_t tag = ++g_tag;
    for (int i = 0; i < 31; i++) {
        g_cbw[i] = 0;
    }
    put32le(g_cbw + 0, CBW_SIG);
    put32le(g_cbw + 4, tag);
    put32le(g_cbw + 8, dlen);
    g_cbw[12] = dir ? 0x80 : 0x00;     /* bmCBWFlags: Bit7 = Daten-IN */
    g_cbw[13] = 0;                     /* bCBWLUN = 0 */
    g_cbw[14] = (uint8_t)cdblen;       /* bCBWCBLength */
    for (int i = 0; i < cdblen && i < 16; i++) {
        g_cbw[15 + i] = cdb[i];
    }

    if (hc_bulk(0, g_cbw, 31) < 0) {                 /* CBW (OUT) */
        hc_bot_reset();
        return -1;
    }

    int got = 0;
    if (dlen > 0 && data) {                            /* Datenphase */
        int r = hc_bulk(dir, data, (int)dlen);
        if (r == -2) {
            hc_clear_halt(dir);                      /* Endpoint-STALL -> loeschen, CSW dennoch lesen */
        } else if (r < 0) {
            hc_bot_reset();
            return -1;
        } else {
            got = r;
        }
    }

    int cr = hc_bulk(1, g_csw, 13);                  /* CSW (IN) */
    if (cr == -2) {                                    /* CSW-Endpoint-STALL -> loeschen + einmal erneut */
        hc_clear_halt(1);
        cr = hc_bulk(1, g_csw, 13);
    }
    if (cr < 13) {
        hc_bot_reset();
        return -1;
    }

    uint32_t sig     = le32(g_csw + 0);
    uint32_t csw_tag = le32(g_csw + 4);
    uint32_t residue = le32(g_csw + 8);
    uint8_t  status  = g_csw[12];
    if (sig != CSW_SIG || csw_tag != tag) {            /* Signatur/Tag falsch -> Desync */
        hc_bot_reset();
        return -1;
    }
    if (status == 2) {                                 /* Phase Error -> Reset-Recovery */
        hc_bot_reset();
        return -1;
    }
    if (status != 0) {                                 /* Command Failed (Aufrufer kann REQUEST SENSE) */
        return -1;
    }
    if (dlen > 0 && residue != 0) {                    /* Kurz-Transfer -> keine vollstaendigen Daten */
        return -1;
    }
    (void)got;
    return 0;
}

static int scsi_request_sense(void)
{
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    return bot_cmd(cdb, 6, 1, g_buf, 18);
}

/* READ(10)/WRITE(10). count wird in Stuecke <=64 Sektoren zerlegt (haelt die
 * HCTSIZ-PktCnt < 1023 und die Datenphase klein), LBA/count gegen die gemeldete
 * Kapazitaet geprueft. op = 0x28 READ, 0x2A WRITE; dir = 1 IN, 0 OUT. */
#define MSC_CHUNK 64u

static int msc_rw(uint8_t op, int dir, uint32_t lba, uint32_t count, uint8_t *buf)
{
    if (g_sectors == 0 || count == 0 || lba >= g_sectors || count > g_sectors - lba) {
        return -1;
    }
    while (count > 0) {
        uint32_t n = count > MSC_CHUNK ? MSC_CHUNK : count;
        uint8_t cdb[10] = {
            op, 0,
            (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
            0, (uint8_t)(n >> 8), (uint8_t)n, 0
        };
        if (bot_cmd(cdb, 10, dir, buf, n * 512u) < 0) {
            return -1;
        }
        lba   += n;
        buf   += n * 512u;
        count -= n;
    }
    return 0;
}

int usbmsc_read(uint32_t lba, uint32_t count, void *buf)
{
    return msc_rw(0x28, 1, lba, count, (uint8_t *)buf);
}

int usbmsc_write(uint32_t lba, uint32_t count, const void *buf)
{
    return msc_rw(0x2A, 0, lba, count, (uint8_t *)buf);
}

uint32_t usbmsc_sectors(void) { return g_sectors; }

static int msc_init(void)
{
    if (hc_dev_kind() != 2) {
        return -1;
    }
    g_tag = 0;

    /* INQUIRY (36 Byte): Hersteller/Produkt. */
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (bot_cmd(inq, 6, 1, g_buf, 36) < 0) {
        uart_puts("    [msc] INQUIRY fehlgeschlagen\n");
        return -1;
    }
    uart_puts("    [msc] INQUIRY: ");
    for (int i = 8; i < 32; i++) {                     /* Vendor(8..15)+Product(16..31) */
        char c = (char)g_buf[i];
        uart_putc((c >= 32 && c < 127) ? c : ' ');
    }
    uart_puts("\n");

    /* READ CAPACITY(10): letzte LBA + Blockgroesse. Bei Unit-Attention kurz nachfassen. */
    uint8_t rc[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int ok = -1;
    for (int t = 0; t < 4 && ok < 0; t++) {
        ok = bot_cmd(rc, 10, 1, g_buf, 8);
        if (ok < 0) {
            scsi_request_sense();                      /* Sense leeren, dann erneut */
        }
    }
    if (ok < 0) {
        uart_puts("    [msc] READ CAPACITY fehlgeschlagen\n");
        return -1;
    }
    uint32_t last_lba = be32(g_buf);
    uint32_t bsize    = be32(g_buf + 4);
    /* Nur 512-Byte-Bloecke (der restliche Stack adressiert in 512-B-Sektoren); den
     * &gt;2-TiB-Sentinel und unplausible Werte eines (evtl. boesartigen) Geraets abweisen. */
    if (last_lba == 0xFFFFFFFFu || bsize != 512) {
        uart_puts("    [msc] ungueltige Kapazitaet/Blockgroesse (");
        uart_putdec(bsize);
        uart_puts(" B) -- abgelehnt\n");
        g_sectors = 0;
        return -1;
    }
    g_sectors = last_lba + 1;
    uart_puts("    [msc] Kapazitaet: ");
    uart_putdec(g_sectors);
    uart_puts(" Sektoren a 512 B (Blockgroesse ");
    uart_putdec(bsize);
    uart_puts(")\n");
    return 0;
}

void usbmsc_probe(void)
{
    if (msc_init() < 0) {
        return;
    }

    /* Sektor 0 lesen (MBR/Bootsektor) und kurz anzeigen. */
    if (usbmsc_read(0, 1, g_buf) == 0) {
        int mbr_ok = (g_buf[510] == 0x55 && g_buf[511] == 0xAA);
        uart_puts("    [msc] Sektor 0 gelesen (MBR-Signatur ");
        uart_puts(mbr_ok ? "ok)\n" : "fehlt)\n");
    }

    /* Nicht-destruktiver Schreibtest auf dem LETZTEN Sektor: sichern -> Muster
     * schreiben -> zuruecklesen+vergleichen -> Original wiederherstellen. */
    uint32_t s = usbmsc_sectors();
    int wrok = 0;
    if (s > 0 && usbmsc_read(s - 1, 1, g_save) == 0) {
        for (int i = 0; i < 512; i++) {
            g_buf[i] = (uint8_t)(i ^ 0x5A);
        }
        if (usbmsc_write(s - 1, 1, g_buf) == 0 && usbmsc_read(s - 1, 1, g_buf) == 0) {
            wrok = 1;
            for (int i = 0; i < 512; i++) {
                if (g_buf[i] != (uint8_t)(i ^ 0x5A)) { wrok = 0; break; }
            }
        }
        usbmsc_write(s - 1, 1, g_save);     /* Originalinhalt wiederherstellen */
    }
    uart_puts("    [msc] schreibtest: ");
    uart_puts(wrok ? "ok (Sektor geschrieben+zurueckgelesen)\n" : "FEHLGESCHLAGEN\n");
}
