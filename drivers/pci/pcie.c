/*
 * drivers/pci/pcie.c  --  BCM2711 PCIe / VL805-Diagnose (nur #ifdef PCIE_PROBE).
 *
 * Ganz #ifdef PCIE_PROBE -> ohne das Flag byte-inert. Liest die PCIe-Root-Complex-Register des
 * BCM2711 (@0xFD500000, im Device-MMIO-Bereich gemappt) + versucht Config-Zugriff auf den VL805
 * (bus 1, dev 0), um festzustellen, ob die Firmware PCIe+VL805 bereits hochgezogen hat. Reine Reads
 * (kein Reset/Schreiben von Steuerregistern) -> ungefaehrlich. Register-Offsets nach brcmstb-pcie.
 */
#ifdef PCIE_PROBE

#include <stdint.h>
#include "mmio.h"
#include "uart.h"
#include "pcie.h"
#include "vfs.h"      /* on-demand Trigger-File + Ergebnis (hdd1) */
#include "sched.h"    /* Poll-Task + task_sleep_ticks */
#include "mailbox.h"  /* VL805-Firmware-(Re)Load via VideoCore (NOTIFY_XHCI_RESET) */
#include "xhci.h"     /* T1.14-xHCI-Treiber (nimmt nur die MMIO-Basis) -> Init + Enumeration am VL805 */
#include "gui_fb.h"   /* gui_fb_move_cursor: Maus-Bewegung sichtbar auf den HDMI-Cursor legen */
#include "usbkbd.h"   /* usbkbd_decode + usbkbd_xhci_push: lokale USB-Tastatur -> Shell-Eingabe */

/* Fault-Fixup (arch/aarch64/exceptions.c): erlaubt, einen externen Abort bei einem PCIe-Zugriff
 * zu TOLERIEREN (Fault-Instruktion ueberspringen) statt zu paniken -> ein noch nicht hochgezogener
 * PCIe-Block killt den Boot nicht mehr, Bringup ist kabellos iterierbar. */
extern volatile int g_fault_fixup;
extern volatile int g_fault_hit;

static int g_vl805_found;   /* von pcie_probe gesetzt: VL805 auf bus1.dev0 erkannt (Config-Space live) */

#define PCIE_BASE            0xFD500000UL
/* --- bekannte BCM2711-PCIe-RC-Register (Offsets ab PCIE_BASE) --- */
#define MISC_MISC_CTRL       0x4008
#define MISC_CPU_2_PCIE_WIN0_LO 0x400c
#define MISC_CPU_2_PCIE_WIN0_HI 0x4010
#define MISC_PCIE_STATUS     0x4068   /* Link-Status: dl_active/phylinkup */
#define MISC_REVISION        0x406c
#define EXT_CFG_DATA         0x8000   /* 4KB-Fenster: Config-Daten des via EXT_CFG_INDEX gewaehlten Geraets */
#define EXT_CFG_INDEX        0x9000   /* (bus<<20)|(dev<<15)|(fn<<12) */
#define RGR1_SW_INIT_1       0x9210

/* Fault-toleranter 32-bit-Read: g_fault_hit vorher nullen; bei externem Abort liefert der Fixup
 * einen (uninitialisierten) Wert + setzt g_fault_hit -> Aufrufer erkennt den Fault. */
static uint32_t rd(uint32_t off)
{
    g_fault_hit = 0;
    uint32_t v = mmio_read32(PCIE_BASE + off);
    return g_fault_hit ? 0u : v;
}

static void dump(const char *name, uint32_t off)
{
    g_fault_hit = 0;
    uint32_t v = mmio_read32(PCIE_BASE + off);
    uart_puts("[pcie] ");
    uart_puts(name);
    uart_puts(" (+");
    uart_puthex(off);
    uart_puts(") = ");
    if (g_fault_hit) { uart_puts("<FAULT>\n"); }
    else { uart_puthex(v); uart_puts("\n"); }
}

/* Config-Wort eines PCIe-Geraets (bus/dev/fn, Register-Offset reg) ueber das EXT_CFG-Fenster lesen. */
static uint32_t cfg_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg)
{
    mmio_write32(PCIE_BASE + EXT_CFG_INDEX, (bus << 20) | (dev << 15) | (fn << 12));
    (void)rd(EXT_CFG_INDEX);   /* posted-write drueckt durch (fault-tolerant) */
    return rd(EXT_CFG_DATA + (reg & 0xFFCu));
}

