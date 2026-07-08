/*
 * arch/aarch64/start.S  --  rpi_rtos Boot-Code (SMP-faehig)
 *
 * QEMU raspi4b (und die Pi-Firmware) starten NUR Core 0 am Kernel-Eintritt
 * (0x80000); die Sekundaerkerne (1..3) haengen im SPIN-TABLE: jeder Kern spinnt
 * mit wfe und liest seine Release-Adresse 0xd8 + 8*core; ist sie != 0, springt er
 * dorthin (br). Core 0 weckt sie, indem es die Adresse von _secondary_entry in
 * 0xE0/0xE8/0xF0 schreibt und sev() ausfuehrt (siehe kernel/smp.c).
 *
 *   1) Primaer-Core: EL-Drop nach EL1, Stack/BSS, kmain().
 *   2) Sekundaerkern (via Spin-Table -> _secondary_entry): EL-Drop, per-core
 *      Stack, secondary_main(core_id).
 */

.section ".text.boot"
.global _start
.global _secondary_entry

_start:
    /* DTB-Zeiger (vom Bootloader/QEMU in x0 uebergeben) SOFORT retten -- die mpidr-Pruefung
     * unten klobbert x0. x20 ist callee-saved und ueberlebt eret + init_sctlr_el1 (nur x0/x30).
     * Wird in el1_entry nach dem BSS-Nullen in g_dtb_ptr geschrieben (T1.12: RAM-Groesse aus /memory). */
    mov     x20, x0
    /* Nur Core 0 (Aff0 == 0) laeuft hier weiter. Ein Sekundaerkern, der wider
     * Erwarten doch hier ankaeme (anderes Boot-Modell), parkt sicher. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbz     x0, 1f
0:
    wfe
    b       0b
1:
    /* Entry-EL fuer Diagnose in x19 merken (ueberlebt die erets). */
    mrs     x19, CurrentEL
    lsr     x19, x19, #2

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.eq    from_el3
    cmp     x0, #2
    b.eq    from_el2
    bl      init_sctlr_el1
    b       el1_entry

from_el3:
    mov     x0, #0x431             /* SCR_EL3: NS | RW | RES1 */
    msr     scr_el3, x0
    mov     x0, #0x3c9             /* SPSR_EL3: EL2h + DAIF=1111 */
    msr     spsr_el3, x0
    adr     x0, from_el2
    msr     elr_el3, x0
    eret

from_el2:
    mrs     x0, cnthctl_el2
    orr     x0, x0, #0x3           /* EL1PCTEN | EL1PCEN */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #(1 << 31)         /* HCR_EL2.RW = 1 */
    msr     hcr_el2, x0
    bl      init_sctlr_el1
    mov     x0, #0x3c5             /* SPSR_EL2: EL1h + DAIF=1111 */
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

/* Sicherer SCTLR_EL1-Grundwert (MMU/Caches aus, RES1-Bits) + deterministische FP/SIMD-Policy.
 * Von JEDEM Kern (primaer + sekundaer, alle Entry-EL-Pfade) durchlaufen. Nur x0/x30. */
init_sctlr_el1:
    ldr     x0, =0x30d00800
    msr     sctlr_el1, x0
    /* CPACR_EL1.FPEN[21:20]: standardmaessig 0b00 -> FP/SIMD trappt bei EL0 UND EL1 (T1.8-Haertung;
     * der context_switch sichert die V-Register NICHT). In GUI-Builds (-DGUI_FP) stattdessen 0b11 ->
     * FP an EL0 erlaubt (Laufzeit-TTF T2.8, 3D/Vulkan Phase 3). Seit T3.1 sichert schedule() den
     * FP/SIMD-Zustand EAGER je Task (fpctx_save/fpctx_restore, arch/aarch64/fpctx.S) -> MEHRERE
     * EL0-Prozesse duerfen gleichzeitig FP nutzen; jeder startet mit genulltem FP-Zustand (kein
     * V-Register-Leak). Der Kernel selbst bleibt -mgeneral-regs-only (einzige Ausnahme: fpctx.S). */
#ifdef GUI_FP
    mov     x0, #(3 << 20)                  /* FPEN = 0b11 -> FP/SIMD an EL0 + EL1 erlaubt */
    msr     cpacr_el1, x0
#else
    msr     cpacr_el1, xzr                  /* FPEN = 0b00 -> FP trappt (RC-Haertung) */
#endif
    isb
    ret

el1_entry:
    ldr     x0, =__stack_top
    mov     sp, x0

    /* BSS nullen: [__bss_start, __bss_end). */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
2:
    cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b
3:
    ldr     x0, =g_entry_el
    str     x19, [x0]
    ldr     x0, =g_dtb_ptr
    str     x20, [x0]

    bl      kmain
hang:
    wfe
    b       hang

/* ===================== Sekundaerkerne (Spin-Table-Einsprung) =====================
 * Core 0 schreibt die Adresse dieses Labels in die Release-Slots; der jeweilige
 * Kern springt hierher (MMU aus, Aff0 = seine Kern-ID). */
_secondary_entry:
    mrs     x9, mpidr_el1
    and     x20, x9, #0xff         /* core_id (1..3) */

    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.eq    sec_from_el3
    cmp     x0, #2
    b.eq    sec_from_el2
    cmp     x0, #1
    b.eq    sec_at_el1
    b       sec_park               /* unerwartetes EL */

sec_from_el3:
    mov     x0, #0x431
    msr     scr_el3, x0
    mov     x0, #0x3c9
    msr     spsr_el3, x0
    adr     x0, sec_from_el2
    msr     elr_el3, x0
    eret

sec_from_el2:
    mrs     x0, cnthctl_el2
    orr     x0, x0, #0x3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0
    bl      init_sctlr_el1
    mov     x0, #0x3c5             /* EL1h + DAIF=1111 */
    msr     spsr_el2, x0
    adr     x0, sec_stack
    msr     elr_el2, x0
    eret

sec_at_el1:
    bl      init_sctlr_el1
    /* fall through */

sec_stack:
    /* Per-core Stack: __stack_top + core_id * 0x4000 (16 KiB je Kern). */
    ldr     x0, =__stack_top
    mov     x1, #0x4000
    madd    x0, x20, x1, x0
    mov     sp, x0
    mov     x0, x20                /* arg: core_id */
    bl      secondary_main

sec_park:
    wfe
    b       sec_park
