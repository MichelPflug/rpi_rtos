/*
 * drivers/timer/timer.c  --  ARM Generic Timer (CNTP, EL1 physical), SMP-faehig
 *
 * Jeder Kern besitzt sein EIGENES CNTP (CNTP_CVAL/CTL) und seinen eigenen PPI 30.
 * Core 0 legt via timer_init() das gemeinsame Tick-Intervall fest und armt seinen
 * Timer; jeder Sekundaerkern armt seinen Timer via timer_init_secondary(). timer_irq()
 * fuehrt eine PER-CORE-Deadline + einen PER-CORE-Tickzaehler; die globale Tickzahl
 * g_ticks (Zeitbasis fuer den Scheduler) wird ausschliesslich von Core 0 gefuehrt.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "gic.h"
#include "timer.h"

#define TIMER_CORES 4

static uint64_t          g_interval;                 /* Ticks pro Periode (global) */
static uint32_t          g_hz;
static volatile uint64_t g_ticks;                    /* globale Zeitbasis (Core 0) */
static uint64_t          g_deadline[TIMER_CORES];    /* naechste CNTP-Deadline je Kern */
static volatile uint64_t g_core_ticks[TIMER_CORES];  /* Timer-IRQs je Kern */

/* CNTP des AUFRUFENDEN Kerns auf die naechste Periode armen + PPI 30 aktivieren. */
static void timer_arm(void)
{
    uint32_t cid = cpu_id();
    /* PPI 30 ist in GICv2 per-core gebankt -> jeder Kern aktiviert ihn fuer sich. */
    gic_enable_irq(TIMER_PPI_INTID, 0xA0);
    if (cid < TIMER_CORES) {
        g_deadline[cid] = READ_SYSREG(cntpct_el0) + g_interval;
        WRITE_SYSREG(cntp_cval_el0, g_deadline[cid]);
    }
    WRITE_SYSREG(cntp_ctl_el0, 1UL);     /* ENABLE, IMASK=0 */
    isb();
}

void timer_init(uint32_t hz)
{
    if (hz == 0) {
        hz = 1;
    }
    uint64_t freq = READ_SYSREG(cntfrq_el0);
    if (freq == 0) {
        freq = 54000000;                 /* BCM2711-Default */
    }
    g_hz       = hz;
    g_interval = freq / hz;
    g_ticks    = 0;

    uart_puts("    CNTFRQ_EL0 = ");
    uart_putdec(freq);
    uart_puts(" Hz, Intervall = ");
    uart_putdec(g_interval);
    uart_puts(" Ticks (");
    uart_putdec(hz);
    uart_puts(" Hz)\n");

    timer_arm();                         /* Core 0 */
}

void timer_init_secondary(void)
{
    timer_arm();                         /* nutzt das von timer_init gesetzte g_interval */
}

void timer_irq(void)
{
    uint32_t cid = cpu_id();
    if (cid < TIMER_CORES) {
        g_deadline[cid] += g_interval;
        WRITE_SYSREG(cntp_cval_el0, g_deadline[cid]);
        isb();                           /* CVAL-Write (Leitung deassert) vor EOIR sichtbar */
        g_core_ticks[cid]++;
    }
    if (cid == 0) {
        g_ticks++;                       /* globale Zeitbasis nur von Core 0 */
    }
}

uint64_t timer_ticks(void)
{
    return g_ticks;
}

uint64_t timer_core_ticks(uint32_t cid)
{
    return (cid < TIMER_CORES) ? g_core_ticks[cid] : 0;
}