/* Config-Wort schreiben (analog cfg_read: Geraet via EXT_CFG_INDEX waehlen, dann EXT_CFG_DATA). */
static void cfg_write(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg, uint32_t val)
{
    mmio_write32(PCIE_BASE + EXT_CFG_INDEX, (bus << 20) | (dev << 15) | (fn << 12));
    (void)rd(EXT_CFG_INDEX);
    mmio_write32(PCIE_BASE + EXT_CFG_DATA + (reg & 0xFFCu), val);
}

/* Zusaetzliche brcmstb-PCIe-Register fuer den Bringup. */
#define MISC_RC_BAR2_CONFIG_LO   0x4034
#define MISC_RC_BAR2_CONFIG_HI   0x4038
#define MISC_CPU_2_PCIE_WIN0_BASE_LIMIT 0x4070
#define MISC_CPU_2_PCIE_WIN0_BASE_HI    0x4080
#define MISC_CPU_2_PCIE_WIN0_LIMIT_HI   0x4084
#define MISC_HARD_PCIE_HARD_DEBUG 0x4204
#define RGR1_SW_INIT_1_INIT      (1u << 0)   /* interner Controller-Reset */
#define RGR1_SW_INIT_1_PERST     (1u << 1)   /* PERST# an den Endpunkt (VL805) */

/* Outbound-Fenster (BCM2711, aus Pi4-DT `ranges`): PCIe-Bus 0xF8000000 <- CPU 0x6_00000000, 64 MB.
 * Die CPU-Basis ist in mmu.c (#ifdef PCIE_PROBE) als Device-MMIO gemappt (GiB 24). */
#define VL805_PCIE_ADDR   0xF8000000UL       /* PCIe-Bus-Adresse der VL805-BAR (im Fenster) */
#define PCIE_WIN_CPU      0x600000000ULL      /* CPU-Basis des Outbound-Fensters -> VL805-xHCI-MMIO */

static void udelay_rough(uint32_t loops) { for (volatile uint32_t i = 0; i < loops; i++) { __asm__ volatile("nop"); } }
static uint32_t rr(uint32_t off) { return rd(off); }   /* fault-tolerant */
static void     wr32(uint32_t off, uint32_t v) { mmio_write32(PCIE_BASE + off, v); }

/* VL805-xHCI-Firmware ueber die VideoCore-Mailbox (re)laden: NOTIFY_XHCI_RESET (Tag 0x00030058).
 * Auf dem Pi4 laedt die VC-Firmware die VL805-Firmware; nach unserem eigenen PCIe-Reset muss das
 * nachgeholt werden, sonst antwortet die xHCI-Funktion nicht auf MMIO (Config-Space lebt, Mem=UR).
 * dev_addr = PCIe-Geraeteadresse des VL805. Der Puffer ist entirely #ifdef PCIE_PROBE -> mailbox.c
 * bleibt byte-identisch. 0 = ok. */
static volatile uint32_t s_mbox_xhci[16] __attribute__((aligned(64)));
static int vl805_fw_notify(uint32_t dev_addr)
{
    volatile uint32_t *mb = s_mbox_xhci;
    mb[0] = 8u * 4u;         /* Gesamtgroesse (8 Woerter) */
    mb[1] = 0u;              /* Request */
    mb[2] = 0x00030058u;     /* Tag: NOTIFY_XHCI_RESET */
    mb[3] = 4u;              /* Value-Puffer 4 Byte */
    mb[4] = 0u;              /* Request-Laenge */
    mb[5] = dev_addr;        /* PCIe-Adresse des VL805 (bus/dev/fn-kodiert) */
    mb[6] = 0u;              /* End-Tag */
    mb[7] = 0u;
    return mailbox_property(mb);
}

/* Erster Bringup-Versuch (brcmstb-Sequenz, gekuerzt). Alle Waits BEGRENZT -> haengt den Boot nicht.
 * Outbound-Fenster: CPU 0x6_00000000 -> PCIe-Mem. Gibt 0 zurueck, wenn dl_active kommt, sonst -1. */
