/*
 * drivers/usb/dwc2.c  --  DWC2 USB-2.0-OTG Host-Treiber (BCM2711)
 *
 * Bringt den Controller in den Host-Modus, schaltet den Root-Port-Strom ein,
 * setzt den Port zurueck und erkennt das angeschlossene Geraet samt Speed.
 * DMA-Modus, reines Polling (keine IRQs). Cache-Pflege fuer DMA-Puffer ist auf
 * echter HW noetig (hier in den spaeteren Transfer-Phasen); in QEMU kohaerent.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "gic.h"
#include "dwc2.h"
#include "usb_hc.h"

#define DWC2_BASE 0xFE980000UL

/* --- Globale Register (Offsets / 4 als Array-Index) --- */
#define GOTGCTL   (0x000 / 4)
#define GAHBCFG   (0x008 / 4)
#define GUSBCFG   (0x00C / 4)
#define GRSTCTL   (0x010 / 4)
#define GINTSTS   (0x014 / 4)
#define GINTMSK   (0x018 / 4)
#define GRXFSIZ   (0x024 / 4)
#define GNPTXFSIZ (0x028 / 4)
#define GSNPSID   (0x040 / 4)
#define HPTXFSIZ  (0x100 / 4)
/* --- Host-Register --- */
#define HCFG      (0x400 / 4)
#define HPRT0     (0x440 / 4)

/* GAHBCFG */
#define GAHBCFG_GINTMSK   (1u << 0)
#define GAHBCFG_DMAEN     (1u << 5)
#define GAHBCFG_HBSTLEN_INCR4 (3u << 1)
/* GUSBCFG */
#define GUSBCFG_FORCEHOST (1u << 29)
#define GUSBCFG_FORCEDEV  (1u << 30)
/* GRSTCTL */
#define GRSTCTL_CSFTRST   (1u << 0)
#define GRSTCTL_RXFFLSH   (1u << 4)
#define GRSTCTL_TXFFLSH   (1u << 5)
#define GRSTCTL_TXFNUM_ALL (0x10u << 6)
#define GRSTCTL_AHBIDLE   (1u << 31)
/* GINTSTS / GINTMSK (Interrupt-Betrieb) */
#define GINTSTS_CURMOD_HOST (1u << 0)
#define GINTSTS_PRTINT      (1u << 24)   /* Host-Port-Interrupt (HPRT-Change) */
#define GINTSTS_HCHINT      (1u << 25)   /* Host-Channels-Interrupt (HAINT) */
/* Host-All-Channels-Interrupt + per-Kanal-Maske */
#define HAINT     (0x414 / 4)
#define HAINTMSK  (0x418 / 4)
#define HCINTMSK(i) ((0x50C + (i) * 0x20) / 4)
/* HPRT0 */
#define HPRT_CONNSTS      (1u << 0)
#define HPRT_CONNDET      (1u << 1)
#define HPRT_ENA          (1u << 2)
#define HPRT_ENCHNG       (1u << 3)
#define HPRT_OCCHNG       (1u << 5)
#define HPRT_RST          (1u << 8)
#define HPRT_PWR          (1u << 12)
#define HPRT_SPD_SHIFT    17
#define HPRT_SPD_MASK     (3u << 17)
/* W1C-Bits in HPRT0: beim Lesen-Aendern-Schreiben ausmaskieren, sonst loescht
 * man sie versehentlich. PRTENA ebenfalls maskieren (HW setzt es, SW darf es
 * mit 1 nicht antasten). */
#define HPRT_WC_MASK      (HPRT_CONNDET | HPRT_ENA | HPRT_ENCHNG | HPRT_OCCHNG)

static volatile uint32_t *const REG = (volatile uint32_t *)DWC2_BASE;

/* Busy-Wait ueber den Generic-Timer-Zaehler (CNTPCT_EL0), unabhaengig vom
 * Scheduler-Tick (DWC2 wird vor/ohne Timer-IRQ betrieben). */
static void udelay(uint32_t us)
{
    uint64_t freq = READ_SYSREG(cntfrq_el0);
    uint64_t start = READ_SYSREG(cntpct_el0);
    uint64_t ticks = (freq / 1000000ULL) * us;
    while ((READ_SYSREG(cntpct_el0) - start) < ticks) {
        /* spin */
    }
}

static void mdelay(uint32_t ms) { udelay(ms * 1000u); }

/* --- Host-Channel-Register (Kanal i, je 0x20 Byte ab 0x500) --- */
#define HCCHAR(i) ((0x500 + (i) * 0x20) / 4)
#define HCSPLT(i) ((0x504 + (i) * 0x20) / 4)
#define HCINT(i)  ((0x508 + (i) * 0x20) / 4)
#define HCTSIZ(i) ((0x510 + (i) * 0x20) / 4)
#define HCDMA(i)  ((0x514 + (i) * 0x20) / 4)

#define HCCHAR_CHENA   (1u << 31)
#define HCCHAR_CHDIS   (1u << 30)
#define HCINT_XFERCOMPL (1u << 0)
#define HCINT_CHHLTD    (1u << 1)
#define HCINT_STALL     (1u << 3)
#define HCINT_NAK       (1u << 4)
#define HCINT_ACK       (1u << 5)
#define HCINT_XACTERR   (1u << 7)
#define HCINT_BBLERR    (1u << 8)
#define HCINT_DATATGLERR (1u << 10)

#define PID_DATA0 0
#define PID_DATA1 2
#define PID_SETUP 3

#define EP_CONTROL 0
#define EP_BULK    2
#define EP_INTR    3

/* DMA-Puffer (cache-line-aligned, identitaetsgemappt -> phys == virt). */
static uint8_t g_setup[8]   __attribute__((aligned(64)));
static uint8_t g_data[512]  __attribute__((aligned(64)));

static usb_speed_t g_speed = USB_SPEED_NONE;
static uint32_t    g_last_pid;     /* naechster Daten-Toggle nach dem letzten Transfer (aus HCTSIZ) */
static int         g_last_xfer;    /* im letzten chan0() uebertragene Bytes (fuer Resume) */

