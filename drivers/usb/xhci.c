/*
 * drivers/usb/xhci.c  --  Generischer xHCI-1.1-Host-Controller
 *
 * Ringe (Command/Event/Transfer), Slot-/Endpoint-Kontexte und Enumeration bis zum
 * GET_DESCRIPTOR(device). DMA-Strukturen liegen in identity-gemapptem NICHT-cacheable
 * RAM (virt) -> kohaerent ohne dc-Wartung; nur `dsb` vor dem Doorbell, damit die
 * TRB-Stores vor der Benachrichtigung sichtbar sind.
 */
#include <stdint.h>
#include "mmio.h"
#include "aarch64.h"
#include "kmem.h"
#include "uart.h"
#include "xhci.h"
#include "usb_hc.h"

/* --- Capability-Register (relativ zur MMIO-Basis) --- */
#define CAP_CAPLENGTH   0x00      /* [7:0] */
#define CAP_HCSPARAMS1  0x04      /* MaxSlots[7:0], MaxIntrs[18:8], MaxPorts[31:24] */
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10      /* CSZ bit2, xECP[31:16] */
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* --- Operational-Register (relativ zu op_base = base + CAPLENGTH) --- */
#define OP_USBCMD       0x00      /* R/S bit0, HCRST bit1, INTE bit2 */
#define OP_USBSTS       0x04      /* HCH bit0, CNR bit11 */
#define OP_CRCR         0x18      /* 64-bit: Command-Ring-Ptr | RCS bit0 */
#define OP_DCBAAP       0x30      /* 64-bit */
#define OP_CONFIG       0x38      /* MaxSlotsEn[7:0] */
#define OP_PORTSC(p)    (0x400 + 0x10 * ((p) - 1))

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBSTS_HCH      (1u << 0)
#define USBSTS_CNR      (1u << 11)

#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PRC      (1u << 21)
#define PORTSC_RW1C     (0x7Fu << 17)   /* CSC..CEC (RW1C) */
#define PORTSC_SPEED(v) (((v) >> 10) & 0xFu)

/* --- Runtime: Interrupter 0 (relativ zu rt_base + 0x20) --- */
#define IR0_IMAN        0x00
#define IR0_ERSTSZ      0x08
#define IR0_ERSTBA      0x10      /* 64-bit */
#define IR0_ERDP        0x18      /* 64-bit */
#define ERDP_EHB        (1u << 3)

/* --- TRB-Typen (control[15:10]) --- */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDR_DEV    11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_XFER_EVENT  32
#define TRB_CMD_COMPL   33
#define TRB_PORTSC_EVT  34
#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_C           (1u << 0)     /* Cycle */
#define TRB_TC          (1u << 1)     /* Toggle Cycle (Link) */
#define TRB_IDT         (1u << 6)     /* Immediate Data (Setup) */
#define TRB_IOC         (1u << 5)     /* Interrupt On Completion */
#define TRB_DIR_IN      (1u << 16)    /* Data/Status Stage direction */
#define TRB_TRT_OUT     (2u << 16)    /* Setup: Transfer Type = OUT-Data */
#define TRB_TRT_IN      (3u << 16)    /* Setup: Transfer Type = IN-Data */
#define COMP_SUCCESS       1
#define COMP_SHORT_PACKET  13     /* weniger Bytes als TD-Laenge, aber erfolgreich (kein Fehler) */
#define COMP_STALL         6      /* Endpoint-STALL (Aufrufer: clear_halt) */

#define CMDR_SIZE   16
#define EVTR_SIZE   64
#define TR_SIZE     16
#define MAX_SLOTS   16
#define MAX_SCRATCH 32     /* Obergrenze der Scratchpad-Puffer; darueber bricht init loud ab */

typedef struct { uint32_t d[4]; } trb_t;   /* 16 Byte */
typedef struct { uint64_t base; uint32_t size; uint32_t rsvd; } erst_ent_t;

/* --- DMA-Strukturen (NC-RAM, aligned) --- */
static uint64_t   g_dcbaa[64]                 __attribute__((aligned(64)));
static trb_t      g_cmd[CMDR_SIZE]            __attribute__((aligned(4096)));
static trb_t      g_evt[EVTR_SIZE]            __attribute__((aligned(4096)));
static erst_ent_t g_erst[1]                   __attribute__((aligned(64)));
static trb_t      g_ep0[TR_SIZE]             __attribute__((aligned(4096)));
static trb_t      g_epin[TR_SIZE]            __attribute__((aligned(4096)));  /* Bulk-IN-Ring */
static trb_t      g_epout[TR_SIZE]           __attribute__((aligned(4096)));  /* Bulk-OUT-Ring */
static uint8_t    g_input[33 * 64]           __attribute__((aligned(64)));   /* control+slot+31 ep */
static uint8_t    g_devctx[32 * 64]         __attribute__((aligned(64)));    /* slot+31 ep */
static uint8_t    g_xfer[256]               __attribute__((aligned(64)));
static uint64_t   g_scratch[MAX_SCRATCH]         __attribute__((aligned(64)));
static uint8_t    g_scratch_buf[MAX_SCRATCH][4096] __attribute__((aligned(4096)));

/* --- Multi-HID (Maus + Tastatur gleichzeitig hinter dem Hub) --- je Geraet ein eigener Interrupt-
 * Transfer-Ring + Output-Device-Context + Report-Puffer. GETRENNT von g_epin/g_devctx (Bulk/MSC) ->
 * der virt/MSC-Pfad bleibt unveraendert. Der Event-Ring g_evt ist geteilt; Events werden per Slot-ID
 * dem richtigen Geraet zugeordnet (Demux). */
#define NHID 2
static trb_t     g_hid_ring[NHID][TR_SIZE] __attribute__((aligned(4096)));
static uint8_t   g_hid_dctx[NHID][32 * 64] __attribute__((aligned(64)));
static uint8_t   g_hid_buf[NHID][64]       __attribute__((aligned(64)));
static uint32_t  g_hid_slot[NHID], g_hid_enq[NHID], g_hid_pcs[NHID];
static uint8_t   g_hid_dci[NHID];
static uint16_t  g_hid_mps[NHID];
static uint32_t  g_hub_slot, g_hub_root;   /* Hub-Slot + Root-Port (fuer Route-String/TT der Downstream-Geraete) */

/* --- Controller-Zustand --- */
static uint64_t g_base, g_op, g_rt, g_db;
static uint32_t g_maxslots, g_maxports, g_ctxsize;
static uint32_t g_cmd_enq, g_cmd_pcs;
static uint32_t g_evt_deq, g_evt_ccs;
static uint32_t g_ep0_enq, g_ep0_pcs;
static uint32_t g_epin_enq, g_epin_pcs, g_epout_enq, g_epout_pcs;
static uint8_t  g_in_dci, g_out_dci;
static uint32_t g_slot, g_port, g_speed;
/* Slot-Context-Felder des zuletzt enumerierten Geraets (Route+Speed / Root-Port / TT), damit ein
 * spaeteres Configure-Endpoint (das den Slot-Context via A0 ueberschreibt) Route-String + TT eines
 * Geraets HINTER einem Hub bewahrt. */