static int pcie_bringup(void)
{
    uart_puts("[pcie] bringup: Reset loesen...\n");
    /* 1) Internen Reset deassertieren, PERST (Endpunkt) noch HALTEN. */
    uint32_t sw = rr(RGR1_SW_INIT_1);
    sw &= ~RGR1_SW_INIT_1_INIT;      /* Controller aus dem Reset */
    sw |=  RGR1_SW_INIT_1_PERST;     /* Endpunkt weiter im Reset */
    wr32(RGR1_SW_INIT_1, sw);
    udelay_rough(2000000);

    /* 2) SERDES/PHY aus IDDQ (HARD_DEBUG). Bit 23 = SERDES_IDDQ. */
    uint32_t hd = rr(MISC_HARD_PCIE_HARD_DEBUG);
    hd &= ~(1u << 23);
    wr32(MISC_HARD_PCIE_HARD_DEBUG, hd);
    udelay_rough(4000000);

    /* 3) MISC_CTRL: SCB0-Access + UR-Config-Read + Burst 128. */
    uint32_t ctrl = rr(MISC_MISC_CTRL);
    ctrl |= (1u << 12);   /* SCB_ACCESS_EN */
    ctrl |= (1u << 13);   /* CFG_READ_UR_MODE */
    ctrl &= ~(3u << 20);  /* MAX_BURST_SIZE = 128 (00) */
    wr32(MISC_MISC_CTRL, ctrl);

    /* 4) Inbound-Fenster RC_BAR2 -> ganzer RAM ab 0 (fuer Endpunkt-DMA). Groesse-Code ~4 GiB. */
    wr32(MISC_RC_BAR2_CONFIG_LO, 0x11);       /* base 0, size-Feld (~4GiB, brcmstb-Codierung, iterativ) */
    wr32(MISC_RC_BAR2_CONFIG_HI, 0x0);

    /* 5) Outbound-Fenster WIN0: CPU 0x6_00000000 -> PCIe-Bus 0xF8000000 (VL805-Mem). */
    wr32(MISC_CPU_2_PCIE_WIN0_LO, 0xF8000000u);   /* PCIe-Bus-Basis low */
    wr32(MISC_CPU_2_PCIE_WIN0_HI, 0x0);
    wr32(MISC_CPU_2_PCIE_WIN0_BASE_HI, 0x6);      /* CPU-Basis 0x6_xxxxxxxx */
    wr32(MISC_CPU_2_PCIE_WIN0_LIMIT_HI, 0x6);
    wr32(MISC_CPU_2_PCIE_WIN0_BASE_LIMIT, (0xF800u << 4) | 0xFFF0u); /* base/limit 16-bit-Felder, iterativ */

    /* 6) PERST deassertieren -> VL805 kommt aus dem Reset. */
    udelay_rough(4000000);
    sw = rr(RGR1_SW_INIT_1) & ~RGR1_SW_INIT_1_PERST;
    wr32(RGR1_SW_INIT_1, sw);
    uart_puts("[pcie] bringup: PERST geloest, warte auf Link...\n");

    /* 7) Auf dl_active warten (BEGRENZT). */
    for (int i = 0; i < 200; i++) {
        if (rr(MISC_PCIE_STATUS) & (1u << 4)) {
            uart_puts("[pcie] bringup: LINK UP (dl_active) nach ~");
            uart_putdec((unsigned)i);
            uart_puts(" Runden!\n");
            return 0;
        }
        udelay_rough(500000);
    }
    uart_puts("[pcie] bringup: kein Link (dl_active bleibt 0) -- Sequenz/Register noch nicht passend\n");
    return -1;
}

/* Verifiziert den Abort-Fixup (arch/aarch64): mit aktivem g_fault_fixup muss ein Zugriff auf eine
 * garantiert NICHT gemappte VA einen Data-Abort ausloesen, den der EL1-Sync-Handler abfaengt
 * (Fault-Instruktion ueberspringen + g_fault_hit setzen), OHNE zu paniken -- und ein danach
 * folgender GUELTIGER Zugriff darf NICHT als Fault gelten. 0x60_00000000 (384 GiB) ist in BEIDEN
 * MMU-Karten (QEMU-Grobkarte 0..4 GiB, HW-DTB-Karte: nur RAM-GiBs + GiB3) unmapped -> der
 * HW-kritische Fixup-Pfad wird so schon in QEMU bewiesen, bevor echtes PCIe angefasst wird. */
void pcie_fixup_selftest(void)
{
    volatile uint32_t *bad = (volatile uint32_t *)0x6000000000ULL;   /* 384 GiB: nie gemappt */
    uart_puts("[fixup] selftest: Zugriff auf nicht gemappte VA (erwarte Abfang)...\n");
    g_fault_fixup = 1;
    g_fault_hit   = 0;
    uint32_t v = *bad;                 /* -> Data Abort (EC 0x25); Fixup ueberspringt die Instruktion */
    (void)v;
    int caught = g_fault_hit;
    g_fault_hit = 0;                   /* Gegenprobe: gueltiger Zugriff darf nicht als Fault zaehlen */
    volatile uint32_t cell = 0xA5;
    uint32_t good = cell;              /* gemappter Stack -> kein Fault */
    (void)good;
    int clean = (g_fault_hit == 0);
    g_fault_fixup = 0;
    uart_puts((caught && clean) ? "[fixup] selftest OK: Abort abgefangen, Boot laeuft weiter\n"
                                : "[fixup] selftest FEHLER (Fixup greift nicht wie erwartet)!\n");
}

