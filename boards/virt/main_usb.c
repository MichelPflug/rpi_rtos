/*
 * boards/virt/main_usb.c  --  xHCI/USB-Test-Harness fuer QEMU `virt`
 *
 * Verifiziert den generischen xHCI-Treiber (T1.14) mit ECHTER qemu-xhci-Emulation:
 *   1) PCIe-ECAM-Scan findet den xHCI-Controller (Class 0x0c0330) auf Bus 0,
 *   2) BAR0 ins 32-bit-MMIO-Fenster (0x10000000) legen + MEM/Bus-Master an,
 *   3) xhci_init (HCRST, DCBAA, Command-/Event-Ring, R/S),
 *   4) Enumeration: Port-Reset -> Enable-Slot -> Address-Device -> GET_DESCRIPTOR(device),
 *      und die VID/PID des angeschlossenen `usb-kbd` (0x0627:0x0001) ausgeben.
 *
 * Reines Polling (kein IRQ). Ersetzt im -VirtUsb-Build boards/virt/main_virt.c.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "kmem.h"
#include "pci.h"
#include "xhci.h"
#include "usb_hc.h"
#include "usbmsc.h"

uint64_t g_entry_el;            /* von start.S gesetzt */
uint64_t g_dtb_ptr;             /* von start.S gesetzt (hier ungenutzt) */
extern char virt_vectors[];     /* aus trap_virt.S */
void mmu_virt_init(void);       /* aus mmu_virt.c */

/* Diagnose-Handler fuer unerwartete Exceptions (siehe trap_virt.S). */
void virt_trap(uint64_t idx)
{
    uart_puts("\n*** virt-Trap (Vektor-Index ");
    uart_putdec(idx);
    uart_puts(") ***\n  ESR_EL1 = ");
    uart_puthex(READ_SYSREG(esr_el1));
    uart_puts("\n  ELR_EL1 = ");
    uart_puthex(READ_SYSREG(elr_el1));
    uart_puts("\n  FAR_EL1 = ");
    uart_puthex(READ_SYSREG(far_el1));
    uart_puts("\n  angehalten.\n");
    for (;;) { wfe(); }
}

#define XHCI_CLASS24   0x0c0330u        /* Serial Bus / USB / xHCI */
#define XHCI_BAR_ADDR  0x10000000ULL    /* 32-bit-PCI-MMIO-Fenster des virt-RC */

/* --- USB-Massenspeicher-Klassentest: Enumeration + Configure-Endpoint (xHCI-Kern), danach
 *     der GETEILTE usbmsc.c-Klassentreiber ueber die usb_hc-vtable (xHCI-Backend). --- */
static uint8_t  g_cfgbuf[256] __attribute__((aligned(64)));
static uint8_t  g_in_dci, g_out_dci;

/* Config-Descriptor holen, MSC-Interface + Bulk-IN/OUT-Endpoints finden, SET_CONFIG +
 * Configure-Endpoint. 0 = ok, < 0 = Fehler. */
static int msc_setup(void)
{
    if (xhci_get_descriptor(2, 0, g_cfgbuf, 9) != 1) { return -10; }
    uint16_t total = (uint16_t)(g_cfgbuf[2] | (g_cfgbuf[3] << 8));
    if (total > sizeof(g_cfgbuf)) { total = sizeof(g_cfgbuf); }
    if (xhci_get_descriptor(2, 0, g_cfgbuf, total) != 1) { return -11; }
    uint8_t cfgval = g_cfgbuf[5];

    int msc = 0, in_ep = 0, out_ep = 0;
    uint16_t in_mps = 0, out_mps = 0;
    int i = 0;
    while (i + 2 <= (int)total) {
        uint8_t blen = g_cfgbuf[i], btype = g_cfgbuf[i + 1];
        /* Der DEKLARIERTE Deskriptor muss komplett in `total` (<= sizeof g_cfgbuf) liegen,
         * bevor irgendein Feld dahinter gelesen wird -- sonst Over-Read aus Fremdspeicher bei
         * malformten/ueberlangen (auf 256 gekappten) Deskriptoren. Zusaetzlich je Zweig die
         * Mindestlaenge pruefen, damit die Feld-Offsets (bis +5) innerhalb blen liegen. */
        if (blen < 2 || i + blen > (int)total) { break; }
        if (btype == 4 && blen >= 9) {                       /* Interface: bInterfaceClass @ +5 */
            msc = (g_cfgbuf[i + 5] == 0x08);
        } else if (btype == 5 && blen >= 7 && msc) {         /* Endpoint (7 Byte) */
            uint8_t addr = g_cfgbuf[i + 2], attr = g_cfgbuf[i + 3];
            uint16_t mps = (uint16_t)(g_cfgbuf[i + 4] | (g_cfgbuf[i + 5] << 8));
            if ((attr & 0x3u) == 2u) {                       /* Bulk */
                if (addr & 0x80) { in_ep = addr; in_mps = mps; }
                else            { out_ep = addr; out_mps = mps; }
            }
        }
        i += blen;
    }
    if (!in_ep || !out_ep) { return -12; }
    g_in_dci  = (uint8_t)(((in_ep & 0xf) * 2) + 1);
    g_out_dci = (uint8_t)((out_ep & 0xf) * 2);
    if (xhci_set_config(cfgval) != 1) { return -13; }
    if (xhci_config_bulk(g_in_dci, in_mps, g_out_dci, out_mps) != 0) { return -14; }
    return 0;
}

