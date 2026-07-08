/*
 * arch/aarch64/mmu.c  --  MMU-Setup fuer BCM2711 (AArch64, 4KB-Granule)
 *
 * Strategie (M1-Fundament): einfache Identity-Map ueber TTBR0_EL1 mit
 * 1-GiB-Bloecken auf Level 1. Higher-Half-Kernel (TTBR1) folgt spaeter.
 *
 * > Zu verifizieren (echte HW): Vor dem Einschalten der Caches sollte der
 *   Daten-Cache invalidiert werden, falls die Firmware ihn nicht sauber
 *   hinterlassen hat. In QEMU ist das unkritisch.
 */
#include <stdint.h>
#include "aarch64.h"
#include "proc.h"
#include "spinlock.h"
#include "mmu.h"
#include "fdt.h"
#include "gui_fb.h"
#ifdef DIAG_BLINK
#include "diag_blink.h"   /* Bring-up-Latches vor/nach dem MMU-Enable (Multimeter-Diagnose) */
#endif

extern uint64_t g_dtb_ptr;   /* von start.S: DTB-Zeiger (0 = keiner, z.B. QEMU) */

/* BCM2711 low-peripheral-Fenster: [PERIPH_BASE, 4 GiB) ist MMIO (GENET 0xFD580000,
 * PCIe 0xFD500000, Mailbox 0xFE00B880, UART 0xFE201000, GIC 0xFF840000). Darunter
 * (0xC0000000..PERIPH_BASE) ist auf 4/8-GB-Boards physischer RAM. */
#define PERIPH_BASE   0xFC000000UL
#define GIB           (1UL << 30)
#define TWO_MIB       (1UL << 21)

/* Diagnose: von mmu_init gesetzt (T1.12-Guardian liest sie). */
static uint64_t s_ram_mapped;     /* Summe gemappter Normal-RAM-Bytes */
static int      s_ram_from_dtb;   /* 1 = aus DTB /memory, 0 = Grobkarte (kein DTB) */
uint64_t mmu_ram_mapped(void)  { return s_ram_mapped; }
int      mmu_ram_from_dtb(void) { return s_ram_from_dtb; }

/* MAIR-Attribute */
#define MAIR_DEVICE_nGnRnE  0x00
#define MAIR_NORMAL_WB      0xFF
#define ATTR_IDX_DEVICE     0
#define ATTR_IDX_NORMAL     1

/* Block-Deskriptor-Flags (Level 1, 4KB-Granule) */
#define DESC_VALID_BLOCK    (1UL << 0)            /* bits[1:0] = 0b01 (Block) */
#define DESC_ATTR(idx)      ((uint64_t)(idx) << 2)
#define DESC_AP_RW_EL1      (0UL << 6)            /* EL1 RW, kein EL0-Zugriff */
#define DESC_AP_RW_ALL      (1UL << 6)            /* EL1 + EL0 RW */
#define DESC_TABLE          0x3                   /* Table-Deskriptor (bits[1:0]=11) */
#define DESC_SH_INNER       (3UL << 8)            /* Inner Shareable */
#define DESC_AF             (1UL << 10)           /* Access Flag */
#define DESC_NG             (1UL << 11)           /* non-Global -> ASID-getaggt */
#define DESC_PXN            (1UL << 53)
#define DESC_UXN            (1UL << 54)

#define MMU_MAX_ASPACE      8                     /* gleichzeitige User-Adressraeume */

/* Level-1-Tabelle: 512 Eintraege x 1 GiB, 4KB-aligned. */
static uint64_t l1_table[512] __attribute__((aligned(4096)));
/* Level-2-Tabelle fuer 0..1 GiB (512 x 2 MiB) -> erlaubt feines EL0-Mapping
 * der User-Kachel. */
static uint64_t l2_table[512] __attribute__((aligned(4096)));