/* VL805 fuer MMIO sichtbar machen: Outbound-Fenster korrekt kodieren, BAR0 zuweisen, Command
 * Mem-Space+Bus-Master an, dann die xHCI-Capability-Register (CAPLENGTH/HCIVERSION/HCSPARAMS1)
 * ueber das Fenster (CPU 0x6_00000000) lesen -> beweist, dass der Kernel die VL805-xHCI-Register
 * erreicht (Voraussetzung fuer xhci_init). Alles fault-tolerant (g_fault_fixup ist in pcie_probe an).
 * Gibt die CPU-MMIO-Basis zurueck, wenn CAPLENGTH plausibel (>=0x20, <=0x40), sonst 0. */
static uint64_t pcie_vl805_map(void)
{
    /* 0) VL805-Firmware ueber die VC-Mailbox (re)laden -- ZUERST, weil der Notify die RC-Register
     *    (MISC_CTRL) zuruecksetzt. dev_addr = bus1<<20 (VL805 auf bus1.dev0). */
    int fw = vl805_fw_notify(0x00100000u);
    uart_puts("[pcie] VL805 fw-notify(0x100000) -> "); uart_puts(fw == 0 ? "ok\n" : "FEHLER\n");
    udelay_rough(8000000);

    /* 1) MISC_CTRL wieder setzen (Notify hat es genullt): SCB_ACCESS_EN(12) + CFG_READ_UR_MODE(13) +
     *    MAX_BURST=128(00) + ★ SCB0_SIZE[31:27]=0x11 (ilog2(4GiB)-15=17). OHNE SCB0_SIZE ist das
     *    Inbound-Fenster 0 gross -> jeder VL805-DMA auf den Host-RAM aborted -> USBSTS.HSE -> Halt. */
    { uint32_t c = rr(MISC_MISC_CTRL);
      c |= (1u << 12) | (1u << 13);
      c &= ~(3u << 20);
      c &= ~(0x1Fu << 27); c |= (0x11u << 27);      /* SCB0_SIZE = 4 GiB */
      wr32(MISC_MISC_CTRL, c); }
    /* Inbound-Fenster RC_BAR2 -> ganzer 4-GiB-RAM ab 0 (fuer VL805-DMA auf Command-/Event-Ring/Contexts,
     * die im Kernel-BSS ~0x80000+ liegen). LO = base_low(0) | size-Code 0x11 (4 GiB), HI = base_high(0). */
    wr32(MISC_RC_BAR2_CONFIG_LO, 0x00000011u);
    wr32(MISC_RC_BAR2_CONFIG_HI, 0x0);

    /* 2) Outbound-Fenster WIN0: PCIe 0xF8000000 <- CPU 0x6_00000000, 64 MB (brcmstb-Kodierung). */
    wr32(MISC_CPU_2_PCIE_WIN0_LO, (uint32_t)VL805_PCIE_ADDR);
    wr32(MISC_CPU_2_PCIE_WIN0_HI, 0x0);
    wr32(MISC_CPU_2_PCIE_WIN0_BASE_LIMIT, 0x03F00000u);
    wr32(MISC_CPU_2_PCIE_WIN0_BASE_HI, 0x6);
    wr32(MISC_CPU_2_PCIE_WIN0_LIMIT_HI, 0x6);

    /* 3) RC-BRIDGE (bus0.dev0) DIREKT konfigurieren: brcmstb-Root-Bus-Config liegt an PCIE_BASE+off
     *    (NICHT ueber EXT_CFG). Ohne Bus-Nummern + Memory-Window + Command forwardet die Bridge KEINE
     *    Memory-Transaktion nach bus1 -> Read = UR/0xDEADDEAD (genau unser Symptom). */
    uart_puts("[pcie] RC(direkt) id="); uart_puthex(rr(0x00));
    uart_puts(" class="); uart_puthex(rr(0x08));
    uart_puts(" cmd="); uart_puthex(rr(0x04)); uart_puts("\n");
    wr32(0x18, 0x00010100u);                 /* primary=0, secondary=1, subordinate=1 */
    wr32(0x20, 0xF800F800u);                  /* Memory-Base=0xF8000000, Limit=0xF80FFFFF */
    { uint32_t rc = rr(0x04); wr32(0x04, (rc & 0xFFFF0000u) | 0x0006u); }   /* RC: Mem-Space + Bus-Master */

    /* 4) VL805 BAR0 = PCIe-Adresse im Fenster; BAR1 (High) = 0; Command Mem+BusMaster. */
    cfg_write(1, 0, 0, 0x10, (uint32_t)VL805_PCIE_ADDR);
    cfg_write(1, 0, 0, 0x14, 0x0);
    { uint32_t cmd = cfg_read(1, 0, 0, 0x04); cfg_write(1, 0, 0, 0x04, (cmd & 0xFFFF0000u) | 0x0006u); }

    /* --- Readback-Diagnose --- */
    uart_puts("[pcie] WIN0_LO="); uart_puthex(rr(MISC_CPU_2_PCIE_WIN0_LO));
    uart_puts(" BASE_LIMIT="); uart_puthex(rr(MISC_CPU_2_PCIE_WIN0_BASE_LIMIT)); uart_puts("\n");
    uart_puts("[pcie] MISC_CTRL="); uart_puthex(rr(MISC_MISC_CTRL));
    uart_puts(" RC_BAR2="); uart_puthex(rr(MISC_RC_BAR2_CONFIG_LO));
    uart_puts(" STATUS="); uart_puthex(rr(MISC_PCIE_STATUS)); uart_puts("\n");
    uart_puts("[pcie] RC busnr="); uart_puthex(rr(0x18));
    uart_puts(" memwin="); uart_puthex(rr(0x20));
    uart_puts(" cmd="); uart_puthex(rr(0x04)); uart_puts("\n");
    uart_puts("[pcie] VL805 BAR0="); uart_puthex(cfg_read(1, 0, 0, 0x10));
    uart_puts(" cmd="); uart_puthex(cfg_read(1, 0, 0, 0x04)); uart_puts("\n");

    /* Test: Lese an einer CPU-Adresse AUSSERHALB des 64-MB-Fensters (0x6_10000000). Liefert auch die
     * 0xDEADDEAD -> die 0xDEADDEAD ist SoC-Default (Adresse geht gar nicht an PCIe); liefert sie was
     * anderes/Abort -> das Fenster ist speziell und das Problem ist die Bridge/das Ziel. */
    { volatile uint32_t *oow = (volatile uint32_t *)(uintptr_t)0x610000000ULL; g_fault_hit = 0;
      uint32_t v = oow[0];
      uart_puts("[pcie] test@0x6_10000000 (ausser Fenster) = ");
      if (g_fault_hit) { uart_puts("<FAULT>\n"); } else { uart_puthex(v); uart_puts("\n"); } }

    /* 5) xHCI-Capability-Register ueber das Fenster lesen (CPU 0x6_00000000). */
    volatile uint32_t *xhci = (volatile uint32_t *)(uintptr_t)PCIE_WIN_CPU;
    g_fault_hit = 0;
    uint32_t cap0 = xhci[0];          /* [7:0]=CAPLENGTH, [31:16]=HCIVERSION */
    if (g_fault_hit) {
        uart_puts("[pcie] xHCI-MMIO: <FAULT> -- Fenster/BAR noch nicht erreichbar\n");
        return 0;
    }
    uint32_t caplen = cap0 & 0xFFu;
    uart_puts("[pcie] xHCI CAP@0x6_00000000 = "); uart_puthex(cap0);
    uart_puts("  CAPLENGTH="); uart_puthex(caplen);
    uart_puts(" HCIVERSION="); uart_puthex((cap0 >> 16) & 0xFFFFu); uart_puts("\n");
    g_fault_hit = 0;
    uint32_t hcs1 = xhci[1];          /* HCSPARAMS1 */
    uart_puts("[pcie] xHCI HCSPARAMS1="); uart_puthex(hcs1);
    uart_puts(" maxports="); uart_putdec((hcs1 >> 24) & 0xFFu); uart_puts("\n");
    /* CAPLENGTH plausibel? xHCI-Cap-Register sind typ. 0x20..0x40 lang. */
    if (!g_fault_hit && caplen >= 0x20u && caplen <= 0x40u) {
        uart_puts("[pcie] xHCI-MMIO ERREICHBAR (CAPLENGTH plausibel) -> bereit fuer xhci_init\n");
        return PCIE_WIN_CPU;
    }
    uart_puts("[pcie] xHCI-CAPLENGTH unplausibel -- Fenster/BAR-Kodierung noch pruefen\n");
    return 0;
}