void kmain(void)
{
    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("   rpi_rtos  -  USB/xHCI-Harness (QEMU virt)\n");
    uart_puts("========================================\n");
    uart_puts("Eintritt auf EL");
    uart_putdec(g_entry_el);
    uart_puts(", jetzt EL");
    uart_putdec(current_el());
    uart_puts("\n");

    WRITE_SYSREG(vbar_el1, (uint64_t)(uintptr_t)virt_vectors);
    isb();

    uart_puts("[1] MMU (RAM + Peripherie + PCIe-ECAM)...\n");
    mmu_virt_init();

    uart_puts("[2] PCIe-ECAM: xHCI-Controller suchen (Class 0x0c0330)...\n");
    pci_bdf_t bdf = 0;
    if (!pci_find_class(XHCI_CLASS24, &bdf)) {
        uart_puts("    FEHLER: kein xHCI auf Bus 0 gefunden. Harness haelt an.\n");
        for (;;) { wfe(); }
    }
    uint16_t vid = pci_cfg_read16(bdf, 0x00);
    uint16_t did = pci_cfg_read16(bdf, 0x02);
    uart_puts("    [pci] xHCI @ bus0 dev");
    uart_putdec((bdf >> 3) & 0x1f);
    uart_puts(" VID=");
    uart_puthex(vid);
    uart_puts(" DID=");
    uart_puthex(did);
    uart_puts("\n");

    uint64_t bar = pci_bar_assign(bdf, 0, XHCI_BAR_ADDR);
    uart_puts("    [pci] BAR0 zugewiesen -> ");
    uart_puthex(bar);
    uart_puts("\n");
    if (!bar) {
        uart_puts("    FEHLER: BAR-Zuweisung. Harness haelt an.\n");
        for (;;) { wfe(); }
    }

    uart_puts("[3] xHCI initialisieren (HCRST + Ringe + R/S)...\n");
    int r = xhci_init(bar);
    uart_puts("    [xhci] init ret=");
    uart_putdec((uint32_t)(uint64_t)(int64_t)r);
    uart_puts(r == 0 ? " (ok)\n" : " (FEHLER)\n");
    if (r != 0) {
        uart_puts("[xhcitest] FEHLER: xhci_init\n");
        for (;;) { wfe(); }
    }

    uart_puts("[4] Enumeration: Port-Reset -> EnableSlot -> AddressDevice -> GET_DESCRIPTOR...\n");
    uint16_t dvid = 0, dpid = 0;
    int e = xhci_enumerate(&dvid, &dpid);
    if (e == 0) {
        uart_puts("    [xhci] Slot=");
        uart_putdec(xhci_last_slot());
        uart_puts(" Port=");
        uart_putdec(xhci_last_port());
        uart_puts(" Speed=");
        uart_putdec(xhci_last_speed());
        uart_puts("\n[xhcitest] Enumeration ok: VID=");
        uart_puthex(dvid);
        uart_puts(" PID=");
        uart_puthex(dpid);
        uart_puts("\n");

        uart_puts("[5] MSC ueber GETEILTEN usbmsc-Treiber (HCD-vtable usb_hc = xHCI)...\n");
        int m = msc_setup();
        if (m == 0) {
            uart_puts("    [msc] Bulk-EPs: IN dci=");
            uart_putdec(g_in_dci);
            uart_puts(" OUT dci=");
            uart_putdec(g_out_dci);
            uart_puts("\n");
            /* xHCI als aktiven Host-Controller registrieren -> der GETEILTE Klassen-Treiber
             * usbmsc.c (auf raspi mit DWC2) faehrt INQUIRY/READ CAPACITY/READ(10)/WRITE(10)
             * jetzt ueber xHCI. Beweist die usb_hc-vtable-Abstraktion end-to-end in QEMU. */
            usb_hc_register(xhci_hc_ops());
            usbmsc_probe();                          /* INQUIRY + READ CAPACITY + Sektor-RW-Test */
            static uint8_t sec[512] __attribute__((aligned(64)));
            int rr = usbmsc_read(0, 1, sec);          /* READ(10) LBA0 ueber usbmsc/xHCI */
            int marker_ok = (rr == 0 && sec[0] == 'R' && sec[1] == 'T' && sec[2] == 'O' && sec[3] == 'S');
            uart_puts("[msctest] usbmsc(GETEILT)/xHCI: sectors=");
            uart_putdec(usbmsc_sectors());
            uart_puts(" marker=");
            uart_puts(marker_ok ? "ok\n" : "FEHLT\n");
        } else {
            uart_puts("[msctest] MSC-Setup FEHLER m=");
            uart_putdec((uint32_t)(uint64_t)(int64_t)m);
            uart_puts("\n");
        }
    } else {
        uart_puts("[xhcitest] Enumeration FEHLER e=");
        uart_putdec((uint32_t)(uint64_t)(int64_t)e);
        uart_puts("\n");
    }

    for (;;) { wfe(); }
}
