/*
 * arch/aarch64/switch.S  --  Kontextwechsel fuer Kernel-Threads (AArch64)
 *
 * context_switch(uint64_t *prev_sp, uint64_t *next_sp):
 *   Sichert die callee-saved Register (x19..x30) des aktuellen C-Kontexts auf
 *   dem aktuellen Stack, schreibt SP nach *prev_sp, laedt SP aus *next_sp und
 *   stellt dessen callee-saved Register wieder her. Das 'ret' kehrt damit in den
 *   Kontext des naechsten Tasks zurueck (entweder eine fruehere context_switch-
 *   Aufrufstelle oder - bei einem neuen Task - task_trampoline).
 *
 * Aufrufkonvention/Invariante: IMMER mit maskierten IRQs aufrufen. DAIF wird
 * NICHT mitgesichert; jeder Wiedereintrittspfad stellt den IRQ-Zustand selbst
 * her (eret bei praeemptierten Tasks, daifclr im Trampolin bei neuen Tasks).
 *
 * Hinweis: FP/SIMD (q0-q31, FPSR/FPCR) wird HIER nicht gesichert. In Builds ohne
 * GUI_FP korrekt (Kernel -mgeneral-regs-only, FP via CPACR_EL1 getrappt). In
 * GUI_FP-Builds (FPEN=0b11, mehrere FP-Prozesse) uebernimmt das schedule() via
 * fpctx_save/fpctx_restore (T3.1, arch/aarch64/fpctx.S) EAGER um diesen Wechsel herum.
 */

.section ".text"

.global context_switch
context_switch:
    sub     sp, sp, #(12*8)
    stp     x19, x20, [sp, #16*0]
    stp     x21, x22, [sp, #16*1]
    stp     x23, x24, [sp, #16*2]
    stp     x25, x26, [sp, #16*3]
    stp     x27, x28, [sp, #16*4]
    stp     x29, x30, [sp, #16*5]
    mov     x2, sp
    str     x2, [x0]            /* *prev_sp = sp */
    ldr     x2, [x1]            /* sp = *next_sp */
    mov     sp, x2
    ldp     x19, x20, [sp, #16*0]
    ldp     x21, x22, [sp, #16*1]
    ldp     x23, x24, [sp, #16*2]
    ldp     x25, x26, [sp, #16*3]
    ldp     x27, x28, [sp, #16*4]
    ldp     x29, x30, [sp, #16*5]
    add     sp, sp, #(12*8)
    ret

/*
 * task_trampoline: Einsprungpunkt eines NEU erzeugten Tasks. Der fabrizierte
 * Kontext liefert x19 = entry, x20 = arg, x30 = task_trampoline.
 */
.global task_trampoline
task_trampoline:
    msr     daifclr, #2         /* IRQs freigeben (Task laeuft praeemptierbar) */
    mov     x0, x20             /* arg */
    blr     x19                 /* entry(arg) */
    mov     x0, #0              /* Exit-Code 0 fuer task_exit(int) */
    bl      task_exit           /* falls entry() zurueckkehrt: Task beenden */
1:
    wfe
    b       1b