/* --- On-demand VL805-xHCI-MMIO-Probe mit Watchdog-Hang-Recovery --------------------------------
 * Die MMIO-Arbeit (pcie_vl805_map) kann HAENGEN, wenn der VL805 nicht antwortet (Firmware/BAR/
 * Fenster). Ein Haenger ist nicht per Fault-Fixup abfangbar. Damit ein Fehlversuch die Fern-
 * steuerung NICHT killt: (1) laeuft nur on-demand (Trigger-File hdd1:XHCIGO.FLG), also NIE beim
 * Boot -> das Netzwerk kommt immer hoch; (2) vor dem riskanten Zugriff wird der PM-Watchdog
 * (12 s) scharfgestellt -> haengt der MMIO-Zugriff, resettet die HW nach 12 s in einen sauberen
 * Boot (Flag wurde ZUVOR geloescht -> kein Retry-Loop, Netzwerk kommt hoch, ich bleibe in Kontrolle).
 * Bei Erfolg wird der Watchdog sofort wieder entwaffnet. */
#define PM_BASE        0xFE100000UL
#define PM_RSTC        (PM_BASE + 0x1CUL)
#define PM_WDOG        (PM_BASE + 0x24UL)
#define PM_PASSWD      0x5A000000u
#define PM_WRCFG_FULL  0x00000020u
#define PM_WRCFG_CLR   0xFFFFFFCFu

