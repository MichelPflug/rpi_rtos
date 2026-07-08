/*
 * arch/aarch64/vectors.S  --  AArch64 Exception-Vektortabelle (VBAR_EL1)
 *
 * 16 Eintraege a 128 Byte, Tabelle auf 2 KiB ausgerichtet. Wir laufen auf
 * EL1h (SP_EL1), daher sind die "Current EL, SPx"-Eintraege (Index 4..7)
 * relevant: Index 4 = Sync (Faults), Index 5 = IRQ (Timer).
 *
 * Trap-Frame: 272 Byte, speichert x0..x30 sowie ELR_EL1/SPSR_EL1.
 */

.section ".text"

/* ----- Register sichern / wiederherstellen ----- */
.macro save_regs
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #16*0]
    stp     x2,  x3,  [sp, #16*1]
    stp     x4,  x5,  [sp, #16*2]
    stp     x6,  x7,  [sp, #16*3]
    stp     x8,  x9,  [sp, #16*4]
    stp     x10, x11, [sp, #16*5]
    stp     x12, x13, [sp, #16*6]
    stp     x14, x15, [sp, #16*7]
    stp     x16, x17, [sp, #16*8]
    stp     x18, x19, [sp, #16*9]
    stp     x20, x21, [sp, #16*10]
    stp     x22, x23, [sp, #16*11]
    stp     x24, x25, [sp, #16*12]
    stp     x26, x27, [sp, #16*13]
    stp     x28, x29, [sp, #16*14]
    mrs     x0, elr_el1
    stp     x30, x0,  [sp, #16*15]    /* x30 @240, ELR_EL1 @248 */
    mrs     x0, spsr_el1
    mrs     x1, sp_el0
    stp     x0, x1,   [sp, #16*16]    /* SPSR_EL1 @256, SP_EL0 @264 */
.endm

.macro restore_regs
    ldp     x30, x0,  [sp, #16*15]    /* x30, ELR_EL1 */
    msr     elr_el1, x0
    ldp     x0,  x1,  [sp, #16*16]    /* SPSR_EL1, SP_EL0 */
    msr     spsr_el1, x0
    msr     sp_el0, x1
    ldp     x0,  x1,  [sp, #16*0]
    ldp     x2,  x3,  [sp, #16*1]
    ldp     x4,  x5,  [sp, #16*2]
    ldp     x6,  x7,  [sp, #16*3]
    ldp     x8,  x9,  [sp, #16*4]
    ldp     x10, x11, [sp, #16*5]
    ldp     x12, x13, [sp, #16*6]
    ldp     x14, x15, [sp, #16*7]
    ldp     x16, x17, [sp, #16*8]
    ldp     x18, x19, [sp, #16*9]
    ldp     x20, x21, [sp, #16*10]
    ldp     x22, x23, [sp, #16*11]
    ldp     x24, x25, [sp, #16*12]
    ldp     x26, x27, [sp, #16*13]
    ldp     x28, x29, [sp, #16*14]
    add     sp, sp, #272
.endm

/* Ein Vektor-Eintrag: 128-Byte-ausgerichtet, springt zum Handler. */
.macro VENTRY label
    .balign 128
    b       \label
.endm

/* Nicht erwartete Exception: Frame sichern, Index melden, anhalten. */
.macro INVALID index
    save_regs
    mov     x0, #\index
    bl      c_exception_handler
    b       .
.endm

/* ----- Vektortabelle ----- */
.balign 2048
.global vectors
vectors:
    /* Current EL mit SP0 (Index 0..3) */
    VENTRY  inv_0
    VENTRY  inv_1
    VENTRY  inv_2
    VENTRY  inv_3
    /* Current EL mit SPx (Index 4..7) */
    VENTRY  el1_spx_sync     /* 4: Sync/Faults */
    VENTRY  el1_spx_irq      /* 5: IRQ (Timer) */
    VENTRY  inv_6            /* 6: FIQ */
    VENTRY  inv_7            /* 7: SError */
    /* Lower EL, AArch64 (Index 8..11) */
    VENTRY  el0_sync         /* 8: Sync aus EL0 -> Syscall (SVC) / User-Fault */
    VENTRY  el1_spx_irq      /* 9: IRQ aus EL0 -> gleicher IRQ-Pfad (Preemption) */
    VENTRY  inv_10
    VENTRY  inv_11
    /* Lower EL, AArch32 (Index 12..15) */
    VENTRY  inv_12
    VENTRY  inv_13
    VENTRY  inv_14
    VENTRY  inv_15

/* ----- Handler ----- */
el1_spx_irq:
    save_regs
    bl      c_irq_handler
    restore_regs
    eret

/* Sync aus EL0: Frame sichern, Zeiger uebergeben, dispatchen (Syscall/Fault). */
el0_sync:
    save_regs
    mov     x0, sp
    bl      c_sync_handler
    restore_regs
    eret

#ifdef HW_FIXUP
/* EL1-Sync (Faults im Kernel), HW-Bringup-Variante (nur #ifdef HW_FIXUP): Frame sichern, C-Handler
 * (Fixup-faehig), zurueck. Ein echter Kernel-Bug paniced im Handler (kehrt nie zurueck); ein
 * tolerierter HW-Bringup-Abort ueberspringt die Fault-Instruktion (ELR+4 im Frame) -> restore_regs/
 * eret setzen fort. OHNE das Flag greift der INVALID-4-Pfad unten -> RC/Vk-Kernel byte-identisch. */
el1_spx_sync:
    save_regs
    mov     x0, sp
    bl      c_el1_sync_handler
    restore_regs
    eret
#else
el1_spx_sync: INVALID 4
#endif

inv_0:  INVALID 0
inv_1:  INVALID 1
inv_2:  INVALID 2
inv_3:  INVALID 3
inv_6:  INVALID 6
inv_7:  INVALID 7
inv_10: INVALID 10
inv_11: INVALID 11
inv_12: INVALID 12
inv_13: INVALID 13
inv_14: INVALID 14
inv_15: INVALID 15