/* Per-Prozess-Adressraeume (SMP): jeder User-Prozess bekommt eigene L1+L2-Tabellen.
 * Die Kernel-Eintraege werden aus den globalen Templates kopiert (global, ASID-frei),
 * nur die User-Kachel ist per-Prozess + nG (ASID-getaggt). So kann jeder Kern via
 * eigenem TTBR0 gleichzeitig einen ANDEREN Prozess fahren, ohne TLB-Flush beim Switch. */
static uint64_t aspace_l1[MMU_MAX_ASPACE][512] __attribute__((aligned(4096)));
static uint64_t aspace_l2[MMU_MAX_ASPACE][512] __attribute__((aligned(4096)));
static uint8_t  aspace_used[MMU_MAX_ASPACE];
/* Schuetzt aspace_used[] cross-core: ein Laufzeit-Spawn alloziert den Aspace auf Kern 0,
 * der Exit-Hook gibt ihn auf dem ZIEL-Kern (z.B. Kern 1) wieder frei -> nebenlaeufig. Nur
 * die Slot-Buchhaltung ist geschuetzt; das Befuellen der Tabellen eines frisch reservierten
 * Slots ist konfliktfrei (kein anderer Kern haelt ihn) und laeuft ausserhalb des Locks. */
static spinlock_t aspace_lock = SPINLOCK_INIT;

/* --- T1.12: DTB-getriebene Voll-RAM-Karte ---
 * Pool von L2-Tabellen fuer GiBs, die 2-MiB-fein aufgeloest werden muessen (teil-gemappte
 * RAM-Regionen + der Peripherie-Split in GiB 3). Voll-gemappte GiBs nutzen 1-GiB-L1-Bloecke
 * (kein Pool). 16 L2 reichen fuer die Grenz-/Peripherie-GiBs eines 8-GB-Boards. */
#define L2_POOL_N   16
#define MMU_MAX_GIB 512                       /* 39-bit VA (T0SZ=25) -> 512 GiB adressierbar */
static uint64_t l2_pool[L2_POOL_N][512] __attribute__((aligned(4096)));
static int      l2_pool_next;

#define FDT_MAX_REGIONS 8

/* Ist Adresse a in irgendeiner RAM-Region? */
static int addr_in_regions(const fdt_mem_region_t *r, int n, uint64_t a)
{
    for (int i = 0; i < n; i++) {
        if (a >= r[i].base && a < r[i].base + r[i].size) {
            return 1;
        }
    }
    return 0;
}

/* Baut die Karte aus DTB-RAM-Regionen: jede 2-MiB-Seite in einer Region -> Normal.
 * Voll gedeckte GiBs -> 1-GiB-L1-Normal-Block; teil-gedeckte GiBs -> L2 aus dem Pool.
 * GiB 0 bleibt die l2_table (User-Kachel-Loch); GiB 3 wird danach fuer den Peripherie-
 * Split ueberschrieben. Liefert die Summe der gemappten Normal-RAM-Bytes. */
static uint64_t map_ram_regions(const fdt_mem_region_t *r, int n,
                                uint64_t normal_l1, uint64_t normal_l2)
{
    uint64_t mapped = 0;
    for (uint64_t g = 1; g < MMU_MAX_GIB; g++) {
        if (g == 3) {
            continue;                          /* GiB 3: Peripherie-Split separat */
        }
        uint64_t gbase = g * GIB;
        /* 2-MiB-Seiten dieser GiB zaehlen, die in einer Region liegen. */
        int covered = 0;
        for (int p = 0; p < 512; p++) {
            if (addr_in_regions(r, n, gbase + (uint64_t)p * TWO_MIB)) {
                covered++;
            }
        }
        if (covered == 0) {
            continue;
        }
        if (covered == 512) {
            l1_table[g] = gbase | normal_l1;   /* voll -> 1-GiB-Block */
            mapped += GIB;
            continue;
        }
        /* teil-gedeckt -> L2 aus dem Pool. */
        if (l2_pool_next >= L2_POOL_N) {
            /* Pool erschoepft: diese GiB grob als Voll-Block mappen (konservativ, deckt
             * mind. die Region ab; Rest ist auf einem RAM-Board ohnehin RAM). */
            l1_table[g] = gbase | normal_l1;
            mapped += GIB;
            continue;
        }
        uint64_t *l2 = l2_pool[l2_pool_next++];
        for (int p = 0; p < 512; p++) {
            uint64_t va = gbase + (uint64_t)p * TWO_MIB;
            if (addr_in_regions(r, n, va)) {
                l2[p] = va | normal_l2;
                mapped += TWO_MIB;
            } else {
                l2[p] = 0;                     /* Loch (nicht gemappt) */
            }
        }
        l1_table[g] = (uint64_t)(uintptr_t)l2 | DESC_TABLE;
    }
    return mapped;
}