/* Watchdog scharf: nach ~sec Sekunden HW-Full-Reset (PM_WDOG-Takt ~65536/s, 20-bit-Feld). */
static void wdog_arm(uint32_t sec)
{
    uint32_t ticks = (sec << 16) & 0x000FFFFFu;      /* sec * 65536, auf 20 bit begrenzt (<=~16 s) */
    mmio_write32(PM_WDOG, PM_PASSWD | ticks);
    uint32_t r = mmio_read32(PM_RSTC);
    r = (r & PM_WRCFG_CLR) | PM_WRCFG_FULL;
    mmio_write32(PM_RSTC, PM_PASSWD | r);
}

/* Watchdog entwaffnen: WRCFG auf 0 (kein Reset) + Zaehler auf Maximum. */
static void wdog_disarm(void)
{
    uint32_t r = mmio_read32(PM_RSTC);
    mmio_write32(PM_RSTC, PM_PASSWD | (r & PM_WRCFG_CLR));   /* WRCFG=00 -> kein Reset */
    mmio_write32(PM_WDOG, PM_PASSWD | 0x000FFFFFu);
}

static int g_nhid;   /* Anzahl eingerichteter HID-Geraete: 1 = nur Tastatur (Shell), 2 = Maus+Tastatur (GUI) */

/* Ein VL805-Bringup-Versuch: MMIO + xHCI-Init + Hub. Dann GUI-Sitzung -> Maus (idx0, Hub-Port 3) +
 * Tastatur (idx1, Hub-Port 1); sonst nur Tastatur (idx0, Port 1). WICHTIG: erst ALLE Hub-Ports
 * reseten (g_slot ist noch der Hub -> dessen EP0), DANN sequenziell enumerieren (geteilter g_ep0).
 * Watchdog-gesichert. 1 = bereit (Watchdog + Fixup danach AUS). */
static int usb_input_bringup(void)
{
    if (!g_vl805_found) { return 0; }
    g_fault_fixup = 1;
    wdog_arm(15);
    int ok = 0;
    uint64_t base = pcie_vl805_map();
    uint16_t vid = 0, pid = 0, dv = 0, dp = 0;
    if (base && xhci_init(base) == 0 && xhci_enumerate(&vid, &pid) == 0 && vid == 0x2109) {
        (void)xhci_hub_scan();
#ifdef GUI_SESSION
        int sp_m = xhci_hub_port_reset(3);   /* Maus (FS) -- beide Ports ZUERST reseten */
        int sp_k = xhci_hub_port_reset(1);   /* Tastatur (LS) */
        if (sp_m > 0 && sp_k > 0 &&
            xhci_hub_enum_device(0, 3, (uint32_t)sp_m, &dv, &dp) == 0 && xhci_hid_setup(0) > 0 &&
            xhci_hub_enum_device(1, 1, (uint32_t)sp_k, &dv, &dp) == 0 && xhci_hid_setup(1) > 0) {
            g_nhid = 2; ok = 1;
        }
#else
        int sp_k = xhci_hub_port_reset(1);   /* nur Tastatur */
        if (sp_k > 0 && xhci_hub_enum_device(0, 1, (uint32_t)sp_k, &dv, &dp) == 0 && xhci_hid_setup(0) > 0) {
            g_nhid = 1; ok = 1;
        }
#endif
    }
    wdog_disarm();
    g_fault_fixup = 0;
    return ok;
}

/* --- xHCI-Maus -> gui_input (GUI-Sitzung) --------------------------------------------------------
 * Der Maus-Poll-Task pflegt Cursor-Position + Buttons; gui_input_tick (Timer-IRQ, Kern 0, strikt
 * single-producer) liest sie via xhci_mouse_get und reiht GUI_EV_MOUSE ein + bewegt den Cursor. */
