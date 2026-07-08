/*
 * drivers/mailbox/mailbox.c  --  ARM<->VideoCore Mailbox (Property-Channel 8)
 *
 * Mailbox-Block im Low-Peripheral-Modus bei 0xFE00B880. Der Property-Puffer liegt
 * im RAM (cacheable, Write-Back) -> vor dem Senden Cache cleanen, nach der Antwort
 * invalidieren (in QEMU kohaerent, auf echter HW noetig). Die VideoCore erwartet
 * eine BUS-Adresse; wir verwenden den uncached-Alias 0xC0000000.
 */
#include <stdint.h>
#include "mmio.h"
#include "aarch64.h"
#include "spinlock.h"
#include "mailbox.h"

#define MBOX_BASE     0xFE00B880UL
#define MBOX_READ     (MBOX_BASE + 0x00)   /* Mailbox 0: VC->ARM (Lese-FIFO) */
#define MBOX_STATUS0  (MBOX_BASE + 0x18)   /* Status von Mailbox 0 (EMPTY fuer Read) */
#define MBOX_WRITE    (MBOX_BASE + 0x20)   /* Mailbox 1: ARM->VC (Schreib-FIFO) */
#define MBOX_STATUS1  (MBOX_BASE + 0x38)   /* Status von Mailbox 1 (FULL fuer Write) */
#define MBOX_FULL     0x80000000u
#define MBOX_EMPTY    0x40000000u
#define MBOX_CH_PROP  8u
#define MBOX_TIMEOUT  0x10000000u          /* obere Spin-Schranke gegen Dauer-Hang */

#define CACHE_LINE   64
#define MBOX_NCORES  4                     /* BCM2711: 4 Kerne (vgl. sched.c NCORES) */

/* Transaktions-Lock: serialisiert ueber ALLE Kerne. IRQ-maskiert (aus preemptierbarem
 * Kontext genommen -> sonst koennte ein preemptierter Halter auf demselben Kern mit dem
 * naechsten Task verklemmen). Leaf-Lock: der Transaktionspfad nimmt kein weiteres Lock. */
static spinlock_t s_mboxlock = SPINLOCK_INIT;

static uint64_t mbox_lock(void)
{
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_mboxlock);
    return f;
}
static void mbox_unlock(uint64_t f)
{
    spin_unlock(&s_mboxlock);
    WRITE_SYSREG(daif, f);
}

#ifdef RTOS_SELFTEST
/* T1.9-Guardian-Instrumentierung: zaehlt, wie viele Kerne GLEICHZEITIG in der Transaktion
 * sind. Mit dem Lock (+ IRQ-Maske) ist das IMMER 1. Fehlt das Lock (Mutationstest), koennen
 * sich zwei Kerne ueberlappen -> occ>1 -> Verletzungs-Latch. Nur im Selbsttest kompiliert. */
static volatile uint32_t s_mbox_occ;       /* aktuell in der Transaktion befindliche Kerne */
static volatile uint32_t s_mbox_occ_max;   /* je beobachtetes Maximum */
static volatile uint32_t s_mbox_occ_viol;  /* Latch: je >1 beobachtet? (0=nie) */

static void mbox_occ_sample(uint32_t o)
{
    if (o > s_mbox_occ_max) { s_mbox_occ_max = o; }
    if (o > 1u)             { s_mbox_occ_viol = 1u; }
}
static void mbox_occ_enter(void)
{
    mbox_occ_sample(++s_mbox_occ);
    /* Fenster kuenstlich weiten: ein FEHLENDES Lock (Mutation) fuehrt so zuverlaessig zu
     * ueberlappenden Transaktionen zweier Kerne (occ>1); MIT Lock bleibt occ==1. */
    for (volatile int w = 0; w < 4000; w++) {
        mbox_occ_sample(s_mbox_occ);
    }
}
static void mbox_occ_leave(void)
{
    --s_mbox_occ;
}
uint32_t mailbox_occ_max(void)       { return s_mbox_occ_max; }
uint32_t mailbox_occ_violation(void) { return s_mbox_occ_viol; }
#endif /* RTOS_SELFTEST */

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