static void cache_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + n;
    for (; a < e; a += 64) {
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void cache_inval(void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + n;
    for (; a < e; a += 64) {
        __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Einen Transfer auf Kanal 0 (Polling). dir: 0=OUT, 1=IN. Liefert uebertragene
 * Bytes (>=0) oder negativen Fehler: -2 STALL (fatal), -3 transienter Transaktions-
 * fehler (XACTERR/DATATGLERR, wiederholbar), -4 NAK (wiederholbar), -1 sonst
 * (Timeout/Babble, fatal). */
static int chan0(uint8_t devaddr, uint8_t epnum, uint8_t dir, uint8_t eptype,
                 uint16_t mps, uint32_t pid, void *buf, int len)
{
    if (mps == 0) {
        return -1;                                 /* defekter Endpoint -> keine Division durch 0 */
    }
    int lowspeed = (g_speed == USB_SPEED_LOW) ? 1 : 0;
    uint32_t pktcnt = (uint32_t)(len + mps - 1) / mps;
    if (pktcnt == 0) {
        pktcnt = 1;
    }
    if (pktcnt > 1023) {
        return -1;                                 /* HCTSIZ.PktCnt ist 10 Bit -> Aufrufer muss stueckeln */
    }

    if (dir == 0 && len > 0) {
        cache_clean(buf, (uint32_t)len);
    }

    REG[HCINT(0)]  = 0xFFFFFFFFu;                  /* alte Status-Bits loeschen */
    REG[HCSPLT(0)] = 0;                            /* keine Split-Transaktionen implementiert:
                                                    * nur direkte FS/LS/HS-Geraete oder FS/LS hinter
                                                    * FS-Hub. FS/LS hinter einem HS-Hub braeuchte
                                                    * SSPLIT/CSPLIT (auf echter HW; hub_find_keyboard
                                                    * weist diesen Fall explizit ab). */
    REG[HCTSIZ(0)] = (pid << 29) | (pktcnt << 19) | ((uint32_t)len & 0x7FFFFu);
    REG[HCDMA(0)]  = (uint32_t)(uintptr_t)buf;
    uint32_t hcchar = ((uint32_t)mps & 0x7FF) | ((uint32_t)epnum << 11) |
                      ((uint32_t)dir << 15) | ((uint32_t)lowspeed << 17) |
                      ((uint32_t)eptype << 18) | (1u << 20) |
                      ((uint32_t)devaddr << 22) | HCCHAR_CHENA;
    REG[HCCHAR(0)] = hcchar;

    uint32_t spin = 2000000, hcint = 0;
    do {
        hcint = REG[HCINT(0)];
    } while (!(hcint & HCINT_CHHLTD) && --spin);
    REG[HCINT(0)] = 0xFFFFFFFFu;

    if (!(hcint & HCINT_CHHLTD)) {
        /* Timeout: Kanal sauber anhalten (CHDIS), sonst bleibt CHENA gesetzt und der
         * naechste chan0() reprogrammiert einen noch aktiven Kanal (laut DWC2-Modell
         * undefiniert -> dauerhaft blockierter Kanal). */
        REG[HCCHAR(0)] = hcchar | HCCHAR_CHDIS;
        uint32_t s2 = 100000;
        while (!(REG[HCINT(0)] & HCINT_CHHLTD) && --s2) { }
        REG[HCINT(0)] = 0xFFFFFFFFu;
        g_last_xfer = 0;
        return -1;
    }
    /* Fortschritt auf JEDEM Halt erfassen (auch NAK/XACTERR), damit chan0_retry einen
     * teil-abgeschlossenen Mehrpaket-Transfer korrekt FORTSETZEN kann (richtiger Daten-
     * Toggle + Offset), statt ihn von vorn mit altem Toggle neu zu starten (Korruption). */
    uint32_t tsz = REG[HCTSIZ(0)];
    g_last_pid = (tsz >> 29) & 0x3u;                  /* naechster Daten-Toggle (HW-aktualisiert) */
    int rem = (int)(tsz & 0x7FFFFu);
    g_last_xfer = (rem <= len) ? (len - rem) : 0;     /* in diesem Versuch uebertragene Bytes */
    if (dir == 1 && g_last_xfer > 0) {
        cache_inval(buf, (uint32_t)g_last_xfer);
    }
    if (hcint & HCINT_XFERCOMPL) {
        return g_last_xfer;                           /* = len (OUT) bzw. len-rem (IN/Short) */
    }
    if (hcint & HCINT_STALL) {
        return -2;                                    /* Endpoint-Stall -> Recovery noetig */
    }
    if (hcint & HCINT_NAK) {
        return -4;                                    /* momentan keine Daten -> wiederholbar */
    }
    if (hcint & (HCINT_XACTERR | HCINT_DATATGLERR)) {
        return -3;                                    /* transienter Bus-/Toggle-Fehler -> wiederholbar */
    }
    return -1;                                        /* Babble u.a. -> fatal */
}

/* chan0 mit Wiederholung gemaess USB, mit RESUME bei Teilfortschritt: NAK
 * (Geraet beschaeftigt; bei Bulk zeitbasiert ~1 s fuer Flash-Schreib-Polling, sonst
 * 50 Versuche), transiente Transaktionsfehler 3x (USB-"3-Strikes"). STALL/Babble/
 * Timeout sind fatal. Liefert die GESAMT uebertragenen Bytes (>=0) oder <0. */
static int chan0_retry(uint8_t devaddr, uint8_t epnum, uint8_t dir, uint8_t eptype,
                       uint16_t mps, uint32_t pid, void *buf, int len)
{
    uint8_t *p = (uint8_t *)buf;
    int done = 0, xact = 3, nak = 50;
    uint64_t freq = READ_SYSREG(cntfrq_el0);
    uint64_t nak_deadline = READ_SYSREG(cntpct_el0) + freq;     /* ~1 s fuer Bulk-NAK */
    for (;;) {
        int r = chan0(devaddr, epnum, dir, eptype, mps, pid, p + done, len - done);
        if (r >= 0) {
            return done + r;                          /* fertig (ggf. Short-Transfer) */
        }
        /* Teilfortschritt uebernehmen und mit korrektem Toggle fortsetzen. */
        done += g_last_xfer;
        pid = g_last_pid;
        if (r == -4) {                                /* NAK */
            if (eptype == EP_BULK) {
                if ((int64_t)(READ_SYSREG(cntpct_el0) - nak_deadline) >= 0) { return -1; }
                udelay(80);
            } else if (--nak <= 0) {
                return -1;
            }
            continue;
        }
        if (r == -3) { if (--xact <= 0) { return -1; } continue; }
        return r;                                     /* -2 STALL / -1 fatal */
    }
}

/* Control-Transfer (SETUP/DATA/STATUS) auf EP0. Liefert Datenlaenge oder -1. */
static int control_msg(uint8_t devaddr, uint16_t ep0mps, uint8_t bmReqType, uint8_t bReq,
                       uint16_t wValue, uint16_t wIndex, void *data, uint16_t wLength)
{
    g_setup[0] = bmReqType; g_setup[1] = bReq;
    g_setup[2] = (uint8_t)wValue;  g_setup[3] = (uint8_t)(wValue >> 8);
    g_setup[4] = (uint8_t)wIndex;  g_setup[5] = (uint8_t)(wIndex >> 8);
    g_setup[6] = (uint8_t)wLength; g_setup[7] = (uint8_t)(wLength >> 8);
    int dirIn = (bmReqType & 0x80) ? 1 : 0;

    if (chan0_retry(devaddr, 0, 0, EP_CONTROL, ep0mps, PID_SETUP, g_setup, 8) < 0) {
        return -1;
    }

    int got = 0;
    if (wLength > 0) {
        if (!dirIn && data) {
            for (uint16_t i = 0; i < wLength; i++) { g_data[i] = ((uint8_t *)data)[i]; }
        }
        /* DATA-Stage mit NAK-/Fehler-Retry. Grenze: bei einem NAK MITTEN in einem
         * mehrpaketigen IN (kleiner ep0mps) wird ab Offset 0 mit DATA1 neu begonnen;
         * fuer einfache HID-Deskriptoren (Geraet ACKt zuegig) unkritisch, ein
         * vollstaendiges Resume-from-HCTSIZ waere fuer groessere Transfers noetig. */
        got = chan0_retry(devaddr, 0, (uint8_t)dirIn, EP_CONTROL, ep0mps, PID_DATA1, g_data, wLength);
        if (got < 0) {
            return -1;
        }
        if (dirIn && data) {
            for (int i = 0; i < got; i++) { ((uint8_t *)data)[i] = g_data[i]; }
        }
    }

    /* STATUS-Stage: entgegengesetzte Richtung, Nulllaenge, DATA1 -- ebenfalls mit
     * Retry (ein Geraet darf die STATUS-Stage legitim NAKen, bis die Operation fertig
     * ist, z.B. nach SET_ADDRESS/SET_CONFIGURATION/PORT_RESET). */
    if (chan0_retry(devaddr, 0, (uint8_t)(dirIn ? 0 : 1), EP_CONTROL, ep0mps, PID_DATA1, g_data, 0) < 0) {
        return -1;
    }
    return got;
}

/* HCD-vtable-Instanz: die dwc2_*-Funktionen erfuellen exakt den usb_hc_ops-Vertrag
 * (Signaturen aus dwc2.h). Wird nach erfolgreichem Init registriert -> usbmsc/usbkbd
 * laufen ueber diese ops. */
static const usb_hc_ops_t dwc2_hc_ops = {
    .name              = "dwc2",
    .bulk              = dwc2_bulk,
    .clear_halt        = dwc2_clear_halt,
    .bot_reset         = dwc2_bot_reset,
    .kbd_irq_getreport = dwc2_kbd_irq_getreport,
    .kbd_poll          = dwc2_kbd_poll,
    .mouse_getreport   = dwc2_kbd_irq_getreport,   /* Maus nutzt denselben HID-Interrupt-IN-Ring */
    .dev_kind          = dwc2_dev_kind,
};

int dwc2_init(void)
{
    if ((REG[GSNPSID] & 0xFFFFF000u) != 0x4F542000u) {
        uart_puts("    [usb] kein DWC2 an 0xFE980000\n");
        return -1;
    }

    /* 1) Auf AHB-Idle warten, dann Core-Soft-Reset. */
    uint32_t spin = 100000;
    while (!(REG[GRSTCTL] & GRSTCTL_AHBIDLE) && --spin) { }
    REG[GRSTCTL] |= GRSTCTL_CSFTRST;
    spin = 100000;
    while ((REG[GRSTCTL] & GRSTCTL_CSFTRST) && --spin) { }
    mdelay(1);

    /* 2) Host-Modus erzwingen (FORCEHOST), Device-Force aus. Laut Datenblatt
     * braucht der Moduswechsel bis ~25 ms. */
    uint32_t usbcfg = REG[GUSBCFG];
    usbcfg &= ~GUSBCFG_FORCEDEV;
    usbcfg |= GUSBCFG_FORCEHOST;
    REG[GUSBCFG] = usbcfg;
    mdelay(50);

    /* 3) DMA-Modus + Burst aktivieren. */
    REG[GAHBCFG] = GAHBCFG_DMAEN | GAHBCFG_HBSTLEN_INCR4;

    /* 4) FIFO-Groessen grob setzen (RX / Non-Periodic-TX / Periodic-TX). Werte in
     * 32-bit-Worten; konservativ fuer Low-/Full-/High-Speed-Control+Interrupt. */
    REG[GRXFSIZ]   = 0x100;                       /* 256 Worte RX-FIFO */
    REG[GNPTXFSIZ] = (0x100u << 16) | 0x100;      /* Start 0x100, Tiefe 0x100 */
    REG[HPTXFSIZ]  = (0x100u << 16) | 0x200;      /* Start 0x200, Tiefe 0x100 */

    /* 5) FIFOs flushen. */
    REG[GRSTCTL] = GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM_ALL;
    spin = 100000;
    while ((REG[GRSTCTL] & GRSTCTL_TXFFLSH) && --spin) { }
    REG[GRSTCTL] = GRSTCTL_RXFFLSH;
    spin = 100000;
    while ((REG[GRSTCTL] & GRSTCTL_RXFFLSH) && --spin) { }

    /* 6) Root-Port mit Strom versorgen. */
    uint32_t hp = REG[HPRT0] & ~HPRT_WC_MASK;
    REG[HPRT0] = hp | HPRT_PWR;
    mdelay(20);

    uart_puts("    [usb] DWC2 Host-Modus aktiv (CURMOD=");
    uart_puts((REG[GINTSTS] & GINTSTS_CURMOD_HOST) ? "host" : "dev");
    uart_puts(")\n");
    usb_hc_register(&dwc2_hc_ops);        /* DWC2 als aktiver Host-Controller fuer usbmsc/usbkbd */
    return 0;
}

usb_speed_t dwc2_port_reset_detect(void)
{
    /* Auf Geraete-Anschluss am Root-Port warten (PRTCONNSTS). */
    uint32_t spin = 200;
    while (!(REG[HPRT0] & HPRT_CONNSTS) && --spin) {
        mdelay(1);
    }
    if (!(REG[HPRT0] & HPRT_CONNSTS)) {
        uart_puts("    [usb] kein Geraet am Root-Port\n");
        return USB_SPEED_NONE;
    }

    mdelay(100);                                  /* Connect-Debounce (TATTDB, USB 2.0 §7.1.7.3) */

    /* Port-Reset: PRTRST setzen, >=50 ms halten, wieder loeschen. */
    uint32_t hp = REG[HPRT0] & ~HPRT_WC_MASK;
    REG[HPRT0] = hp | HPRT_RST;
    mdelay(60);
    hp = REG[HPRT0] & ~HPRT_WC_MASK;
    REG[HPRT0] = hp & ~HPRT_RST;
    mdelay(20);                                   /* Recovery, dann ist PRTENA gesetzt */

    uint32_t prt = REG[HPRT0];
    usb_speed_t spd = (usb_speed_t)((prt & HPRT_SPD_MASK) >> HPRT_SPD_SHIFT);
    g_speed = spd;
    uart_puts("    [usb] Geraet erkannt, Port enabled=");
    uart_puts((prt & HPRT_ENA) ? "ja" : "nein");
    uart_puts(", speed=");
    uart_puts(spd == USB_SPEED_HIGH ? "high\n" :
              spd == USB_SPEED_FULL ? "full\n" :
              spd == USB_SPEED_LOW  ? "low\n"  : "?\n");
    return spd;
}

/* --- Geraete-Zustand nach der Enumeration --- */
static uint8_t  g_addr;        /* zugewiesene USB-Adresse */
static uint16_t g_ep0mps;      /* EP0 max packet size */
static uint8_t  g_hid_ep;      /* Interrupt-IN-Endpoint des HID-Geraets (0 = keiner) */
static uint16_t g_hid_mps;
static uint8_t  g_hid_iface;
static uint8_t  g_hid_proto;   /* bInterfaceProtocol: 1 = Boot-Tastatur, 2 = Boot-Maus */
static uint32_t g_hid_toggle;  /* DATA0/DATA1-Toggle fuer den Interrupt-IN-Endpoint */

/* Massenspeicher (MSC/Bulk-Only) */
static uint8_t  g_dev_kind;    /* 0 = keins, 1 = HID-Tastatur, 2 = MSC-Massenspeicher, 3 = HID-Maus */
static uint8_t  g_msc_in;      /* Bulk-IN-Endpoint (0 = keiner) */
static uint8_t  g_msc_out;     /* Bulk-OUT-Endpoint */
static uint16_t g_msc_mps;
static uint8_t  g_msc_iface;   /* Interface-Nummer des MSC (fuer Bulk-Only-Reset) */
static uint32_t g_msc_tog_in;  /* Bulk-Daten-Toggles je Richtung */
static uint32_t g_msc_tog_out;

static void puthex16(uint16_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    char b[4] = { hx[(v >> 12) & 0xF], hx[(v >> 8) & 0xF], hx[(v >> 4) & 0xF], hx[v & 0xF] };
    uart_putc(b[0]); uart_putc(b[1]); uart_putc(b[2]); uart_putc(b[3]);
}

/* Geraet im Default-Zustand (Adresse 0) ansprechen, Adresse 'newaddr' zuweisen
 * und den vollstaendigen Device-Deskriptor lesen. Liefert bDeviceClass oder -1. */
static int dev_set_address(uint8_t newaddr, uint16_t *mps0_out)
{
    uint16_t mps0 = 8;
    if (control_msg(0, mps0, 0x80, 6, 0x0100, 0, g_data, 8) < 8) {   /* mind. 8 Byte, sonst stale */
        return -1;
    }
    mps0 = g_data[7] ? g_data[7] : 8;
    if (control_msg(0, mps0, 0x00, 5, newaddr, 0, 0, 0) < 0) {
        return -1;
    }
    mdelay(5);
    if (control_msg(newaddr, mps0, 0x80, 6, 0x0100, 0, g_data, 18) < 18) { /* voller Device-Desc */
        return -1;
    }
    uint16_t vid = (uint16_t)(g_data[8] | (g_data[9] << 8));
    uint16_t pid = (uint16_t)(g_data[10] | (g_data[11] << 8));
    uint8_t  cls = g_data[4];
    uart_puts("    [usb] addr="); uart_putdec(newaddr);
    uart_puts(" class=0x"); { static const char hx[]="0123456789ABCDEF"; uart_putc(hx[(cls>>4)&0xF]); uart_putc(hx[cls&0xF]); }
    uart_puts(" VID=0x"); puthex16(vid); uart_puts(" PID=0x"); puthex16(pid);
    uart_puts(" EP0MPS="); uart_putdec(mps0); uart_puts("\n");
    *mps0_out = mps0;
    return cls;
}

#ifdef VISION
/* --- A4.1b: UVC-Kamera-HW-Glue (Vision-Track, nur #ifdef VISION -> der USB-Kern fuer Tastatur/
 * Maus/MSC bleibt byte-identisch). Nutzt die dwc2-Internas (control_msg fuer PROBE/COMMIT,
 * chan0_retry fuer Bulk-IN) + den spec-definierten Klassen-Layer (drivers/usb/uvc.c). Die Kamera
 * wird in config_find_dev enumeriert (Bulk-UVC, da der Stack keine isochronen Transfers hat).
 * NICHT in QEMU testbar (kein UVC-Geraet) -> Bring-up am Pi4 ueber die [uvc]-Serial-Marker. --- */
#include "uvc.h"
static uint8_t  g_uvc_ep, g_uvc_iface, g_uvc_alt;    /* Bulk-IN-Endpoint-Nr + VS-Interface/Alt */
static uint16_t g_uvc_mps;                            /* wMaxPacketSize des Bulk-IN */
static uint32_t g_uvc_toggle;                         /* Bulk-Daten-Toggle (DATA0/DATA1) */
static int      g_uvc_committed;                      /* PROBE/COMMIT erledigt? */
static uint8_t  g_uvc_probe[26] __attribute__((aligned(64)));
#endif

/* Konfiguration lesen, ein unterstuetztes Geraet erkennen (HID-Boot-Tastatur ueber
 * Interrupt-IN ODER Bulk-Only-Massenspeicher ueber Bulk-IN+OUT) und die Konfiguration
 * aktivieren. 0 = Geraet eingerichtet (g_dev_kind gesetzt). */
static int config_find_dev(uint8_t addr, uint16_t mps0)
{
    if (control_msg(addr, mps0, 0x80, 6, 0x0200, 0, g_data, 9) < 9) {
        return -1;
    }
    uint16_t total = (uint16_t)(g_data[2] | (g_data[3] << 8));
    if (total > sizeof(g_data)) {
        total = sizeof(g_data);
    }
    int cgot = control_msg(addr, mps0, 0x80, 6, 0x0200, 0, g_data, total);
    if (cgot < 9) {
        return -1;
    }
    /* Nur die TATSAECHLICH empfangenen Bytes parsen (kein Short-Read -> stale g_data). */
    uint16_t lim = (cgot < (int)total) ? (uint16_t)cgot : total;
    uint8_t cfgval = g_data[5];

    g_hid_ep = 0; g_msc_in = 0; g_msc_out = 0; g_hid_iface = 0; g_msc_iface = 0;
    int is_hid = 0;                                       /* aktuelles Interface HID? */
    int is_msc = 0;                                       /* aktuelles Interface MSC/SCSI/BOT? */
#ifdef VISION
    g_uvc_ep = 0;
    int is_uvc = 0;                                       /* aktuelles Interface UVC-VideoStreaming? */
#endif
    for (uint16_t o = 0; o + 2 <= lim; ) {
        uint8_t blen = g_data[o];
        uint8_t btype = g_data[o + 1];
        if (blen < 2) {
            break;
        }
        if (btype == 0x04 && o + 9 <= lim) {              /* Interface */
            uint8_t icls = g_data[o + 5];                 /* bInterfaceClass */
            uint8_t isub = g_data[o + 6];                 /* bInterfaceSubClass */
            uint8_t iproto = g_data[o + 7];               /* bInterfaceProtocol */
            is_hid = (icls == 3);
            is_msc = (icls == 8 && isub == 0x06 && iproto == 0x50);  /* SCSI + Bulk-Only */
            if (is_hid) {
                g_hid_iface = g_data[o + 2];
                g_hid_proto = iproto;                     /* 1 = Boot-Tastatur, 2 = Boot-Maus */
            } else if (is_msc) {
                g_msc_iface = g_data[o + 2];
            }
#ifdef VISION
            is_uvc = (icls == 0x0E && isub == 0x02);       /* CC_VIDEO / VideoStreaming */
            if (is_uvc) { g_uvc_iface = g_data[o + 2]; g_uvc_alt = g_data[o + 3]; }
#endif
        } else if (btype == 0x05 && o + 7 <= lim) {       /* Endpoint */
            uint8_t  epaddr = g_data[o + 2];
            uint8_t  attr   = g_data[o + 3];
            uint16_t epmps  = (uint16_t)(g_data[o + 4] | (g_data[o + 5] << 8));
            if (is_hid && (attr & 0x03) == 0x03 && (epaddr & 0x80)) {  /* HID Interrupt IN */
                g_hid_ep  = epaddr & 0x0F;
                g_hid_mps = epmps;
            } else if (is_msc && (attr & 0x03) == 0x02) {              /* MSC Bulk IN/OUT */
                if (epaddr & 0x80) { g_msc_in = epaddr & 0x0F; } else { g_msc_out = epaddr & 0x0F; }
                g_msc_mps = epmps;
            }
#ifdef VISION
            else if (is_uvc && (attr & 0x03) == 0x02 && (epaddr & 0x80)) {  /* UVC Bulk IN */
                g_uvc_ep = epaddr & 0x0F; g_uvc_mps = epmps;
            }
#endif
        }
        o = (uint16_t)(o + blen);
    }

    if (control_msg(addr, mps0, 0x00, 9, cfgval, 0, 0, 0) < 0) {       /* SET_CONFIGURATION */
        return -1;
    }
    g_addr = addr;
    g_ep0mps = mps0;

    if (g_hid_ep != 0) {                                  /* --- HID-Boot-Geraet (Tastatur ODER Maus) --- */
        if (g_hid_mps == 0 || g_hid_mps > 64) {
            g_hid_mps = 8;
        }
        g_hid_toggle = PID_DATA0;
        int is_mouse = (g_hid_proto == 2);
        g_dev_kind = is_mouse ? 3 : 1;                    /* 3 = Maus, 1 = Tastatur */
        /* HID-Boot-Protokoll (Fehler unkritisch). Gilt fuer Tastatur wie Maus gleichermassen. */
        control_msg(addr, mps0, 0x21, 0x0B, 0, g_hid_iface, 0, 0);   /* SET_PROTOCOL boot */
        control_msg(addr, mps0, 0x21, 0x0A, 0, g_hid_iface, 0, 0);   /* SET_IDLE 0 */
        uart_puts(is_mouse ? "    [usb] HID-Maus: addr=" : "    [usb] HID-Tastatur: addr=");
        uart_putdec(addr);
        uart_puts(" Interrupt-IN EP"); uart_putdec(g_hid_ep);
        uart_puts(" MPS="); uart_putdec(g_hid_mps); uart_puts("\n");
        return 0;
    }
    if (g_msc_in != 0 && g_msc_out != 0) {                /* --- Bulk-Only-Massenspeicher --- */
        if (g_msc_mps == 0 || g_msc_mps > 512) {
            g_msc_mps = 64;
        }
        g_msc_tog_in = PID_DATA0;
        g_msc_tog_out = PID_DATA0;
        g_dev_kind = 2;
        uart_puts("    [usb] Massenspeicher: addr="); uart_putdec(addr);
        uart_puts(" Bulk-IN EP"); uart_putdec(g_msc_in);
        uart_puts(" Bulk-OUT EP"); uart_putdec(g_msc_out);
        uart_puts(" MPS="); uart_putdec(g_msc_mps); uart_puts("\n");
        return 0;
    }
#ifdef VISION
    if (g_uvc_ep != 0) {                                  /* --- UVC-Kamera (Bulk-Streaming) --- */
        if (g_uvc_mps == 0 || g_uvc_mps > 512) { g_uvc_mps = 512; }
        g_uvc_toggle = PID_DATA0;
        g_uvc_committed = 0;
        g_dev_kind = 4;
        uart_puts("    [usb] UVC-Kamera: addr="); uart_putdec(addr);
        uart_puts(" VS-Interface"); uart_putdec(g_uvc_iface);
        uart_puts(" alt"); uart_putdec(g_uvc_alt);
        uart_puts(" Bulk-IN EP"); uart_putdec(g_uvc_ep);
        uart_puts(" MPS="); uart_putdec(g_uvc_mps); uart_puts("\n");
        return 0;
    }
#endif
    return -1;
}

#ifdef VISION
#define UVC_SET_CUR   0x01
#define UVC_GET_CUR   0x81
#define UVC_VS_PROBE  0x0100   /* VS_PROBE_CONTROL  << 8 */
#define UVC_VS_COMMIT 0x0200   /* VS_COMMIT_CONTROL << 8 */

/* Stream aushandeln: SET_INTERFACE (Bulk-Alt) + PROBE (SET_CUR/GET_CUR) + COMMIT (SET_CUR). */
static int dwc2_uvc_setup(void)
{
    control_msg(g_addr, g_ep0mps, 0x01, 0x0B, g_uvc_alt, g_uvc_iface, 0, 0);        /* SET_INTERFACE */
    uvc_build_probe(g_uvc_probe, (int)sizeof(g_uvc_probe), 1, 1, 333333u);          /* Format 1, Frame 1, ~30fps */
    if (control_msg(g_addr, g_ep0mps, 0x21, UVC_SET_CUR, UVC_VS_PROBE, g_uvc_iface, g_uvc_probe, 26) < 0) {
        uart_puts("    [uvc] PROBE SET_CUR fehlgeschlagen\n"); return -1;
    }
    if (control_msg(g_addr, g_ep0mps, 0xA1, UVC_GET_CUR, UVC_VS_PROBE, g_uvc_iface, g_uvc_probe, 26) < 0) {
        uart_puts("    [uvc] PROBE GET_CUR fehlgeschlagen\n"); return -1;
    }
    uint32_t maxframe = uvc_probe_max_frame_size(g_uvc_probe, 26);
    if (control_msg(g_addr, g_ep0mps, 0x21, UVC_SET_CUR, UVC_VS_COMMIT, g_uvc_iface, g_uvc_probe, 26) < 0) {
        uart_puts("    [uvc] COMMIT fehlgeschlagen\n"); return -1;
    }
    g_uvc_toggle = PID_DATA0;
    g_uvc_committed = 1;
    uart_puts("    [uvc] Stream committed (maxFrame="); uart_putdec(maxframe); uart_puts(" B)\n");
    return 0;
}

/* Ein UVC-Frame nach user_buf (EL0-VA, im Syscall gueltig) greifen: Bulk-IN-Payloads sammeln,
 * je UVC-Payload-Header strippen (uvc.c) und bis zum EOF-Bit zu einem YUYV-Frame fuegen.
 * Rueckgabe: Frame-Bytes / -1. Aufgerufen von vi_cam_grab (kernel/vi_parallel.c). */
int dwc2_uvc_grab(uint64_t user_buf, unsigned long max)
{
    if (g_dev_kind != 4 || g_uvc_ep == 0) { return -1; }
    if (!g_uvc_committed) { if (dwc2_uvc_setup() < 0) { return -1; } }
    uint8_t *ub = (uint8_t *)(uintptr_t)user_buf;
    int pos = 0, done = 0, guard = 0;
    while (!done && guard++ < 8192) {
        int n = chan0_retry(g_addr, g_uvc_ep, 1, EP_BULK, g_uvc_mps, g_uvc_toggle, g_data, g_uvc_mps);
        g_uvc_toggle = g_last_pid;                        /* naechster Toggle aus HCTSIZ */
        if (n < 0) { return -1; }
        if (n == 0) { continue; }                         /* NAK-Leerlauf -> weiter */
        cache_inval(g_data, (uint32_t)n);
        uvc_payload_append(g_data, n, ub, (int)max, &pos, &done);
    }
    return done ? pos : -1;
}
#endif

/* --- Hub-Klassen-Requests --- */
#define HUB_FEAT_PORT_RESET   4
#define HUB_FEAT_PORT_POWER   8
#define HUB_FEAT_C_CONNECTION 16
#define HUB_FEAT_C_RESET      20
#define PORT_STAT_CONNECTION  (1u << 0)
#define PORT_STAT_ENABLE      (1u << 1)
#define PORT_STAT_LOWSPEED    (1u << 9)
#define PORT_STAT_HIGHSPEED   (1u << 10)

static int hub_port_status(uint8_t hub, uint16_t mps, uint8_t port)
{
    if (control_msg(hub, mps, 0xA3, 0, 0, port, g_data, 4) < 4) {   /* mind. wPortStatus+wPortChange */
        return -1;
    }
    return (int)(uint16_t)(g_data[0] | (g_data[1] << 8));
}

/* Den Hub einrichten, den Port mit der Tastatur finden + resetten und das Geraet
 * dahinter (Adresse 2) enumerieren. */
static int hub_find_keyboard(uint8_t hub, uint16_t hubmps)
{
    /* Hub-Deskriptor (Klasse 0x29) -> Portzahl + Power-Good-Zeit. */
    if (control_msg(hub, hubmps, 0xA0, 6, 0x2900, 0, g_data, 8) < 0) {
        uart_puts("    [usb] Hub-Deskriptor fehlgeschlagen\n");
        return -1;
    }
    uint8_t nports = g_data[2];
    if (nports > 15) {
        nports = 15;                              /* USB-2.0: max. 15 Ports; gegen bNbrPorts=255-Hang */
    }
    usb_speed_t hub_speed = g_speed;              /* Hub-Speed (= Root-Port-Speed) vor der Geraete-Speed */
    uint32_t pwrgood_ms = (uint32_t)g_data[5] * 2;
    uart_puts("    [usb] Hub mit "); uart_putdec(nports); uart_puts(" Ports\n");

    for (unsigned p = 1; p <= nports; p++) {
        control_msg(hub, hubmps, 0x23, 3, HUB_FEAT_PORT_POWER, (uint16_t)p, 0, 0);
    }
    mdelay(pwrgood_ms + 50);

    /* Verbundenen Port suchen. */
    int kbport = 0, status = 0;
    for (unsigned p = 1; p <= nports; p++) {
        status = hub_port_status(hub, hubmps, (uint8_t)p);
        if (status >= 0 && (status & PORT_STAT_CONNECTION)) {
            kbport = (int)p;
            break;
        }
    }
    if (!kbport) {
        uart_puts("    [usb] kein Geraet an einem Hub-Port\n");
        return -1;
    }
    control_msg(hub, hubmps, 0x23, 1, HUB_FEAT_C_CONNECTION, (uint16_t)kbport, 0, 0);
    mdelay(100);                                  /* Connect-Debounce (TATTDB) vor dem Port-Reset */

    /* Port zuruecksetzen, auf Enable warten. */
    control_msg(hub, hubmps, 0x23, 3, HUB_FEAT_PORT_RESET, (uint16_t)kbport, 0, 0);
    for (int t = 0; t < 50; t++) {
        mdelay(10);
        status = hub_port_status(hub, hubmps, (uint8_t)kbport);
        if (status >= 0 && (status & PORT_STAT_ENABLE)) {
            break;
        }
    }
    control_msg(hub, hubmps, 0x23, 1, HUB_FEAT_C_RESET, (uint16_t)kbport, 0, 0);
    mdelay(20);

    g_speed = (status & PORT_STAT_LOWSPEED) ? USB_SPEED_LOW :
              (status & PORT_STAT_HIGHSPEED) ? USB_SPEED_HIGH : USB_SPEED_FULL;
    uart_puts("    [usb] Geraet an Hub-Port "); uart_putdec((uint32_t)kbport);
    uart_puts(", speed="); uart_puts(g_speed == USB_SPEED_LOW ? "low\n" :
                                     g_speed == USB_SPEED_HIGH ? "high\n" : "full\n");

    /* Split-Transaktionen sind nicht implementiert: ein FS/LS-Geraet hinter einem
     * High-Speed-Hub braeuchte SSPLIT/CSPLIT. Statt stillem Timeout sauber abweisen.
     * In QEMU laeuft die Kette durchgehend Full-Speed -> dieser Fall tritt dort nicht ein. */
    if (hub_speed == USB_SPEED_HIGH && g_speed != USB_SPEED_HIGH) {
        uart_puts("    [usb] FS/LS-Geraet hinter HS-Hub: Split-Transaktionen noetig (nicht implementiert)\n");
        return -1;
    }

    /* Geraet hinter dem Hub (jetzt Adresse 0) auf Adresse 2 enumerieren. */
    uint16_t kmps = 8;
    if (dev_set_address(2, &kmps) < 0) {
        uart_puts("    [usb] Tastatur-Enumeration fehlgeschlagen\n");
        return -1;
    }
    return config_find_dev(2, kmps);
}

int dwc2_enumerate(void)
{
    uint16_t mps0 = 8;
    int cls = dev_set_address(1, &mps0);
    if (cls < 0) {
        uart_puts("    [usb] Enumeration (Adresse 1) fehlgeschlagen\n");
        return -1;
    }
    if (cls == 0x09) {                 /* Hub -> Tastatur dahinter suchen */
        return hub_find_keyboard(1, mps0);
    }
    return config_find_dev(1, mps0);   /* direkt angeschlossenes Geraet (HID/MSC) */
}

int dwc2_dev_kind(void) { return g_dev_kind; }

int dwc2_bulk(int dir, void *buf, int len)
{
    if (g_dev_kind != 2) {
        return -1;
    }
    uint8_t   ep  = dir ? g_msc_in : g_msc_out;
    uint32_t *tog = dir ? &g_msc_tog_in : &g_msc_tog_out;
    int r = chan0_retry(g_addr, ep, (uint8_t)dir, EP_BULK, g_msc_mps, *tog, buf, len);
    if (r >= 0) {
        *tog = g_last_pid;                /* HW-aktualisierten Daten-Toggle uebernehmen */
    }
    return r;
}

void dwc2_clear_halt(int dir)
{
    if (g_dev_kind != 2) {
        return;
    }
    /* CLEAR_FEATURE(ENDPOINT_HALT) auf den Bulk-Endpoint dieser Richtung + Toggle-Reset. */
    uint8_t ep_addr = dir ? (uint8_t)(g_msc_in | 0x80) : g_msc_out;
    control_msg(g_addr, g_ep0mps, 0x02, 1, 0, ep_addr, 0, 0);
    if (dir) { g_msc_tog_in = PID_DATA0; } else { g_msc_tog_out = PID_DATA0; }
}

void dwc2_bot_reset(void)
{
    if (g_dev_kind != 2) {
        return;
    }
    /* Bulk-Only Mass Storage Reset (Klassen-Request 0x21/0xFF, wIndex=Interface),
     * danach beide Bulk-Endpoints enthalten + beide Toggles auf DATA0. */
    control_msg(g_addr, g_ep0mps, 0x21, 0xFF, 0, g_msc_iface, 0, 0);
    dwc2_clear_halt(1);
    dwc2_clear_halt(0);
}

int dwc2_kbd_poll(uint8_t report[8])
{
    if (g_hid_ep == 0) {
        return -1;
    }
    int r = chan0(g_addr, g_hid_ep, 1 /* IN */, EP_INTR, g_hid_mps, g_hid_toggle, g_data, 8);
    if (r == -4) {
        return 0;                      /* NAK -> momentan keine Eingabe */
    }
    if (r < 0) {
        return r;
    }
    /* Erfolgreicher Transfer: Daten-Toggle umschalten und Report kopieren. */
    g_hid_toggle = (g_hid_toggle == PID_DATA0) ? PID_DATA1 : PID_DATA0;
    int n = r < 8 ? r : 8;
    for (int i = 0; i < n; i++) {
        report[i] = g_data[i];
    }
    for (int i = n; i < 8; i++) {
        report[i] = 0;
    }
    return n;
}

/* ===================== IRQ-getriebene Tastatur + Hot-Plug ===================== */
/*
 * Statt CHHLTD zu pollen, liefert der DWC2 ueber GIC-SPI 105 (empirisch ermittelt)
 * einen Interrupt, sobald die Interrupt-IN-Transaktion haltet. Der Handler kopiert
 * fertige 8-Byte-Reports in einen SPSC-Ringpuffer (Producer: IRQ, Consumer: Shell).
 *
 * IRQ-STURM-VERMEIDUNG: Ein Interrupt-IN-Endpoint NAKt sofort, wenn keine Taste
 * anliegt -> sofortiges Neu-Armen im IRQ wuerde einen Dauer-Interrupt erzeugen.
 * Daher wird der Kanal NICHT im IRQ neu geschaltet, sondern getaktet im 100-Hz-Timer
 * (dwc2_kbd_tick): Completion ist IRQ-getrieben, Re-Arm hoechstens alle 10 ms.
 */
/* DMA-Ziel: volle 64-Byte-Cache-Line (nur die ersten 8 Byte genutzt). EXKLUSIV --
 * teilt sich die Line mit KEINER CPU-geschriebenen Variable, sonst verwirft das
 * 'dc ivac' nach der Completion deren dirty Writes (g_kbd_armed/g_hid_toggle) auf
 * echter, nicht-kohaerenter HW -> Tastatur-Stillstand / DMA-Korruption. */
static uint8_t          g_kbd_buf[64] __attribute__((aligned(64)));
static volatile int     g_kbd_irq   = 0;        /* IRQ-Modus aktiv */
static volatile int     g_kbd_armed = 0;        /* Kanal 0 aktuell scharf (im IRQ-Kontext gepflegt) */
#define KBD_RING 16                              /* Zweierpotenz */
static volatile uint8_t  g_kbd_ring[KBD_RING][8];
static volatile uint32_t g_kbd_head = 0;        /* nur IRQ schreibt */
static volatile uint32_t g_kbd_tail = 0;        /* nur Consumer schreibt */
static volatile uint32_t g_hotplug  = 0;        /* Port-Change-Zaehler (Hot-Plug) */

/* Kanal 0 fuer eine Interrupt-IN-Transaktion (8 Byte) scharf schalten -- ohne Polling. */
static void kbd_arm(void)
{
    int lowspeed = (g_speed == USB_SPEED_LOW) ? 1 : 0;
    uint32_t mps = g_hid_mps ? g_hid_mps : 8;
    uint32_t pktcnt = (8 + mps - 1) / mps;
    if (pktcnt == 0) pktcnt = 1;
    REG[HCINT(0)]  = 0xFFFFFFFFu;
    REG[HCSPLT(0)] = 0;
    REG[HCTSIZ(0)] = (g_hid_toggle << 29) | (pktcnt << 19) | 8u;
    REG[HCDMA(0)]  = (uint32_t)(uintptr_t)g_kbd_buf;
    REG[HCCHAR(0)] = ((uint32_t)mps & 0x7FF) | ((uint32_t)g_hid_ep << 11) |
                     (1u << 15) /* IN */ | ((uint32_t)lowspeed << 17) |
                     ((uint32_t)EP_INTR << 18) | (1u << 20) |
                     ((uint32_t)g_addr << 22) | HCCHAR_CHENA;
}

/* DWC2-Interrupt-Handler (aus c_irq_handler, IRQs maskiert). Behandelt Port-Change
 * (Hot-Plug) und Host-Channel-Halt (Tastatur). */
void dwc2_irq(void)
{
    uint32_t gs = REG[GINTSTS];

    if (gs & GINTSTS_PRTINT) {                   /* Hot-Plug: HPRT-Change quittieren */
        uint32_t hp = REG[HPRT0];
        uint32_t chg = hp & (HPRT_CONNDET | HPRT_ENCHNG | HPRT_OCCHNG);
        REG[HPRT0] = (hp & ~HPRT_WC_MASK) | chg;  /* W1C der Change-Bits, ENA nicht antasten */
        if (chg & HPRT_CONNDET) {
            /* Connect/Disconnect am Root-Port nur ZAEHLEN. NICHT aus dem IRQ drucken:
             * uart_putc spiegelt nach fbcon (nicht-reentranter Cursor/Scroll/ANSI-
             * Zustand) -> Race mit Task-Ausgabe. Ein Task kann den Zaehler via
             * dwc2_hotplug_count() abfragen und das Ereignis selbst melden. */
            g_hotplug++;
        }
    }

    if (gs & GINTSTS_HCHINT) {                   /* Host-Channel-Interrupt */
        uint32_t haint = REG[HAINT];
        if (haint & 1u) {                         /* Kanal 0 = Tastatur */
            uint32_t hcint = REG[HCINT(0)];
            REG[HCINT(0)] = 0xFFFFFFFFu;          /* W1C -> de-assertiert HAINT/HChInt */
            if (hcint & HCINT_CHHLTD) {
                g_kbd_armed = 0;                  /* Re-Arm im Timer-Tick */
                if (hcint & HCINT_XFERCOMPL) {
                    g_hid_toggle = (g_hid_toggle == PID_DATA0) ? PID_DATA1 : PID_DATA0;
                    cache_inval(g_kbd_buf, 8);
                    uint32_t nh = (g_kbd_head + 1u) & (KBD_RING - 1);
                    if (nh != g_kbd_tail) {       /* Ring nicht voll -> Report ablegen */
                        for (int i = 0; i < 8; i++) g_kbd_ring[g_kbd_head][i] = g_kbd_buf[i];
                        g_kbd_head = nh;
                    }
                }
            }
        } else {
            /* Anderer Kanal (sollte im Tastatur-IRQ-Modus nicht vorkommen): quittieren. */
            for (int ch = 1; ch < 8; ch++) {
                if (haint & (1u << ch)) REG[HCINT(ch)] = 0xFFFFFFFFu;
            }
        }
    }

    /* Die Quittierungs-Writes (HPRT0/HCINT) liegen im DWC2-Peripheriebereich, der
     * folgende GICC_EOIR im GIC-Bereich -- zwischen verschiedenen Peripherien ist die
     * Reihenfolge ohne Barriere nicht garantiert (Device-nGnRnE, BCM2711 1.3). dsb sy
     * stellt sicher, dass die Source-Deassertion VOR dem EOI sichtbar ist (sonst ein
     * spurious Re-Trigger). Analog zu timer_irq. */
    dsb_sy();
}

/* Aktiviert den IRQ-Modus fuer das HID-Boot-Geraet (Tastatur ODER Maus, nach der Enumeration).
 * Maus (dev_kind==3) und Tastatur (dev_kind==1) teilen sich denselben Interrupt-IN-Kanal/Ring;
 * die Arm-/Report-Logik ist geraeteunabhaengig (nutzt g_hid_ep/g_hid_mps/g_addr/g_hid_toggle). */
void dwc2_kbd_irq_enable(void)
{
    if ((g_dev_kind != 1 && g_dev_kind != 3) || g_hid_ep == 0) {
        return;
    }
    g_kbd_head = g_kbd_tail = 0;
    g_kbd_armed = 0;
    /* HPRT-Change-Bits + GINTSTS sauber quittieren, dann gezielt unmaskieren. */
    uint32_t hp = REG[HPRT0];
    REG[HPRT0]   = (hp & ~HPRT_WC_MASK) | (hp & (HPRT_CONNDET | HPRT_ENCHNG | HPRT_OCCHNG));
    REG[GINTSTS] = 0xFFFFFFFFu;
    REG[HCINTMSK(0)] = HCINT_CHHLTD;             /* Kanal 0 meldet bei Halt */
    REG[HAINTMSK]    = 1u;                         /* Kanal 0 traegt zu HChInt bei */
    REG[GINTMSK]     = GINTSTS_PRTINT | GINTSTS_HCHINT;  /* nur Port + Host-Channel */
    REG[GAHBCFG]    |= GAHBCFG_GINTMSK;            /* globale IRQ-Leitung an */
    dsb_sy();
    gic_enable_irq(105, 0xA0);                     /* DWC2 = GIC-SPI 105 (empirisch) */
    g_kbd_irq = 1;
}

/* Aktiviert NUR den Port-IRQ (Hot-Plug), ohne Tastatur -- fuer den Fall, dass beim
 * Boot kein Geraet am Root-Port haengt: ein spaeterer Connect feuert dann PrtInt. */
void dwc2_hotplug_enable(void)
{
    uint32_t hp = REG[HPRT0];
    REG[HPRT0]   = (hp & ~HPRT_WC_MASK) | (hp & (HPRT_CONNDET | HPRT_ENCHNG | HPRT_OCCHNG));
    REG[GINTSTS] = 0xFFFFFFFFu;
    REG[GINTMSK]  = GINTSTS_PRTINT;                /* nur Port-Change */
    REG[GAHBCFG] |= GAHBCFG_GINTMSK;
    dsb_sy();
    gic_enable_irq(105, 0xA0);
}

/* 100-Hz-Tick (aus dem Timer-IRQ): Kanal scharf schalten, falls nicht aktiv.
 * Begrenzt das Re-Armen auf 100/s -> kein NAK-Sturm. */
void dwc2_kbd_tick(void)
{
    if (g_kbd_irq && !g_kbd_armed) {
        kbd_arm();
        g_kbd_armed = 1;
    }
}

/* Holt einen fertigen 8-Byte-Report aus dem Ring (SPSC, lock-frei). 8 oder 0. */
int dwc2_kbd_irq_getreport(uint8_t report[8])
{
    if (!g_kbd_irq || g_kbd_tail == g_kbd_head) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        report[i] = g_kbd_ring[g_kbd_tail][i];
    }
    g_kbd_tail = (g_kbd_tail + 1u) & (KBD_RING - 1);
    return 8;
}

/* Anzahl erkannter Port-Connect/Disconnect-Changes (Hot-Plug). */
uint32_t dwc2_hotplug_count(void)
{
    return g_hotplug;
}

/* 1, sobald dwc2_kbd_irq_enable() den HID-Interrupt-IN wirklich scharfgeschaltet hat. Der
 * Selbsttest nutzt das, um den dev_kind-Guard fuer die Maus (dev_kind==3) zu beweisen: bei
 * abgelehnter Maus bliebe g_kbd_irq==0 und die echte Maus produzierte nie ein Event. */
int dwc2_kbd_irq_active(void)
{
    return g_kbd_irq ? 1 : 0;
}