static volatile int g_xm_x = 960, g_xm_y = 540, g_xm_btn, g_xm_dirty;
int xhci_mouse_get(int *x, int *y, int *btn)
{
    if (!g_xm_dirty) { return 0; }
    if (x)   { *x = g_xm_x; }
    if (y)   { *y = g_xm_y; }
    if (btn) { *btn = g_xm_btn; }
    g_xm_dirty = 0;
    return 1;
}

/* Dauerhafter kombinierter Input-Poll: beide Geraete primen, dann Reports per Slot-Demux holen +
 * routen. GUI: idx0=Maus -> Cursor/Buttons (g_xm), idx1=Tastatur -> Konsole (gui_input). Shell:
 * idx0=Tastatur -> Konsole. Nach jedem Report das gemeldete Geraet neu primen. Kehrt NIE zurueck. */
static void usb_input_loop(void)
{
    vfs_write_file("hdd1:XHCIRES.TXT", "input aktiv\n", 12);
    uart_puts("[usb] EINGABE AKTIV (Maus + Tastatur), Geraete="); uart_putdec((unsigned)g_nhid); uart_puts("\n");
    for (int i = 0; i < g_nhid; i++) { xhci_hid_prime(i); }
    static uint8_t rep[64] __attribute__((aligned(64)));
    for (;;) {
        int idx = xhci_hid_wait(g_nhid, rep, sizeof(rep), 100);
        if (idx < 0) { continue; }                       /* Timeout -> beide bleiben geprimt */
#ifdef GUI_SESSION
        if (idx == 0) {                                  /* Maus: Deltas akkumulieren, auf FB klemmen */
            int dx = (int)(int8_t)rep[1], dy = (int)(int8_t)rep[2];
            int nx = g_xm_x + dx, ny = g_xm_y + dy;
            if (nx < 0) { nx = 0; } if (nx > 1919) { nx = 1919; }
            if (ny < 0) { ny = 0; } if (ny > 1079) { ny = 1079; }
            g_xm_x = nx; g_xm_y = ny; g_xm_btn = rep[0]; g_xm_dirty = 1;
        } else {                                         /* Tastatur */
            int c = usbkbd_decode(rep, 1); if (c >= 0) { usbkbd_xhci_push(c); }
        }
#else
        { int c = usbkbd_decode(rep, 1); if (c >= 0) { usbkbd_xhci_push(c); } }
#endif
        xhci_hid_prime(idx);                             /* das gemeldete Geraet neu primen */
    }
}

/* USB-Task (nur HW): AUTO-Start beim Boot (VL805 + Tastatur mit Retry), Fallback = manueller Trigger.
 * Reboot-Loop-Schutz ueber einen Breadcrumb-Zaehler auf hdd1 (nach zu vielen Fehlstarts skippen). */
void pcie_xhci_task(void *arg)
{
    (void)arg;
    static char rbuf[8];
    /* Kurz warten, bis Scheduler/VFS/Netzwerk stabil sind (falls der Bringup wider Erwarten haengt,
     * bleibt das System per Netz erreichbar). Dann Auto-Bringup mit Retry (der erste Versuch nach
     * Reboot ist transient flaky). ~1.5 s -> die Tastatur ist schnell nutzbar. */
    task_sleep_ticks(150);
    int giveup = 0;
    {
        char c[4] = { 0, 0, 0, 0 };
        int rr2 = vfs_read_file("hdd1:USBBOOT.CNT", c, 3);
        int cnt = (rr2 >= 1 && c[0] >= '0' && c[0] <= '9') ? (c[0] - '0') : 0;
        if (cnt >= 4) { giveup = 1; uart_puts("[kbd] zu viele Fehlstarts in Folge -> Auto-USB uebersprungen\n"); }
        else { char nc[2] = { (char)('0' + cnt + 1), 0 }; vfs_write_file("hdd1:USBBOOT.CNT", nc, 1); }
    }
    if (!giveup && g_vl805_found) {
        for (int attempt = 0; attempt < 4; attempt++) {
            uart_puts("[usb] Auto-Bringup Versuch "); uart_putdec((unsigned)(attempt + 1)); uart_puts("...\n");
            if (usb_input_bringup()) {
                vfs_write_file("hdd1:USBBOOT.CNT", "0", 1);   /* Erfolg -> Zaehler zuruecksetzen */
                usb_input_loop();                              /* kehrt nie zurueck */
            }
            task_sleep_ticks(30);   /* kurz zwischen Retry-Versuchen (flaky erster Versuch) */
        }
        uart_puts("[usb] Auto-Bringup fehlgeschlagen -- warte auf manuellen Trigger\n");
    }
    /* Fallback: auf manuellen Flag-Trigger warten (sendfile x hdd1:XHCIGO.FLG). */
    for (;;) {
        int r = vfs_read_file("hdd1:XHCIGO.FLG", rbuf, sizeof(rbuf));
        if (r >= 0) {
            vfs_delete("hdd1:XHCIGO.FLG");
            if (usb_input_bringup()) {
                vfs_write_file("hdd1:USBBOOT.CNT", "0", 1);
                usb_input_loop();
            }
        }
        task_sleep_ticks(200);
    }
}

