/*
 * arch/aarch64/exceptions.c  --  VBAR-Setup + C-Dispatcher
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "fbcon.h"      /* fbcon_putc: Panik-Dump auf den Schirm zurueckholen (nach GUI-fbcon-Handoff) */
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "sched.h"
#include "syscall.h"
#include "dwc2.h"
#include "gui_input.h"
#include "spinlock.h"

extern char vectors[];   /* Symbol aus vectors.S */

void exceptions_init(void)
{
    WRITE_SYSREG(vbar_el1, (uint64_t)vectors);
    isb();
}

/* Panik-Zustand: gesetzt, sobald ein Kern panic() gewinnt. GLOBAL (auch von uart.c geprueft),
 * um die Normalausgabe noch laufender Kerne waehrend des Panik-Dumps zu unterdruecken -> der
 * Register-Dump bleibt lesbar. Nur EIN Kern dumpt + broadcastet, weitere halten direkt. */
volatile int g_panicking;
/* Serialisiert die Panik-Ausgabe (Haupt-Dump + Halt-Meldungen der Sekundaerkerne), damit sie
 * sich nicht zeichenweise verschraenken. Eigener Lock, NICHT der UART-Lock -> kein Self-Deadlock,
 * falls der fehlerhafte Kern den UART-Lock hielt (dieser Lock wird nie im Fehlerpfad gehalten). */
static spinlock_t s_panic_out = SPINLOCK_INIT;
/* Anzahl ANDERER Kerne, die den Halt-SGI bestaetigt haben (fuer die best-effort-Garantie). */
static volatile int s_halt_ack;

/* Kontrolliertes System-Panik bei nicht behebbarem Kernel-Fehler: lock-freie Diagnose
 * ausgeben, ALLE anderen Kerne per Halt-SGI stoppen, selbst dauerhaft halten. Kehrt nie
 * zurueck. Ersetzt das fruehere Ein-Kern-`b .`, das die anderen Kerne weiterlaufen liess
 * (System halb-tot, schwer diagnostizierbar). NCORES=4 (vgl. sched.c). */
void panic(const char *msg)
{
    irq_disable();
    if (__atomic_exchange_n(&g_panicking, 1, __ATOMIC_SEQ_CST)) {
        for (;;) { wfe(); }              /* ein anderer Kern paniced bereits -> nur halten */
    }
    /* Falls die GUI-Sitzung den fbcon-Mirror abgeschaltet hat (T2.6-Handoff): fuer den Panik-Dump
     * zurueckholen, damit ein Kern-0-Crash auch auf HDMI sichtbar wird (putc_raw spiegelt nur von
     * Kern 0). Auf HW ohne serielle Konsole sonst unsichtbar. */
    uart_set_mirror(fbcon_putc);
    uint64_t esr = READ_SYSREG(esr_el1);
    uint64_t elr = READ_SYSREG(elr_el1);
    uint64_t far = READ_SYSREG(far_el1);
    uint32_t cid = cpu_id();
    char cd[2] = { (char)('0' + (cid % 10)), 0 };

    spin_lock(&s_panic_out);
    uart_panic_puts("\n\n*** KERNEL PANIC (Kern ");
    uart_panic_puts(cd);
    uart_panic_puts(") ***\n  Grund   : ");
    uart_panic_puts(msg ? msg : "(unbekannt)");
    uart_panic_puts("\n  ESR_EL1 = "); uart_panic_hex(esr);
    uart_panic_puts("\n  ELR_EL1 = "); uart_panic_hex(elr);
    uart_panic_puts("\n  FAR_EL1 = "); uart_panic_hex(far);
    uart_panic_puts("\n  Stoppe alle Kerne...\n");
    for (uint32_t c = 0; c < 4; c++) {
        if (c != cid) { gic_send_sgi(SGI_HALT, c); }
    }
    spin_unlock(&s_panic_out);       /* Halt-Meldungen der Sekundaerkerne koennen nun folgen */

    /* Best-effort auf die Halt-Bestaetigung der anderen 3 Kerne warten (begrenzt). Ein Kern,
     * der mit maskierten IRQs haengt (Deadlock), nimmt den SGI NIE an -> ehrlich als
     * "reagiert nicht" melden, statt faelschlich "alle gestoppt" zu behaupten. Kerne mit
     * freien IRQs bestaetigen praktisch sofort -> die Schleife bricht dann frueh ab. */
    for (volatile uint64_t s = 0; s < 100000000ULL && s_halt_ack < 3; s++) { }
    spin_lock(&s_panic_out);
    uart_panic_puts(s_halt_ack >= 3 ? "  Alle Kerne gestoppt.\n"
                                    : "  WARNUNG: nicht alle Kerne reagierten (Halt-Timeout).\n");
    spin_unlock(&s_panic_out);
    for (;;) { wfe(); }
}