void mmu_init(void)
{
    const uint64_t normal = DESC_VALID_BLOCK | DESC_ATTR(ATTR_IDX_NORMAL) |
                            DESC_AP_RW_EL1 | DESC_SH_INNER | DESC_AF;
    const uint64_t device = DESC_VALID_BLOCK | DESC_ATTR(ATTR_IDX_DEVICE) |
                            DESC_AP_RW_EL1 | DESC_AF | DESC_PXN | DESC_UXN;

    /* 0..1 GiB ueber eine L2-Tabelle (2-MiB-Bloecke), Kernel (AP=00, kein EL0). Die
     * USER_BASE-Kachel wird unten ausgelassen; per-Prozess-Adressraeume (mmu_create_aspace)
     * mappen sie EL0-zugaenglich + nG/ASID-getaggt. */
    const uint64_t k2m = DESC_VALID_BLOCK | DESC_ATTR(ATTR_IDX_NORMAL) |
                         DESC_AP_RW_EL1 | DESC_SH_INNER | DESC_AF | DESC_UXN;
    for (int i = 0; i < 512; i++) {
        l2_table[i] = ((uint64_t)i << 21) | k2m;
    }
    /* Die USER_BASE-Kachel im KERNEL-Adressraum NICHT mappen: jeder Prozess mappt diese
     * VA per-Prozess als nG (ASID-getaggt). Ein zusaetzlicher GLOBALER Kernel-Eintrag fuer
     * dieselbe VA fuehrt zu einem global+nG-TLB-Konflikt (CONSTRAINED UNPREDICTABLE auf
     * echter HW). Der Kernel greift nie auf VA USER_BASE zu (User-Images via phys-Identity
     * ab USER_PHYS_BASE). In QEMU (TCG) unsichtbar, auf Cortex-A72 ein Isolations-Defekt.
     * Das Tile ist USER_SIZE gross (mehrere 2-MiB-Bloecke) -> die GANZE Region aushoehlen. */
    for (uint32_t ub = 0; ub < (uint32_t)(USER_SIZE >> 21); ub++) {
        l2_table[((USER_BASE >> 21) & 0x1FF) + ub] = 0;
    }
    /* Ebenso das GUI-Framebuffer-Fenster (T2.1) im KERNEL-Adressraum auslassen: es wird per-
     * Prozess (nur mit USER_CAP_GUI) als nG gemappt -> ein zusaetzlicher globaler Eintrag ergaebe
     * denselben global+nG-TLB-Konflikt. Der Kernel greift nie auf VA/phys GUI_FB_USER_VA zu.
     * GUI_BB_TILES Kacheln (1 bei 640x480, 5 bei -DFULLHD/1080p). */
    for (uint32_t fbt = 0; fbt < GUI_BB_TILES; fbt++) {
        l2_table[((GUI_FB_USER_VA >> 21) & 0x1FF) + fbt] = 0;
    }
    l1_table[0] = (uint64_t)&l2_table[0] | DESC_TABLE;   /* 0..1 GiB -> L2 (beide Pfade) */

    /* T1.12: Wenn der Bootloader einen DTB uebergab (echte HW), die WAHRE RAM-Groesse aus
     * /memory nehmen und den GESAMTEN RAM als Normal mappen (auch 3..4 GiB unterhalb der
     * Peripherie und High-RAM >4 GiB auf 4/8-GB-Boards) + die MMIO-Fenster als Device.
     * Ohne DTB (QEMU raspi4b) exakt die bisherige konservative Grobkarte (0..3 GiB Normal,
     * 3..4 GiB Device) -> QEMU-Regression ausgeschlossen. */
    fdt_mem_region_t regs[FDT_MAX_REGIONS];
#ifdef FORCE_COARSE_MAP
    /* Bring-up-Experiment: den (auf echter HW bisher ungetesteten) DTB-Speicherpfad UMGEHEN und
     * die einfache, QEMU-erprobte Grobkarte erzwingen (0..3 GiB Normal, 3..4 GiB Device). Bootet
     * der Pi damit, lag der frühe Hänger im DTB-Mapping. Gegated -> ohne das Flag unveraendert. */
    int nreg = 0;
    (void)regs;
#else
    int nreg = fdt_get_memory(g_dtb_ptr, regs, FDT_MAX_REGIONS);
#endif
    if (nreg > 0) {
        s_ram_from_dtb = 1;
        /* GiB 0: die l2_table mappt alle 512 2-MiB-Seiten Normal AUSSER der User-Kachel
         * (1 Loch) -- unabhaengig von den DTB-Regionen. Also 511 Seiten (nicht region-gezaehlt,
         * sonst wird das Loch mitgezaehlt bzw. der VC-Carveout am GiB0-Ende untergezaehlt). */
        uint64_t mapped = 511ULL * TWO_MIB;
        mapped += map_ram_regions(regs, nreg, normal, k2m);   /* GiB 1,2,4.. */
        /* GiB 3: Peripherie-Split via eigene L2 (>=1 Pool-Slot frei, falls nicht -> ganz Device). */
        if (l2_pool_next < L2_POOL_N) {
            uint64_t *l2 = l2_pool[l2_pool_next++];
            for (int p = 0; p < 512; p++) {
                uint64_t va = 3UL * GIB + (uint64_t)p * TWO_MIB;
                if (va >= PERIPH_BASE) {
                    l2[p] = va | device;                      /* MMIO (GENET/PCIe/Mailbox/GIC) */
                } else if (addr_in_regions(regs, nreg, va)) {
                    l2[p] = va | k2m; mapped += TWO_MIB;      /* RAM unterhalb der Peripherie */
                } else {
                    l2[p] = va | device;                      /* nicht als RAM gemeldet -> Device */
                }
            }
            l1_table[3] = (uint64_t)(uintptr_t)l2 | DESC_TABLE;
        } else {
            l1_table[3] = (3UL << 30) | device;
        }
        s_ram_mapped = mapped;
    } else {
        s_ram_from_dtb = 0;
        l1_table[1] = (1UL << 30) | normal;                  /* 1..2 GiB */
        l1_table[2] = (2UL << 30) | normal;                  /* 2..3 GiB */
        l1_table[3] = (3UL << 30) | device;                  /* 3..4 GiB */
        s_ram_mapped = 3UL * GIB;
    }

#ifdef PCIE_PROBE
    /* PCIe-Outbound-Fenster (BCM2711, Pi4-DT ranges): CPU 0x6_00000000 -> PCIe-Mem. Als Device-MMIO
     * mappen, damit die VL805-xHCI-Register (BAR im Fenster) vom Kernel erreichbar sind. GLOBAL hier
     * in mmu_init gesetzt (vor jeder mmu_create_aspace-Erzeugung) -> jeder Aspace erbt den Eintrag
     * (l1_table[i] wird kopiert) -> ein USB-IRQ kann aus JEDEM Kontext (auch waehrend ein User-
     * Aspace aktiv ist) auf die xHCI-MMIO zugreifen, wie beim dwc2 in GiB 3. GiB 24 = 0x600000000>>30.
     * In QEMU harmlos (nichts greift dort zu; pcie_probe ist HW-only via mmu_ram_from_dtb). */
    l1_table[24] = (24UL * GIB) | device;
#endif

    /* Page-Table-Stores bis in den RAM treiben und etwaige stale/dirty Cache-Lines
     * ueber der Tabelle bereinigen, BEVOR die Caches aktiviert werden. Auf echter HW
     * lief die Firmware mit aktiven Caches; ohne diese Wartung koennte eine dirty
     * Cache-Line spaeter die frisch geschriebene Tabelle ueberschreiben. (Cortex-A72:
     * 64-Byte-Cache-Line.) In QEMU wegen kohaerentem Cache-Modell unsichtbar. */
    dsb_sy();
    for (uintptr_t p = (uintptr_t)l1_table;
         p < (uintptr_t)l1_table + sizeof(l1_table); p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    for (uintptr_t p = (uintptr_t)l2_table;
         p < (uintptr_t)l2_table + sizeof(l2_table); p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    /* T1.12: die im DTB-Pfad benutzten L2-Pool-Tabellen ebenso zur PoC treiben. */
    for (int t = 0; t < l2_pool_next; t++) {
        for (uintptr_t p = (uintptr_t)l2_pool[t];
             p < (uintptr_t)l2_pool[t] + sizeof(l2_pool[t]); p += 64) {
            __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
        }
    }
    dsb_sy();

    /* MAIR: attr0 = Device-nGnRnE, attr1 = Normal Write-Back. */
    uint64_t mair = ((uint64_t)MAIR_NORMAL_WB     << (8 * ATTR_IDX_NORMAL)) |
                    ((uint64_t)MAIR_DEVICE_nGnRnE << (8 * ATTR_IDX_DEVICE));
    WRITE_SYSREG(mair_el1, mair);

    /* TCR_EL1: 39-bit VA (T0SZ=25), 4KB TG0, WB-WA cacheable Walks,
     * Inner-Shareable, IPS=40-bit, TTBR1-Walks aus (EPD1). */
    uint64_t tcr = (25UL)          /* T0SZ */
                 | (1UL << 8)      /* IRGN0 = WB-WA */
                 | (1UL << 10)     /* ORGN0 = WB-WA */
                 | (3UL << 12)     /* SH0   = Inner Shareable */
                 | (0UL << 14)     /* TG0   = 4KB */
                 | (1UL << 23)     /* EPD1  = TTBR1-Walks deaktiviert */
                 | (2UL << 32);    /* IPS   = 40-bit */
    WRITE_SYSREG(tcr_el1, tcr);

    WRITE_SYSREG(ttbr0_el1, (uint64_t)&l1_table[0]);
    isb();

    /* TLB invalidieren, dann sicherstellen, dass alles sichtbar ist. */
    __asm__ volatile("dsb ish; tlbi vmalle1; dsb ish; isb" ::: "memory");

    /* Instruktions-Cache invalidieren, bevor SCTLR.I gesetzt wird (Firmware lief
     * mit aktivem I-Cache; Inhalt nach Handoff architektonisch unbestimmt). */
    __asm__ volatile("ic iallu; dsb ish; isb" ::: "memory");

#ifdef DIAG_BLINK
    diag_latch(24);   /* Tabellen + Cache-Maint + MAIR/TCR/TTBR/TLB/IC fertig, VOR SCTLR-Enable (Pin 18) */
#endif

    /* MMU + Daten-Cache (C) + Instruktions-Cache (I) einschalten. */
    uint64_t sctlr = READ_SYSREG(sctlr_el1);
    sctlr |= (1UL << 0)   /* M: MMU an */
           | (1UL << 2)   /* C: Daten-Cache an */
           | (1UL << 12); /* I: Instruktions-Cache an */
    WRITE_SYSREG(sctlr_el1, sctlr);
    isb();
#ifdef DIAG_BLINK
    diag_latch(25);   /* MMU/Caches AN + isb ueberstanden -> MMU-Enable + MMIO-Mapping ok (Pin 22) */
#endif
}

void mmu_init_secondary(void)
{
    /* Dieselben inner-shareable Tabellen wie Core 0 (l1_table/l2_table sind bereits
     * aufgebaut). Nur die per-Core-Systemregister setzen + MMU/Caches einschalten.
     * SMPEN (Cluster-Kohaerenz) wird vom Firmware-/QEMU-Armstub vor dem Release
     * bereits gesetzt -> hier nicht angefasst (EL1-Schreibzugriff koennte trappen). */
    uint64_t mair = ((uint64_t)MAIR_NORMAL_WB     << (8 * ATTR_IDX_NORMAL)) |
                    ((uint64_t)MAIR_DEVICE_nGnRnE << (8 * ATTR_IDX_DEVICE));
    WRITE_SYSREG(mair_el1, mair);

    uint64_t tcr = (25UL)          /* T0SZ = 39-bit VA */
                 | (1UL << 8)      /* IRGN0 = WB-WA */
                 | (1UL << 10)     /* ORGN0 = WB-WA */
                 | (3UL << 12)     /* SH0   = Inner Shareable */
                 | (0UL << 14)     /* TG0   = 4KB */
                 | (1UL << 23)     /* EPD1  = TTBR1-Walks aus */
                 | (2UL << 32);    /* IPS   = 40-bit */
    WRITE_SYSREG(tcr_el1, tcr);

    WRITE_SYSREG(ttbr0_el1, (uint64_t)&l1_table[0]);
    isb();

    __asm__ volatile("dsb ish; tlbi vmalle1; dsb ish; isb" ::: "memory");
    __asm__ volatile("ic iallu; dsb ish; isb" ::: "memory");

    uint64_t sctlr = READ_SYSREG(sctlr_el1);
    sctlr |= (1UL << 0)   /* M: MMU an */
           | (1UL << 2)   /* C: Daten-Cache an */
           | (1UL << 12); /* I: Instruktions-Cache an */
    WRITE_SYSREG(sctlr_el1, sctlr);
    isb();
}

/* TTBR0 des Kernel-Adressraums (globale Identity-Map, ASID 0). */
uint64_t mmu_kernel_ttbr(void)
{
    return (uint64_t)(uintptr_t)&l1_table[0];     /* ASID 0 in [63:48] */
}

/* Tabellen eines Aspace-Slots zur PoC clean'en (Table-Walker / andere Kerne). */
static void aspace_clean(int s)
{
    dsb_sy();
    for (uintptr_t p = (uintptr_t)&aspace_l1[s][0];
         p < (uintptr_t)&aspace_l1[s][0] + sizeof(aspace_l1[s]); p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    for (uintptr_t p = (uintptr_t)&aspace_l2[s][0];
         p < (uintptr_t)&aspace_l2[s][0] + sizeof(aspace_l2[s]); p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    dsb_sy();
}

/* Legt einen Per-Prozess-Adressraum an: Kernel-Mappings (global) kopieren, eigene
 * L2 fuer 0..1 GiB, die User-Kachel (USER_BASE) auf 'phys' (EL0 RW, nG/ASID-getaggt).
 * Liefert die Aspace-ID (>=0) oder -1. */
int mmu_create_aspace(uint64_t phys, int map_gui_fb)
{
    /* Nur die Slot-Reservierung unter Lock + maskierten IRQs (kurz; verhindert, dass ein
     * gleichzeitiger Free auf einem anderen Kern oder ein preemptierender Task denselben
     * Slot vergibt). Tabellenaufbau danach lock-frei -- der Slot gehoert exklusiv uns. */
    int s = -1;
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&aspace_lock);
    for (int i = 0; i < MMU_MAX_ASPACE; i++) {
        if (!aspace_used[i]) { s = i; aspace_used[i] = 1; break; }
    }
    spin_unlock(&aspace_lock);
    WRITE_SYSREG(daif, f);
    if (s < 0) {
        return -1;
    }

    for (int i = 0; i < 512; i++) {
        aspace_l1[s][i] = l1_table[i];        /* Kernel 1..4 GiB (global) */
        aspace_l2[s][i] = l2_table[i];        /* Kernel 0..1 GiB (global) */
    }
    /* L1[0] dieses Prozesses auf SEINE L2 zeigen lassen. */
    aspace_l1[s][0] = (uint64_t)(uintptr_t)&aspace_l2[s][0] | DESC_TABLE;

    /* User-Kachel: EL0 RW+exec, PXN (Kernel fuehrt User nicht aus), nG (ASID-getaggt). */
    const uint64_t u2m = DESC_VALID_BLOCK | DESC_ATTR(ATTR_IDX_NORMAL) |
                         DESC_AP_RW_ALL | DESC_SH_INNER | DESC_AF | DESC_PXN | DESC_NG;
    uint32_t idx = (uint32_t)((USER_BASE >> 21) & 0x1FF);
    for (uint32_t b = 0; b < (uint32_t)(USER_SIZE >> 21); b++) {   /* USER_SIZE/2MiB Bloecke */
        aspace_l2[s][idx + b] = ((phys + (uint64_t)b * 0x200000UL) & ~0x1FFFFFUL) | u2m;
    }

    /* GUI-Grafik-Bruecke (T2.1): NUR fuer Prozesse mit USER_CAP_GUI (map_gui_fb) den 2-MiB-aligned
     * Backbuffer an die feste EL0-VA GUI_FB_USER_VA mappen -- RW+UXN (reine Daten), nG. So bekommt
     * nur die GUI-App das Fenster (kein Cross-App-Screen-Leak/-Spoofing). Der L2-Eintrag ist im
     * Kernel-Aspace ausgehoehlt -> kein global+nG-Konflikt; der Kernel nutzt diese VA/phys nie. */
    uint64_t bbphys = gui_fb_phys();
    if (map_gui_fb && bbphys) {
        const uint64_t fbm = DESC_VALID_BLOCK | DESC_ATTR(ATTR_IDX_NORMAL) |
                             DESC_AP_RW_ALL | DESC_SH_INNER | DESC_AF | DESC_PXN | DESC_UXN | DESC_NG;
        uint32_t fbidx = (uint32_t)((GUI_FB_USER_VA >> 21) & 0x1FF);
        /* GUI_BB_TILES aufeinanderfolgende 2-MiB-Kacheln (1 bei 640x480, 5 bei -DFULLHD/1080p). */
        for (uint32_t t = 0; t < GUI_BB_TILES; t++) {
            aspace_l2[s][fbidx + t] = ((bbphys + (uint64_t)t * 0x200000UL) & ~0x1FFFFFUL) | fbm;
        }
    }

    aspace_clean(s);
    return s;
}

/* TTBR0-Wert eines Aspace (Tabellen-Basis + ASID = id+1 in [63:48]). */
uint64_t mmu_aspace_ttbr(int aspace)
{
    if (aspace < 0 || aspace >= MMU_MAX_ASPACE) {
        return mmu_kernel_ttbr();
    }
    return (uint64_t)(uintptr_t)&aspace_l1[aspace][0] |
           ((uint64_t)(aspace + 1) << 48);       /* ASID 1..MMU_MAX_ASPACE */
}

/* Aspace freigeben: dessen ASID broadcast-invalidieren (vor Reuse, alle Kerne). */
void mmu_free_aspace(int aspace)
{
    if (aspace < 0 || aspace >= MMU_MAX_ASPACE) {
        return;
    }
    uint64_t asid = (uint64_t)(aspace + 1) << 48;
    __asm__ volatile("dsb ish; tlbi aside1is, %0; dsb ish; isb" :: "r"(asid) : "memory");
    /* Slot freigeben unter Lock + maskierten IRQs (laeuft auf dem Ziel-Kern, parallel zu
     * einer evtl. Allokation auf Kern 0). */
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&aspace_lock);
    aspace_used[aspace] = 0;
    spin_unlock(&aspace_lock);
    WRITE_SYSREG(daif, f);
}

/* TTBR0_EL1 auf 'ttbr' setzen (inkl. ASID). Kein TLB-Flush noetig: die User-Kachel
 * ist nG/ASID-getaggt, die Kernel-Seiten sind global -> beim Wechsel zwischen
 * Adressraeumen bleiben die jeweils gueltigen Eintraege erhalten. */
void mmu_switch(uint64_t ttbr)
{
    WRITE_SYSREG(ttbr0_el1, ttbr);
    isb();
}