void pcie_probe(void)
{
    uart_puts("[pcie] --- BCM2711 PCIe-RC @ 0xFD500000 Diagnose ---\n");
    /* Ab hier tolerieren wir externe Aborts bei PCIe-Zugriffen (Fixup: Fault-Instruktion
     * ueberspringen) -> ein noch nicht hochgezogener PCIe-Block crasht den Boot NICHT mehr,
     * die Fernsteuerung bleibt am Leben, Bringup ist kabellos iterierbar. */
    g_fault_fixup = 1;
    uint32_t st = rd(MISC_PCIE_STATUS);
    dump("MISC_PCIE_STATUS", MISC_PCIE_STATUS);
    /* brcmstb: bit4 = rc_dl_active (Data-Link up), bit5 = rc_phylinkup. Beide melden. */
    uart_puts("[pcie]   dl_active(bit4)=");
    uart_puts((st & (1u << 4)) ? "1" : "0");
    uart_puts(" phylinkup(bit5)=");
    uart_puts((st & (1u << 5)) ? "1\n" : "0\n");
    dump("MISC_MISC_CTRL", MISC_MISC_CTRL);
    dump("CPU_2_PCIE_WIN0_LO", MISC_CPU_2_PCIE_WIN0_LO);
    dump("CPU_2_PCIE_WIN0_HI", MISC_CPU_2_PCIE_WIN0_HI);
    dump("MISC_REVISION", MISC_REVISION);
    dump("RGR1_SW_INIT_1", RGR1_SW_INIT_1);

    /* --- Bringup-Versuch (D.2) --- */
    (void)pcie_bringup();
    dump("MISC_PCIE_STATUS(nach bringup)", MISC_PCIE_STATUS);

    /* RC selbst (bus0.dev0) + VL805 (bus1.dev0) Config-ID lesen. */
    uint32_t rc_id = cfg_read(0, 0, 0, 0x00);
    uart_puts("[pcie] RC   bus0.dev0 CFG[0x00]=");
    uart_puthex(rc_id);
    uart_puts("\n");
    uint32_t id = cfg_read(1, 0, 0, 0x00);
    uart_puts("[pcie] dev  bus1.dev0 CFG[0x00]=");
    uart_puthex(id);
    uart_puts(((id & 0xFFFFu) == 0x1106u) ? " -> VIA VL805 GEFUNDEN!\n" : " (kein VL805/0xFFFF=nix)\n");
    if ((id & 0xFFFFu) == 0x1106u) {
        uart_puts("[pcie] VL805 CFG[0x04]cmd/sts=");  uart_puthex(cfg_read(1, 0, 0, 0x04)); uart_puts("\n");
        uart_puts("[pcie] VL805 CFG[0x10]BAR0=");     uart_puthex(cfg_read(1, 0, 0, 0x10)); uart_puts("\n");
        uart_puts("[pcie] VL805 CFG[0x14]BAR1=");     uart_puthex(cfg_read(1, 0, 0, 0x14)); uart_puts("\n");
        g_vl805_found = 1;
    }
    /* WICHTIG: pcie_probe bleibt boot-sicher (nur Config-Space + RC-Reads, KEIN MMIO). Die riskante
     * VL805-xHCI-MMIO-Arbeit (Fenster/BAR/CMD + CAPLENGTH-Read) kann HAENGEN, wenn das Geraet nicht
     * antwortet -- ein Haenger ist NICHT per Fault-Fixup abfangbar und wuerde den Boot vor dem Netzwerk
     * killen. Sie laeuft daher NUR on-demand + Watchdog-gesichert (pcie_xhci_task) -> siehe unten. */
    uart_puts("[pcie] --- Diagnose Ende ---\n");
    g_fault_fixup = 0;   /* Fixup wieder aus -> echte Kernel-Bugs paniken danach wieder normal */
}

#endif /* PCIE_PROBE */