/* Unbehandelte/unerwartete Exception (u.a. EL1-Sync-Fault = Kernel-Bug, FIQ, SError):
 * nicht behebbar -> System-Panik (alle Kerne stoppen), statt nur diesen Kern zu haengen. */
void c_exception_handler(uint64_t vector_index)
{
    char msg[48];       /* lokal: kein geteilter Puffer -> keine Race bei Simultan-Fault */
    const char *pfx = "Unerwartete Exception, Vektor-Index ";
    int p = 0;
    for (const char *s = pfx; *s; s++) { msg[p++] = *s; }
    uint32_t vi = (uint32_t)vector_index;
    if (vi >= 10) { msg[p++] = (char)('0' + vi / 10); }
    msg[p++] = (char)('0' + vi % 10);
    msg[p]   = 0;
    panic(msg);
}

#ifdef HW_FIXUP
/* --- HW-Bringup-Fault-Fixup (nur #ifdef HW_FIXUP, impliziert von -PcieProbe) ---------------
 * Erlaubt, EINEN EL1-Data/Instruction-Abort (z.B. externer Abort bei einem noch nicht
 * funktionierenden Peripherie-Zugriff wie PCIe/VL805) zu TOLERIEREN statt zu paniken: die
 * faultende Instruktion wird uebersprungen (ELR+4). So laesst sich HW-Bringup KABELLOS iterieren,
 * ohne dass ein Fehlzugriff den Boot killt. Default 0 -> auch im HW_FIXUP-Build inert, bis eine
 * Probe das Flag setzt. OHNE das Flag ist dieser Pfad praeprozessor-leer und vectors.S nutzt den
 * unveraenderten INVALID-4-Pfad -> der RC/Vk-Kernel bleibt BYTE-IDENTISCH. */
volatile int g_fault_fixup;   /* 1 = EL1-Abort tolerieren (ELR+4) */
volatile int g_fault_hit;     /* wird bei jedem tolerierten Abort gesetzt (Aufrufer prueft/nullt) */

/* EL1-Sync-Exception (aus vectors.S: el1_spx_sync). frame = Trap-Frame (uint64_t[34]); ELR @ frame[31]. */
void c_el1_sync_handler(uint64_t *frame)
{
    uint64_t esr = READ_SYSREG(esr_el1);
    uint32_t ec  = (uint32_t)((esr >> 26) & 0x3F);
    /* EC 0x25 = Data Abort (same EL), 0x21 = Instruction Abort (same EL). */
    if (g_fault_fixup && (ec == 0x25 || ec == 0x21)) {
        g_fault_hit = 1;
        frame[31] += 4;           /* gesichertes ELR_EL1 -> Fault-Instruktion ueberspringen */
        return;                   /* restore_regs + eret (in vectors.S) setzen fort */
    }
    panic("EL1-Sync-Fault (unerwartet / Kernel-Bug)");
}
#endif /* HW_FIXUP */