static uint32_t g_slc0b, g_slc1s, g_slc2s;

static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* --- Cache-Maintenance fuer NICHT-kohaerente DMA -------------------------------------------------
 * Auf echter Pi-4-HW haengt der VL805 hinter dem BCM2711-PCIe; seine DMA-Zugriffe auf den (cacheable)
 * Kernel-RAM werden von den A72-Caches NICHT gesnoopt. Daher: was der Controller LIEST (Ringe/Kontexte/
 * DCBAA/OUT-Daten), muss vor der Doorbell aus dem Cache bis zum DRAM geschrieben werden (dc cvac);
 * was der Controller SCHREIBT (Event-Ring, IN-Daten, Device-Context), muss vor dem CPU-Read im Cache
 * verworfen werden (dc ivac), damit die CPU die DRAM-Schreibung des Controllers sieht. In QEMU
 * (kohaerentes TCG-Modell) sind beide wirkungslos -> virt-Pfad unveraendert. 64-B-Cache-Line (A72). */
static void dc_clean(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + n;
    for (; a < e; a += 64) { __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory"); }
    __asm__ volatile("dsb sy" ::: "memory");
}
static void dc_inval(const void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + n;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < e; a += 64) { __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory"); }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Alle vom Controller GELESENEN DMA-Strukturen bis zum DRAM cleanen (vor jeder Doorbell aufrufen).
 * Heavy-handed, aber robust: so sieht der Controller immer die frischen TRBs/Kontexte. */
static void xhci_clean_all(void)
{
    dc_clean(g_dcbaa, sizeof(g_dcbaa));
    dc_clean(g_cmd,   sizeof(g_cmd));
    dc_clean(g_erst,  sizeof(g_erst));
    dc_clean(g_ep0,   sizeof(g_ep0));
    dc_clean(g_epin,  sizeof(g_epin));
    dc_clean(g_epout, sizeof(g_epout));
    dc_clean(g_input, sizeof(g_input));
    dc_clean(g_devctx, sizeof(g_devctx));
    dc_clean(g_xfer,  sizeof(g_xfer));
    dc_clean(g_scratch, sizeof(g_scratch));
    dc_clean(g_hid_ring, sizeof(g_hid_ring));    /* Multi-HID: per-Geraet-Interrupt-Ringe */
    dc_clean(g_hid_dctx, sizeof(g_hid_dctx));    /* per-Geraet-Output-Contexts */
}

static uint32_t rd(uint64_t a)            { return mmio_read32((uintptr_t)a); }
static void     wr(uint64_t a, uint32_t v){ mmio_write32((uintptr_t)a, v); }
static void     wr64(uint64_t a, uint64_t v) { wr(a, (uint32_t)v); wr(a + 4, (uint32_t)(v >> 32)); }

static void xhci_udelay(uint32_t us)
{
    uint64_t f = READ_SYSREG(cntfrq_el0);
    if (!f) { return; }
    uint64_t start = READ_SYSREG(cntpct_el0);
    uint64_t ticks = (f / 1000000u) * us;
    if (!ticks) { ticks = 1; }
    while (READ_SYSREG(cntpct_el0) - start < ticks) { }
}

/* Wartet (bounded, ~ms) bis (rd(a) & mask) == want. 1 = ok, 0 = Timeout. */
static int wait_bits(uint64_t a, uint32_t mask, uint32_t want, uint32_t ms)
{
    for (uint32_t i = 0; i < ms * 1000u; i++) {
        if ((rd(a) & mask) == want) { return 1; }
        xhci_udelay(1);
    }
    return 0;
}

static uint32_t *ctx_ent(uint8_t *b, int i) { return (uint32_t *)(b + i * g_ctxsize); }

/* Nächsten Event-Ring-Eintrag holen (bounded warten). Liefert 1 + füllt code, slot, type, ptr,
 * resid; 0 = Timeout. Aktualisiert ERDP. resid = "TRB Transfer Length" [23:0] = bei Transfer-
 * Events die NICHT übertragenen Rest-Bytes (Residue) -> übertragen = angefordert - resid. */
static int poll_event(uint32_t *code, uint32_t *slot, uint32_t *type, uint64_t *ptr,
                      uint32_t *resid, uint32_t ms)
{
    for (uint32_t i = 0; i < ms * 1000u; i++) {
        trb_t *e = &g_evt[g_evt_deq];
        dc_inval(e, sizeof(*e));                 /* HW schreibt Events per DMA -> stale Cache-Line verwerfen */
        if ((e->d[3] & TRB_C) == g_evt_ccs) {
            uint32_t c3 = e->d[3];
            if (code)  { *code  = (e->d[2] >> 24) & 0xFFu; }
            if (resid) { *resid = e->d[2] & 0xFFFFFFu; }
            if (slot)  { *slot  = (c3 >> 24) & 0xFFu; }
            if (type)  { *type  = (c3 >> 10) & 0x3Fu; }
            if (ptr)   { *ptr   = (uint64_t)e->d[0] | ((uint64_t)e->d[1] << 32); }
            g_evt_deq++;
            if (g_evt_deq == EVTR_SIZE) { g_evt_deq = 0; g_evt_ccs ^= TRB_C; }
            wr64(g_rt + 0x20 + IR0_ERDP, (uint64_t)(uintptr_t)&g_evt[g_evt_deq] | ERDP_EHB);
            return 1;
        }
        xhci_udelay(1);
    }
    return 0;
}

/* Command-TRB (param, control ohne Cycle) einreihen, Doorbell 0 läuten, auf Command-Completion
 * warten. Liefert Completion-Code (<0 = Timeout), *out_slot = zugewiesener Slot. */
static int submit_cmd(uint64_t param, uint32_t control, uint32_t *out_slot)
{
    trb_t *t = &g_cmd[g_cmd_enq];
    t->d[0] = (uint32_t)param;
    t->d[1] = (uint32_t)(param >> 32);
    t->d[2] = 0;
    t->d[3] = control | g_cmd_pcs;
    g_cmd_enq++;
    if (g_cmd_enq == CMDR_SIZE - 1) {          /* Link-TRB: Cycle setzen, wrappen, togglen */
        g_cmd[CMDR_SIZE - 1].d[3] = TRB_TYPE(TRB_LINK) | TRB_TC | g_cmd_pcs;
        g_cmd_enq = 0;
        g_cmd_pcs ^= TRB_C;
    }
    xhci_clean_all();                           /* HW liest den Command-TRB per DMA -> Cache -> DRAM */
    wr(g_db + 0, 0);                            /* Command-Ring-Doorbell (Target 0) */

    uint32_t code = 0, slot = 0, type = 0;
    uint64_t ptr = 0;
    for (int tries = 0; tries < 8; tries++) {
        if (!poll_event(&code, &slot, &type, &ptr, 0, 200)) { return -1; }
        if (type == TRB_CMD_COMPL) {
            if (out_slot) { *out_slot = slot; }
            return (int)code;
        }
        /* Port-Status-Change o.ä. überspringen. */
    }
    return -2;
}

int xhci_init(uint64_t mmio_base)
{
    g_base = mmio_base;
    uint32_t caplen = rd(g_base + CAP_CAPLENGTH) & 0xFFu;
    g_op = g_base + caplen;
    g_rt = g_base + (rd(g_base + CAP_RTSOFF) & ~0x1Fu);
    g_db = g_base + (rd(g_base + CAP_DBOFF) & ~0x3u);

    uint32_t hcs1 = rd(g_base + CAP_HCSPARAMS1);
    g_maxslots = hcs1 & 0xFFu;
    g_maxports = (hcs1 >> 24) & 0xFFu;
    g_ctxsize  = (rd(g_base + CAP_HCCPARAMS1) & (1u << 2)) ? 64u : 32u;
    if (g_maxslots > MAX_SLOTS) { g_maxslots = MAX_SLOTS; }

    /* 1) Reset. */
    wr(g_op + OP_USBCMD, USBCMD_HCRST);
    if (!wait_bits(g_op + OP_USBCMD, USBCMD_HCRST, 0, 100)) { return -1; }
    if (!wait_bits(g_op + OP_USBSTS, USBSTS_CNR, 0, 100))   { return -2; }

    /* 2) Slots freigeben. */
    wr(g_op + OP_CONFIG, g_maxslots);

    /* 3) DCBAA (+ Scratchpad falls gefordert). */
    for (int i = 0; i < 64; i++) { g_dcbaa[i] = 0; }
    uint32_t hcs2 = rd(g_base + CAP_HCSPARAMS2);
    uint32_t nscr = (((hcs2 >> 21) & 0x1Fu) << 5) | ((hcs2 >> 27) & 0x1Fu);
    if (nscr > MAX_SCRATCH) {
        return -4;                              /* LOUD FAIL: der HC liest genau nscr Zeiger aus
                                                 * dem Array (HCSPARAMS2); ein stilles Kappen liesse
                                                 * ihn NULL-Zeiger -> DMA auf Phys 0 nutzen. */
    }
    if (nscr) {
        for (uint32_t i = 0; i < nscr; i++) { g_scratch[i] = (uint64_t)(uintptr_t)g_scratch_buf[i]; }
        g_dcbaa[0] = (uint64_t)(uintptr_t)g_scratch;
    }
    wr64(g_op + OP_DCBAAP, (uint64_t)(uintptr_t)g_dcbaa);

    /* 4) Command-Ring. */
    memset(g_cmd, 0, sizeof(g_cmd));
    g_cmd[CMDR_SIZE - 1].d[0] = (uint32_t)(uintptr_t)g_cmd;
    g_cmd[CMDR_SIZE - 1].d[1] = (uint32_t)((uint64_t)(uintptr_t)g_cmd >> 32);
    g_cmd[CMDR_SIZE - 1].d[3] = TRB_TYPE(TRB_LINK) | TRB_TC;
    g_cmd_enq = 0; g_cmd_pcs = TRB_C;
    wr64(g_op + OP_CRCR, (uint64_t)(uintptr_t)g_cmd | 1u /* RCS */);

    /* 5) Event-Ring + Interrupter 0. */
    memset(g_evt, 0, sizeof(g_evt));
    g_erst[0].base = (uint64_t)(uintptr_t)g_evt;
    g_erst[0].size = EVTR_SIZE;
    g_erst[0].rsvd = 0;
    g_evt_deq = 0; g_evt_ccs = TRB_C;
    wr(g_rt + 0x20 + IR0_ERSTSZ, 1);
    wr64(g_rt + 0x20 + IR0_ERDP, (uint64_t)(uintptr_t)g_evt);
    wr64(g_rt + 0x20 + IR0_ERSTBA, (uint64_t)(uintptr_t)g_erst);

    /* 6) Run. Vor R/S ALLE frisch initialisierten DMA-Strukturen bis zum DRAM cleanen (die HW liest
     *    DCBAA/Command-Ring/ERST/Scratchpad sofort per DMA; das Event-Ring muss sauber-genullt in DRAM
     *    liegen, damit Cycle-Bit + dc_inval greifen). Nicht-kohaerente PCIe-DMA auf echter HW. */
    xhci_clean_all();
    dc_clean(g_evt, sizeof(g_evt));
    dc_clean(g_scratch_buf, sizeof(g_scratch_buf));
    wr(g_op + OP_USBCMD, USBCMD_RS);
    if (!wait_bits(g_op + OP_USBSTS, USBSTS_HCH, 0, 100)) { return -3; }
    return 0;
}

/* Ein TRB in einen Transfer-Ring legen (Cycle aus *pcs), mit Link-Wrap+Toggle am vorletzten
 * Eintrag -- damit wiederholte Transfers den Ring nicht ueberlaufen. */
static void ring_push(trb_t *ring, uint32_t *enq, uint32_t *pcs,
                      uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    trb_t *t = &ring[*enq];
    t->d[0] = d0; t->d[1] = d1; t->d[2] = d2; t->d[3] = d3 | *pcs;
    (*enq)++;
    if (*enq == TR_SIZE - 1) {
        ring[TR_SIZE - 1].d[3] = TRB_TYPE(TRB_LINK) | TRB_TC | *pcs;
        *enq = 0;
        *pcs ^= TRB_C;
    }
}

/* Residue (nicht übertragene Rest-Bytes) des zuletzt von xfer_wait konsumierten Transfer-Events. */
static uint32_t g_last_resid;

/* Auf ein Transfer-Event warten (bounded); Command-/Port-Events dazwischen ueberspringen. */
static int xfer_wait(uint32_t ms)
{
    uint32_t code = 0, type = 0, resid = 0;
    for (int tries = 0; tries < 16; tries++) {
        if (!poll_event(&code, 0, &type, 0, &resid, ms)) { return -1; }
        if (type == TRB_XFER_EVENT) { g_last_resid = resid; return (int)code; }
    }
    return -2;
}

/* Transfer erfolgreich? SUCCESS oder SHORT_PACKET (Kurz-Transfer ist kein Fehler). Auf SS ist
 * das im QEMU-Fixture nie ausgeloest, auf echter HW (kurze CSW/Descriptor-Antworten) aber real.
 * Auf COMP_SUCCESS normiert, damit der dokumentierte "1 = ok"-Vertrag der API erhalten bleibt. */
static int xfer_norm(int code)
{
    return (code == COMP_SUCCESS || code == COMP_SHORT_PACKET) ? COMP_SUCCESS : code;
}

/* Ring/State fuer eine gegebene DCI (EP0=1, sonst Bulk-IN/OUT). 0 = unbekannt. */
static trb_t *ring_for(uint8_t dci, uint32_t **enq, uint32_t **pcs)
{
    if (dci == 1)            { *enq = &g_ep0_enq;   *pcs = &g_ep0_pcs;   return g_ep0; }
    if (dci == g_in_dci)     { *enq = &g_epin_enq;  *pcs = &g_epin_pcs;  return g_epin; }
    if (dci == g_out_dci)    { *enq = &g_epout_enq; *pcs = &g_epout_pcs; return g_epout; }
    return 0;
}

/* Link-TRB eines Transfer-Rings initialisieren + Zustand zuruecksetzen. */
static void ring_init(trb_t *ring, uint32_t *enq, uint32_t *pcs)
{
    memset(ring, 0, sizeof(trb_t) * TR_SIZE);
    ring[TR_SIZE - 1].d[0] = (uint32_t)(uintptr_t)ring;
    ring[TR_SIZE - 1].d[1] = (uint32_t)((uint64_t)(uintptr_t)ring >> 32);
    ring[TR_SIZE - 1].d[3] = TRB_TYPE(TRB_LINK) | TRB_TC;
    *enq = 0; *pcs = TRB_C;
}

/* EP0-Control-Transfer. buf/len = Datenphase (len==0 -> keine), dir_in = Richtung der Daten. */
static int ep0_control(const uint8_t setup[8], void *buf, uint32_t len, int dir_in)
{
    uint32_t trt = len ? (dir_in ? TRB_TRT_IN : TRB_TRT_OUT) : 0u;
    ring_push(g_ep0, &g_ep0_enq, &g_ep0_pcs,
              (uint32_t)setup[0] | (setup[1] << 8) | (setup[2] << 16) | ((uint32_t)setup[3] << 24),
              (uint32_t)setup[4] | (setup[5] << 8) | (setup[6] << 16) | ((uint32_t)setup[7] << 24),
              8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
    if (len) {
        uint64_t b = (uint64_t)(uintptr_t)buf;
        ring_push(g_ep0, &g_ep0_enq, &g_ep0_pcs, (uint32_t)b, (uint32_t)(b >> 32), len,
                  TRB_TYPE(TRB_DATA) | (dir_in ? TRB_DIR_IN : 0u));
    }
    /* Status Stage: bei Daten-IN OUT, sonst (Daten-OUT / No-Data) IN. */
    uint32_t sdir = (len && dir_in) ? 0u : TRB_DIR_IN;
    ring_push(g_ep0, &g_ep0_enq, &g_ep0_pcs, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | sdir);

    if (len && !dir_in) { dc_clean(buf, len); } /* OUT-Daten -> DRAM, bevor die HW sie liest */
    xhci_clean_all();                           /* Ring/Kontexte -> DRAM */
    wr(g_db + 4u * g_slot, 1);                 /* Slot-Doorbell, DCI 1 = EP0 */
    int rc = xfer_norm(xfer_wait(300));
    if (len && dir_in) { dc_inval(buf, len); }  /* IN-Daten: HW schrieb per DMA -> Cache verwerfen */
    return rc;
}

/* Rueckwaerts-kompatibler Wrapper (Enumeration liest in g_xfer). */
static int ep0_control_in(const uint8_t setup[8], uint32_t len)
{
    return ep0_control(setup, g_xfer, len, 1);
}

int xhci_enumerate(uint16_t *vid, uint16_t *pid)
{
    /* Diagnose: ALLE Root-Ports scannen (Verbindung + Speed) -> zeigt, wo Maus/Tastatur haengen.
     * Speed: 1=FS, 2=LS, 3=HS(USB2), 4=SS(USB3). */
    for (uint32_t p = 1; p <= g_maxports; p++) {
        uint32_t s = rd(g_op + OP_PORTSC(p));
        uart_puts("[xhci-dbg] port"); uart_putdec(p);
        uart_puts(" sc="); uart_puthex(s);
        uart_puts(" ccs="); uart_puts((s & PORTSC_CCS) ? "1" : "0");
        uart_puts(" sp="); uart_putdec(PORTSC_SPEED(s)); uart_puts("\n");
    }

    /* 1) Root-Port mit Verbindung finden + reset. */
    uint32_t port = 0;
    for (uint32_t p = 1; p <= g_maxports; p++) {
        if (rd(g_op + OP_PORTSC(p)) & PORTSC_CCS) { port = p; break; }
    }
    if (!port) { return -1; }
    uint32_t sc = rd(g_op + OP_PORTSC(port));
    wr(g_op + OP_PORTSC(port), (sc & ~(PORTSC_PED | PORTSC_RW1C)) | PORTSC_PR);
    if (!wait_bits(g_op + OP_PORTSC(port), PORTSC_PRC, PORTSC_PRC, 200)) { return -2; }
    sc = rd(g_op + OP_PORTSC(port));
    wr(g_op + OP_PORTSC(port), (sc & ~(PORTSC_PED)) | PORTSC_PRC);   /* PRC (RW1C) löschen */
    g_port  = port;
    g_speed = PORTSC_SPEED(sc);

    /* 2) Enable Slot. */
    uart_puts("[xhci-dbg] port="); uart_putdec(port);
    uart_puts(" portsc="); uart_puthex(sc);
    uart_puts(" speed="); uart_putdec(g_speed);
    uart_puts(" usbsts="); uart_puthex(rd(g_op + OP_USBSTS));
    uart_puts(" crcr="); uart_puthex(rd(g_op + OP_CRCR)); uart_puts("\n");
    uint32_t slot = 0;
    int rc = submit_cmd(0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
    uart_puts("[xhci-dbg] enable-slot rc="); uart_puthex((uint32_t)rc);
    uart_puts(" slot="); uart_putdec(slot);
    uart_puts(" usbsts="); uart_puthex(rd(g_op + OP_USBSTS)); uart_puts("\n");
    if (rc != COMP_SUCCESS || slot == 0 || slot > g_maxslots) { return -3; }
    g_slot = slot;

    /* 3) EP0-Transfer-Ring vorbereiten. */
    memset(g_ep0, 0, sizeof(g_ep0));
    g_ep0[TR_SIZE - 1].d[0] = (uint32_t)(uintptr_t)g_ep0;
    g_ep0[TR_SIZE - 1].d[1] = (uint32_t)((uint64_t)(uintptr_t)g_ep0 >> 32);
    g_ep0[TR_SIZE - 1].d[3] = TRB_TYPE(TRB_LINK) | TRB_TC;
    g_ep0_enq = 0; g_ep0_pcs = TRB_C;

    /* 4) Input-Context: Add-Flags A0(slot)+A1(ep0), Slot-Ctx, EP0-Ctx. */
    memset(g_input, 0, sizeof(g_input));
    memset(g_devctx, 0, sizeof(g_devctx));
    uint32_t *icc  = ctx_ent(g_input, 0);
    icc[1] = 0x3u;                              /* A0 | A1 */
    uint32_t *slc  = ctx_ent(g_input, 1);
    slc[0] = (g_speed << 20) | (1u << 27);      /* Speed + Context Entries = 1 */
    slc[1] = (port << 16);                      /* Root-Hub-Port-Nummer */
    g_slc0b = (g_speed << 20); g_slc1s = (port << 16); g_slc2s = 0;   /* Slot-Ctx merken (Root, keine TT) */
    /* EP0-MaxPacketSize nach Speed (LS/FS=8, HS=64, SS=512). */
    uint32_t mps = (g_speed == 4) ? 512 : (g_speed == 3) ? 64 : 8;
    uint32_t *ep0 = ctx_ent(g_input, 2);
    ep0[1] = (3u << 1) | (4u << 3) | (mps << 16);   /* CErr=3, EPType=Control(4), MPS */
    ep0[2] = (uint32_t)(uintptr_t)g_ep0 | 1u;       /* TR Dequeue Ptr | DCS */
    ep0[3] = (uint32_t)((uint64_t)(uintptr_t)g_ep0 >> 32);
    ep0[4] = 8;                                     /* Average TRB Length (>0 gefordert) */

    g_dcbaa[slot] = (uint64_t)(uintptr_t)g_devctx;
    dsb();

    /* 5) Address Device. */
    rc = submit_cmd((uint64_t)(uintptr_t)g_input, TRB_TYPE(TRB_ADDR_DEV) | (slot << 24), 0);
    if (rc != COMP_SUCCESS) { return -4; }

    /* 6) GET_DESCRIPTOR(device): erst 8 Byte (durch EP0-MPS begrenzt), bMaxPacketSize0 lesen;
     * bei Full-Speed die reale MPS ggf. per Evaluate Context nachziehen (sonst Babble bei
     * MPS0>8); dann den vollen 18-Byte-Descriptor. Fuer LS/HS/SS ist die Schaetzung exakt. */
    static const uint8_t setup8[8]  = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 8,  0x00 };
    static const uint8_t setup18[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0x00 };
    memset(g_xfer, 0, 18);
    dsb();
    rc = ep0_control_in(setup8, 8);
    if (rc != COMP_SUCCESS) { return -5; }

    if (g_speed == 1) {                             /* Full-Speed: MPS0 = tatsaechlicher Wert */
        uint32_t real_mps = g_xfer[7] ? g_xfer[7] : 8;
        if (real_mps != mps) {
            memset(g_input, 0, sizeof(g_input));
            ctx_ent(g_input, 0)[1] = 0x2u;          /* A1: nur EP0 evaluieren */
            uint32_t *e = ctx_ent(g_input, 2);
            e[1] = (3u << 1) | (4u << 3) | (real_mps << 16);
            e[4] = 8;
            dsb();
            rc = submit_cmd((uint64_t)(uintptr_t)g_input, TRB_TYPE(TRB_EVAL_CTX) | (slot << 24), 0);
            if (rc != COMP_SUCCESS) { return -6; }
        }
    }

    memset(g_xfer, 0, 18);
    dsb();
    rc = ep0_control_in(setup18, 18);
    if (rc != COMP_SUCCESS) { return -7; }

    if (vid) { *vid = (uint16_t)(g_xfer[8] | (g_xfer[9] << 8)); }
    if (pid) { *pid = (uint16_t)(g_xfer[10] | (g_xfer[11] << 8)); }
    return 0;
}

/* Nach xhci_enumerate (g_slot = Hub, USB2/HS): Hub konfigurieren + Downstream-Ports powern + Status
 * scannen. Diagnose-lastig -> zeigt, an welchen Hub-Ports Maus/Tastatur haengen. Gibt bNbrPorts. */
int xhci_hub_scan(void)
{
    g_hub_slot = g_slot; g_hub_root = g_port;   /* Hub-Slot/Root fuer die Downstream-Enumeration merken */
    uint8_t sc[8] = { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };   /* SET_CONFIGURATION(1) */
    if (ep0_control(sc, 0, 0, 0) != COMP_SUCCESS) { uart_puts("[hub] SET_CONFIG fehlgeschlagen\n"); return -1; }

    /* GET class hub-descriptor (Typ 0x29). bmRequestType=0xA0 (D2H, class, device). */
    uint8_t gd[8] = { 0xA0, 0x06, 0x00, 0x29, 0x00, 0x00, 0x40, 0x00 };
    memset(g_xfer, 0, 64);
    if (ep0_control(gd, g_xfer, 64, 1) != COMP_SUCCESS) { uart_puts("[hub] GET-hubdesc fehlgeschlagen\n"); return -2; }
    uint8_t nports = g_xfer[2];                                          /* bNbrPorts */
    uart_puts("[hub] bNbrPorts="); uart_putdec(nports); uart_puts("\n");
    if (nports == 0 || nports > 15) { return -3; }

    /* Alle Downstream-Ports powern: SET_FEATURE(PORT_POWER=8), bmRequestType=0x23 (H2D, class, other). */
    for (uint8_t p = 1; p <= nports; p++) {
        uint8_t sp[8] = { 0x23, 0x03, 0x08, 0x00, p, 0x00, 0x00, 0x00 };
        ep0_control(sp, 0, 0, 0);
    }
    xhci_udelay(150000);                                                /* Power-Good abwarten (~150ms) */

    /* Port-Status lesen: GET_STATUS, bmRequestType=0xA3 (D2H, class, other), wLength=4. bit0 = Connect. */
    for (uint8_t p = 1; p <= nports; p++) {
        uint8_t gs[8] = { 0xA3, 0x00, 0x00, 0x00, p, 0x00, 0x04, 0x00 };
        memset(g_xfer, 0, 4);
        if (ep0_control(gs, g_xfer, 4, 1) == COMP_SUCCESS) {
            uint32_t st = (uint32_t)g_xfer[0] | ((uint32_t)g_xfer[1] << 8) |
                          ((uint32_t)g_xfer[2] << 16) | ((uint32_t)g_xfer[3] << 24);
            uart_puts("[hub] port"); uart_putdec(p);
            uart_puts(" wPortStatus="); uart_puthex(st & 0xFFFFu);
            uart_puts((st & 0x1u) ? " CONNECTED\n" : " -\n");
        }
    }
    return nports;
}

/* Hub-Downstream-Port hub_port reset + Speed lesen. g_slot MUSS beim Aufruf der HUB sein (nutzt
 * dessen EP0) -> ALLE Port-Resets VOR den Downstream-Enumerationen ausfuehren (der geteilte g_ep0
 * wird sonst vom ersten Downstream-Geraet belegt). Liefert Speed (1=FS,2=LS,3=HS) oder <0. */
int xhci_hub_port_reset(uint8_t hub_port)
{
    uint8_t rst[8] = { 0x23, 0x03, 0x04, 0x00, hub_port, 0x00, 0x00, 0x00 };   /* SET_FEATURE PORT_RESET */
    if (ep0_control(rst, 0, 0, 0) != COMP_SUCCESS) { return -1; }
    xhci_udelay(80000);                          /* >=50 ms Reset + Recovery */
    uint8_t gs[8] = { 0xA3, 0x00, 0x00, 0x00, hub_port, 0x00, 0x04, 0x00 };    /* GET_STATUS */
    memset(g_xfer, 0, 4);
    if (ep0_control(gs, g_xfer, 4, 1) != COMP_SUCCESS) { return -2; }
    uint32_t pst = (uint32_t)g_xfer[0] | ((uint32_t)g_xfer[1] << 8);
    if (!(pst & 0x1u)) { return -3; }            /* nicht (mehr) verbunden */
    uint32_t dspeed = (pst & (1u << 9)) ? 2u : (pst & (1u << 10)) ? 3u : 1u;   /* LS/HS/FS */
    uart_puts("[hub] port"); uart_putdec(hub_port);
    uart_puts(" reset pst="); uart_puthex(pst); uart_puts(" speed="); uart_putdec(dspeed); uart_puts("\n");
    return (int)dspeed;
}

/* Downstream-Geraet an hub_port (bereits reset, Speed dspeed) als Geraet-Index idx (0/1) enumerieren:
 * Enable-Slot + Address-Device (Route-String/TT aus g_hub_slot/g_hub_root) + Device-Descriptor. Nutzt
 * den per-Geraet-Output-Context g_hid_dctx[idx] + den geteilten g_ep0 (sequenziell, nur bei Setup).
 * Setzt g_hid_slot[idx]. 0 = ok. */
int xhci_hub_enum_device(int idx, uint8_t hub_port, uint32_t dspeed, uint16_t *vid, uint16_t *pid)
{
    uint32_t hub_slot = g_hub_slot, root_port = g_hub_root;

    uint32_t slot = 0;
    int rc = submit_cmd(0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
    if (rc != COMP_SUCCESS || slot == 0 || slot > g_maxslots) { return -4; }
    g_slot = slot; g_speed = dspeed; g_port = root_port;
    g_hid_slot[idx] = slot;

    /* EP0-Transfer-Ring (geteilt, sequenziell). */
    memset(g_ep0, 0, sizeof(g_ep0));
    g_ep0[TR_SIZE - 1].d[0] = (uint32_t)(uintptr_t)g_ep0;
    g_ep0[TR_SIZE - 1].d[1] = (uint32_t)((uint64_t)(uintptr_t)g_ep0 >> 32);
    g_ep0[TR_SIZE - 1].d[3] = TRB_TYPE(TRB_LINK) | TRB_TC;
    g_ep0_enq = 0; g_ep0_pcs = TRB_C;

    /* Input-Context mit Route-String (tier1 = hub_port) + TT (Parent-Hub-Slot/-Port). */
    memset(g_input, 0, sizeof(g_input));
    memset(g_hid_dctx[idx], 0, sizeof(g_hid_dctx[idx]));
    ctx_ent(g_input, 0)[1] = 0x3u;                                   /* A0 | A1 */
    uint32_t *slc = ctx_ent(g_input, 1);
    slc[0] = ((uint32_t)hub_port & 0xFu) | (dspeed << 20) | (1u << 27);
    slc[1] = (root_port << 16);
    slc[2] = (hub_slot & 0xFFu) | (((uint32_t)hub_port & 0xFFu) << 8);
    g_slc0b = ((uint32_t)hub_port & 0xFu) | (dspeed << 20);          /* Slot-Ctx merken (fuer Configure-EP) */
    g_slc1s = (root_port << 16);
    g_slc2s = (hub_slot & 0xFFu) | (((uint32_t)hub_port & 0xFFu) << 8);
    uint32_t mps = (dspeed == 3) ? 64u : 8u;
    uint32_t *ep0 = ctx_ent(g_input, 2);
    ep0[1] = (3u << 1) | (4u << 3) | (mps << 16);
    ep0[2] = (uint32_t)(uintptr_t)g_ep0 | 1u;
    ep0[3] = (uint32_t)((uint64_t)(uintptr_t)g_ep0 >> 32);
    ep0[4] = 8;
    g_dcbaa[slot] = (uint64_t)(uintptr_t)g_hid_dctx[idx];            /* per-Geraet-Output-Context */
    dsb();

    rc = submit_cmd((uint64_t)(uintptr_t)g_input, TRB_TYPE(TRB_ADDR_DEV) | (slot << 24), 0);
    if (rc != COMP_SUCCESS) { uart_puts("[hub] addr-dev fail rc="); uart_puthex((uint32_t)rc); uart_puts("\n"); return -5; }

    static const uint8_t s8[8]  = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 8,  0x00 };
    static const uint8_t s18[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0x00 };
    memset(g_xfer, 0, 18); dsb();
    if (ep0_control_in(s8, 8) != COMP_SUCCESS) { return -6; }
    uint32_t real_mps = g_xfer[7] ? g_xfer[7] : 8;
    if (real_mps != mps) {
        memset(g_input, 0, sizeof(g_input));
        ctx_ent(g_input, 0)[1] = 0x2u;
        uint32_t *e = ctx_ent(g_input, 2);
        e[1] = (3u << 1) | (4u << 3) | (real_mps << 16); e[4] = 8;
        dsb();
        if (submit_cmd((uint64_t)(uintptr_t)g_input, TRB_TYPE(TRB_EVAL_CTX) | (slot << 24), 0) != COMP_SUCCESS) { return -7; }
    }
    memset(g_xfer, 0, 18); dsb();
    if (ep0_control_in(s18, 18) != COMP_SUCCESS) { return -8; }
    if (vid) { *vid = (uint16_t)(g_xfer[8]  | (g_xfer[9]  << 8)); }
    if (pid) { *pid = (uint16_t)(g_xfer[10] | (g_xfer[11] << 8)); }
    uart_puts("[hub] DEV idx="); uart_putdec((unsigned)idx);
    uart_puts(" vid="); uart_puthex(g_xfer[8] | (g_xfer[9] << 8));
    uart_puts(" pid="); uart_puthex(g_xfer[10] | (g_xfer[11] << 8)); uart_puts("\n");
    return 0;
}

/* GET_DESCRIPTOR (Control-IN) beliebigen Typs in buf. Liefert Completion-Code. */
int xhci_get_descriptor(uint8_t type, uint8_t index, void *buf, uint16_t len)
{
    uint8_t s[8] = { 0x80, 0x06, index, type, 0, 0, (uint8_t)len, (uint8_t)(len >> 8) };
    return ep0_control(s, buf, len, 1);
}

/* SET_CONFIGURATION (Control-OUT, keine Datenphase). */
int xhci_set_config(uint8_t cfg)
{
    uint8_t s[8] = { 0x00, 0x09, cfg, 0, 0, 0, 0, 0 };
    return ep0_control(s, 0, 0, 0);
}

/* Configure Endpoint: Bulk-IN (in_dci) + Bulk-OUT (out_dci) am Slot einrichten. 0 = ok. */
int xhci_config_bulk(uint8_t in_dci, uint16_t in_mps, uint8_t out_dci, uint16_t out_mps)
{
    if (in_dci < 2 || in_dci > 31 || out_dci < 2 || out_dci > 31) { return -3; }
    g_in_dci = in_dci; g_out_dci = out_dci;
    ring_init(g_epin,  &g_epin_enq,  &g_epin_pcs);
    ring_init(g_epout, &g_epout_enq, &g_epout_pcs);

    memset(g_input, 0, sizeof(g_input));
    uint32_t maxdci = in_dci > out_dci ? in_dci : out_dci;
    ctx_ent(g_input, 0)[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);   /* A0 | A(in) | A(out) */
    uint32_t *slc = ctx_ent(g_input, 1);
    slc[0] = (g_speed << 20) | (maxdci << 27);   /* Speed + Context Entries = groesste DCI */
    slc[1] = (g_port << 16);

    /* Im INPUT-Context liegt DCI n bei Eintrag (n+1): Eintrag 0=Control, 1=Slot, 2=DCI1(EP0). */
    uint32_t *ei = ctx_ent(g_input, in_dci + 1);
    ei[1] = (3u << 1) | (6u << 3) | ((uint32_t)in_mps << 16);   /* CErr=3, EPType=Bulk-IN(6), MPS */
    ei[2] = (uint32_t)(uintptr_t)g_epin | 1u;
    ei[3] = (uint32_t)((uint64_t)(uintptr_t)g_epin >> 32);
    ei[4] = in_mps;

    uint32_t *eo = ctx_ent(g_input, out_dci + 1);
    eo[1] = (3u << 1) | (2u << 3) | ((uint32_t)out_mps << 16);  /* CErr=3, EPType=Bulk-OUT(2), MPS */
    eo[2] = (uint32_t)(uintptr_t)g_epout | 1u;
    eo[3] = (uint32_t)((uint64_t)(uintptr_t)g_epout >> 32);
    eo[4] = out_mps;

    dsb();
    int rc = submit_cmd((uint64_t)(uintptr_t)g_input, TRB_TYPE(TRB_CONFIG_EP) | (g_slot << 24), 0);
    return (rc == COMP_SUCCESS) ? 0 : -1;
}

/* HID-Interrupt-IN-Endpoint des aktuellen Geraets (g_slot) fuer Geraet-Index idx (0/1) einrichten:
 * Config-Descriptor lesen, ersten Interrupt-IN-EP finden, SET_CONFIGURATION, Configure-Endpoint
 * (EPType=7) mit DEM per-Geraet-Ring g_hid_ring[idx]. Slot-Context aus den gemerkten g_slc*-Feldern
 * -> Route-String/TT des Hub-Geraets bleiben erhalten. Gibt die IN-DCI (>0) oder <0. */
int xhci_hid_setup(int idx)
{
    static uint8_t cfg[96];
    if (xhci_get_descriptor(0x02, 0, cfg, 9) != COMP_SUCCESS) { uart_puts("[hid] cfg9 fail\n"); return -1; }
    uint32_t total = (uint32_t)cfg[2] | ((uint32_t)cfg[3] << 8);
    if (total > sizeof(cfg)) { total = sizeof(cfg); }
    if (xhci_get_descriptor(0x02, 0, cfg, (uint16_t)total) != COMP_SUCCESS) { uart_puts("[hid] cfg fail\n"); return -2; }

    uint8_t epaddr = 0; uint16_t epmps = 0;
    for (uint32_t i = 0; i + 6 < total; ) {
        uint8_t blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2) { break; }
        if (btype == 0x05 && (cfg[i + 3] & 0x3u) == 0x3u && (cfg[i + 2] & 0x80u)) {   /* Interrupt IN */
            epaddr = cfg[i + 2]; epmps = (uint16_t)(cfg[i + 4] | (cfg[i + 5] << 8)); break;
        }
        i += blen;
    }
    if (!epaddr) { uart_puts("[hid] kein Interrupt-IN-EP\n"); return -3; }
    uint8_t dci = (uint8_t)(((epaddr & 0xFu) * 2u) + 1u);
    uart_puts("[hid] int-in ep="); uart_puthex(epaddr);
    uart_puts(" mps="); uart_putdec(epmps); uart_puts(" dci="); uart_putdec(dci); uart_puts("\n");

    if (xhci_set_config(1) != COMP_SUCCESS) { uart_puts("[hid] SET_CONFIG fail\n"); return -4; }

    ring_init(g_hid_ring[idx], &g_hid_enq[idx], &g_hid_pcs[idx]);   /* per-Geraet-Interrupt-Ring */
    memset(g_input, 0, sizeof(g_input));
    ctx_ent(g_input, 0)[1] = (1u << 0) | (1u << dci);           /* A0 | A(dci) */
    uint32_t *slc = ctx_ent(g_input, 1);
    slc[0] = g_slc0b | ((uint32_t)dci << 27);                    /* Route+Speed + CtxEntries = dci */
    slc[1] = g_slc1s;
    slc[2] = g_slc2s;                                            /* TT (Hub-Geraet) bewahren */
    uint32_t *ei = ctx_ent(g_input, dci + 1);
    ei[0] = (7u << 16);                                          /* Interval ~ 2^7 Microframes (~16 ms) */
    ei[1] = (3u << 1) | (7u << 3) | ((uint32_t)epmps << 16);     /* CErr=3, EPType=Interrupt-IN(7), MPS */
    ei[2] = (uint32_t)(uintptr_t)g_hid_ring[idx] | 1u;
    ei[3] = (uint32_t)((uint64_t)(uintptr_t)g_hid_ring[idx] >> 32);
    ei[4] = epmps;
    dsb();
    if (submit_cmd((uint64_t)(uintptr_t)g_input, TRB_TYPE(TRB_CONFIG_EP) | (g_slot << 24), 0) != COMP_SUCCESS) {
        uart_puts("[hid] CONFIG_EP fail\n"); return -5;
    }
    g_hid_dci[idx] = dci; g_hid_mps[idx] = epmps ? epmps : 8;
    uart_puts("[hid] Interrupt-Endpoint konfiguriert (dci="); uart_putdec(dci); uart_puts(")\n");
    return dci;
}

/* Einen ausstehenden Interrupt-Transfer fuer Geraet idx anlegen (Normal-TRB auf dessen Ring +
 * Doorbell). Der Report landet spaeter per DMA in g_hid_buf[idx]; das Transfer-Event holt
 * xhci_hid_wait. Beide Geraete werden geprimt gehalten -> je genau ein ausstehender Transfer. */
void xhci_hid_prime(int idx)
{
    uint32_t n = g_hid_mps[idx];
    if (n > sizeof(g_hid_buf[idx])) { n = (uint32_t)sizeof(g_hid_buf[idx]); }
    uint64_t b = (uint64_t)(uintptr_t)g_hid_buf[idx];
    ring_push(g_hid_ring[idx], &g_hid_enq[idx], &g_hid_pcs[idx],
              (uint32_t)b, (uint32_t)(b >> 32), n, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    xhci_clean_all();
    dc_clean(g_hid_ring[idx], sizeof(g_hid_ring[idx]));   /* HW liest den Ring-TRB per DMA */
    dc_clean(g_hid_dctx[idx], sizeof(g_hid_dctx[idx]));
    wr(g_db + 4u * g_hid_slot[idx], g_hid_dci[idx]);      /* Slot-Doorbell, Target = Interrupt-DCI */
}

/* Auf den naechsten HID-Report warten (bounded ~ms). Ordnet das Transfer-Event per Slot-ID einem
 * der nhid Geraete (0..nhid-1) zu, invalidiert dessen DMA-Puffer + kopiert bis outlen Byte nach out.
 * Liefert den Geraet-Index (>=0) oder -1 (Timeout). Command-/Port-Events werden uebersprungen. */
int xhci_hid_wait(int nhid, uint8_t *out, uint32_t outlen, uint32_t ms)
{
    uint32_t code = 0, slot = 0, type = 0;
    for (int tries = 0; tries < 12; tries++) {
        if (!poll_event(&code, &slot, &type, 0, 0, ms)) { return -1; }
        if (type != TRB_XFER_EVENT) { continue; }        /* Command-/Port-Change-Event -> weiter */
        for (int i = 0; i < nhid; i++) {
            if (g_hid_slot[i] == slot) {
                uint32_t n = g_hid_mps[i];
                if (n > outlen) { n = outlen; }
                dc_inval(g_hid_buf[i], n);                /* HW schrieb den Report per DMA */
                for (uint32_t k = 0; k < n; k++) { out[k] = g_hid_buf[i][k]; }
                return i;
            }
        }
        /* Event fuer einen unbekannten Slot -> ignorieren */
    }
    return -1;
}

/* Ein Bulk-Transfer (Normal-TRB) auf der Endpoint-DCI. Liefert Completion-Code (SUCCESS=1). */
int xhci_bulk(uint8_t dci, void *buf, uint32_t len)
{
    uint32_t *enq, *pcs;
    trb_t *ring = ring_for(dci, &enq, &pcs);
    if (!ring || dci == 1) { return -3; }
    uint64_t b = (uint64_t)(uintptr_t)buf;
    ring_push(ring, enq, pcs, (uint32_t)b, (uint32_t)(b >> 32), len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    if (dci == g_out_dci) { dc_clean(buf, len); }   /* OUT-Daten -> DRAM, bevor die HW sie liest */
    xhci_clean_all();
    wr(g_db + 4u * g_slot, dci);               /* Slot-Doorbell, Target = DCI */
    int rc = xfer_norm(xfer_wait(500));
    if (dci == g_in_dci) { dc_inval(buf, len); }    /* IN-Daten: HW schrieb per DMA -> Cache verwerfen */
    return rc;
}

uint32_t xhci_last_slot(void)  { return g_slot; }
uint32_t xhci_last_port(void)  { return g_port; }
uint32_t xhci_last_speed(void) { return g_speed; }

/* --- HCD-vtable-Anbindung (usb_hc): xHCI treibt die geteilten Klassen-Treiber usbmsc/usbkbd ---
 * bulk() bildet die dwc2-Semantik nach: dir 0=OUT/1=IN -> die per xhci_config_bulk eingerichtete
 * Endpoint-DCI; Rueckgabe: Bytes (Erfolg, inkl. Short via xfer_norm), -2 = STALL, <0 = Fehler. */
static int xhci_hc_bulk(int dir, void *buf, int len)
{
    uint8_t dci = dir ? g_in_dci : g_out_dci;
    int code = xhci_bulk(dci, buf, (uint32_t)len);
    if (code == COMP_SUCCESS) {
        /* ECHTE Byte-Zahl (angefordert - Residue), nicht len -- sonst wuerde ein Kurz-Transfer
         * (z.B. ein auf < 13 Byte gekuerztes CSW auf echter HW) usbmscs `cr < 13`-Schutz aushebeln
         * und stale Puffer-Bytes als Erfolg werten. Deckt sich mit dwc2_bulk (len - rem). */
        int got = len - (int)g_last_resid;
        return got < 0 ? 0 : got;
    }
    if (code == COMP_STALL) { return -2; }
    return -1;
}

/* STALL-/Fehler-Recovery auf xHCI = Reset-Endpoint- + Set-TR-Dequeue-Pointer-Command.
 * Noch nicht implementiert (im QEMU-usb-storage-Happy-Path nicht ausgeloest); auf echter
 * VL805-HW in T1.15/T1.21 nachzuruesten. Ehrliche No-ops statt stiller Falsch-Recovery. */
static void xhci_hc_clear_halt(int dir) { (void)dir; }
static void xhci_hc_bot_reset(void)     { }

/* Massenspeicher, sobald xhci_config_bulk die Bulk-Endpoints eingerichtet hat (sonst keins).
 * HID-Interrupt-Transfers kennt der T1.14-Kern noch nicht -> keine kbd_*-ops. */
static int xhci_hc_dev_kind(void)
{
    return (g_in_dci && g_out_dci) ? 2 : 0;
}

static const usb_hc_ops_t xhci_hc_ops_inst = {
    .name       = "xhci",
    .bulk       = xhci_hc_bulk,
    .clear_halt = xhci_hc_clear_halt,
    .bot_reset  = xhci_hc_bot_reset,
    .dev_kind   = xhci_hc_dev_kind,
    /* kbd_irq_getreport / kbd_poll bleiben NULL (kein Interrupt-Transfer im T1.14-Kern) */
};

const usb_hc_ops_t *xhci_hc_ops(void) { return &xhci_hc_ops_inst; }