/* Die eigentliche Transaktion. Erwartet, dass der Aufrufer s_mboxlock haelt. */
static int mailbox_property_locked(volatile uint32_t *buf)
{
    uint32_t size = buf[0];
    cache_clean((const void *)buf, size);

    /* Bus-Adresse (uncached VC-Alias) + Kanal in den unteren 4 Bit. */
    uint32_t addr = (uint32_t)((uintptr_t)buf & 0x3FFFFFFFu) | 0xC0000000u;

    /* Beschraenkte Warteschleifen: gibt -1 zurueck, statt bei nicht antwortender
     * VideoCore (z.B. Fehlkonfiguration auf echter HW) dauerhaft zu haengen. */
    uint32_t spin;

    /* Vor dem Schreiben in MBOX1_WRITE auf FULL von MBOX1_STATUS warten (nicht
     * MBOX0_STATUS -- dessen FULL-Bit beschreibt das Lese-FIFO, nicht das Schreib-FIFO). */
    spin = MBOX_TIMEOUT;
    while (mmio_read32(MBOX_STATUS1) & MBOX_FULL) {
        if (--spin == 0) {
            return -1;
        }
    }
    mmio_write32(MBOX_WRITE, (addr & ~0xFu) | MBOX_CH_PROP);

    spin = MBOX_TIMEOUT;
    for (;;) {
        /* Spin-Guard PRO ITERATION dekrementieren -- gilt damit fuer BEIDE Faelle: warten
         * auf eine Antwort (FIFO leer) UND das Verwerfen einer Fremd-Kanal-Antwort weiter
         * unten. Frueher stand '--spin' nur im EMPTY-Zweig -> ein dauerhaft nicht-leeres
         * Lese-FIFO mit Nicht-Kanal-8-Woertern (defekte/fremd bespielte VideoCore) lief ewig. */
        if (--spin == 0) {
            return -1;
        }
        if (mmio_read32(MBOX_STATUS0) & MBOX_EMPTY) {
            continue;                       /* noch keine Antwort -> weiter warten (bis Timeout) */
        }
        uint32_t r = mmio_read32(MBOX_READ);
        if ((r & 0xFu) == MBOX_CH_PROP) {
            cache_inval((void *)buf, size);
            return (buf[1] == 0x80000000u) ? 0 : -1;
        }
        /* Fremd-Kanal (untere 4 Bit != MBOX_CH_PROP): Wort verwerfen, naechste Iteration
         * (der Guard oben begrenzt auch diesen Pfad). */
    }
}

int mailbox_property(volatile uint32_t *buf)
{
    uint64_t f = mbox_lock();
#ifdef RTOS_SELFTEST
    mbox_occ_enter();
#endif
    int r = mailbox_property_locked(buf);
#ifdef RTOS_SELFTEST
    mbox_occ_leave();
#endif
    mbox_unlock(f);
    return r;
}

/* Je Kern ein eigener, vollstaendig 64-B-cache-line-isolierter Property-Puffer: der Puffer
 * wird VOR dem Lock (in mailbox_property) befuellt und per dc cvac/ivac gewartet -> zwei
 * Kerne duerfen sich weder den Puffer noch dessen Cache-Line teilen (sonst verwirft ivac
 * des einen die frisch geschriebenen Bytes des anderen). 16 Woerter = genau eine Cache-Line. */
static volatile uint32_t s_fwbuf[MBOX_NCORES][16] __attribute__((aligned(64)));

int mailbox_get_arm_memory(uint32_t *base, uint32_t *size)
{
    uint32_t c = cpu_id();
    if (c >= MBOX_NCORES) {
        c = 0;
    }
    volatile uint32_t *mb = s_fwbuf[c];
    mb[0] = 8u * 4u;           /* Gesamtgroesse (8 Woerter) */
    mb[1] = 0u;                /* Request */
    mb[2] = 0x00010005u;       /* Tag: Get ARM memory (base + size) */
    mb[3] = 8u;                /* Value-Puffer 8 Byte (base, size) */
    mb[4] = 0u;                /* Request-Laenge */
    mb[5] = 0u;                /* Response: base */
    mb[6] = 0u;                /* Response: size */
    mb[7] = 0u;                /* End-Tag */
    if (mailbox_property(mb) != 0) {
        return -1;
    }
    if (base) { *base = mb[5]; }
    if (size) { *size = mb[6]; }
    return 0;
}

int mailbox_get_vc_memory(uint32_t *base, uint32_t *size)
{
    uint32_t c = cpu_id();
    if (c >= MBOX_NCORES) {
        c = 0;
    }
    volatile uint32_t *mb = s_fwbuf[c];
    mb[0] = 8u * 4u;
    mb[1] = 0u;
    mb[2] = 0x00010006u;       /* Tag: Get VC memory (base + size) */
    mb[3] = 8u;
    mb[4] = 0u;
    mb[5] = 0u;                /* Response: base */
    mb[6] = 0u;                /* Response: size */
    mb[7] = 0u;
    if (mailbox_property(mb) != 0) {
        return -1;
    }
    if (base) { *base = mb[5]; }
    if (size) { *size = mb[6]; }
    return 0;
}

int mailbox_get_fw_rev(uint32_t *out)
{
    uint32_t c = cpu_id();
    if (c >= MBOX_NCORES) {
        c = 0;
    }
    volatile uint32_t *mb = s_fwbuf[c];
    mb[0] = 7u * 4u;           /* Gesamtgroesse (7 genutzte Woerter) */
    mb[1] = 0u;                /* Request */
    mb[2] = 0x00000001u;       /* Tag: Get firmware revision */
    mb[3] = 4u;                /* Value-Puffer 4 Byte */
    mb[4] = 0u;                /* Request-Laenge (Response schreibt Laenge zurueck) */
    mb[5] = 0u;                /* Value (Response-Revision) */
    mb[6] = 0u;                /* End-Tag */
    if (mailbox_property(mb) != 0) {
        return -1;
    }
    if (out) {
        *out = mb[5];
    }
    return 0;
}