/* Synchrone Exception aus EL0: SVC -> Syscall, sonst User-Fault (Task beenden). */
void c_sync_handler(uint64_t *frame)
{
    /* Safe-Point: ein per SYS_KILL markierter EL0-Prozess beendet sich hier (EL0->EL1-Grenze,
     * kein Kernel-Lock gehalten), bevor der Syscall/Fault bearbeitet wird -- kehrt nie zurueck. */
    sched_exit_if_killed();

    uint64_t esr = READ_SYSREG(esr_el1);
    uint32_t ec  = (uint32_t)((esr >> 26) & 0x3F);

    if (ec == 0x15) {                 /* SVC aus AArch64 */
        syscall_dispatch(frame);
        return;
    }

    uart_puts("\n*** EL0-Fault (EC = ");
    uart_puthex(ec);
    uart_puts(", ESR = ");
    uart_puthex(esr);
    uart_puts(")\n  ELR_EL1 = ");
    uart_puthex(READ_SYSREG(elr_el1));
    uart_puts("\n  FAR_EL1 = ");
    uart_puthex(READ_SYSREG(far_el1));
    uart_puts("\n  User-Task wird beendet.\n");
    task_exit(TASK_EXIT_FAULT);       /* nur den Task beenden, nicht den Kernel */
}

/* IRQ-Dispatch ueber das GIC-CPU-Interface. */
void c_irq_handler(void)
{
    /* Herkunft (EL0/EL1) VOR jedem moeglichen Reschedule festhalten: SPSR_EL1.M[3:0]==0 -> der
     * IRQ unterbrach EL0-Code (Task haelt keinen Kernel-Lock -> Safe-Point fuer einen Kill). */
    int from_el0 = ((READ_SYSREG(spsr_el1) & 0xF) == 0);

    uint32_t iar   = gic_acknowledge_irq();
    uint32_t intid = iar & 0x3FF;

    if (intid >= 1020) {
        /* 1020..1023 = spurious / kein anstehender IRQ: kein EOI noetig. */
        return;
    }

    uint32_t cid = cpu_id();

    if (intid == SGI_HALT) {
        /* Ein anderer Kern paniced -> diesen Kern sofort und dauerhaft anhalten. Lock-freie
         * Ausgabe (der panickende Kern koennte den UART-Lock halten). Kein EOI/Reschedule mehr. */
        irq_disable();
        char cd[2] = { (char)('0' + (cid % 10)), 0 };
        spin_lock(&s_panic_out);
        uart_panic_puts("[panic] Kern ");
        uart_panic_puts(cd);
        uart_panic_puts(" gestoppt\n");
        spin_unlock(&s_panic_out);
        __atomic_fetch_add(&s_halt_ack, 1, __ATOMIC_SEQ_CST);   /* Halt bestaetigen */
        for (;;) { wfe(); }
    }

    if (intid == TIMER_PPI_INTID) {
        timer_irq();                  /* per-core Deadline + Tickzaehler */
        if (cid == 0) {
            dwc2_kbd_tick();          /* USB-HID-Kanal getaktet neu armen (nur Kern 0) */
            gui_input_tick();         /* T2.3: Maus pollen -> GUI-Event-Queue (nur Kern 0) */
        }
        sched_tick();                 /* per-core Preemption (jeder Kern schedulet sich selbst) */
    } else if (intid == SGI_RESCHED) {
        sched_ipi_received();         /* Reschedule-IPI: das eigentliche Umplanen macht
                                       * schedule_if_needed() nach dem EOI (need_resched ist
                                       * vom weckenden Kern bereits gesetzt) */
    } else if (intid == DWC2_IRQ_SPI) {
        dwc2_irq();                   /* USB: Host-Channel-Halt + Hot-Plug (SPI -> Core 0) */
    } else {
        uart_puts("[IRQ] unerwartete INTID = ");
        uart_putdec(intid);
        uart_puts("\n");
    }

    gic_end_irq(iar);

    /* Preemptionspunkt nach dem EOI (IRQs sind hier maskiert), pro Kern. */
    schedule_if_needed();

    /* Safe-Point: unterbrach dieser IRQ EL0-Code, beendet sich ein per SYS_KILL markierter
     * Prozess jetzt (kehrt nie zurueck). So wird auch eine EL0-Endlosschleife ohne Syscall
     * spaetestens beim naechsten Timer-Tick gekillt. */
    if (from_el0) {
        sched_exit_if_killed();
    }
}
