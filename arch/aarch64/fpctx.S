/*
 * arch/aarch64/fpctx.S  --  FP/SIMD-Kontextsicherung (T3.1, nur GUI_FP-Builds)
 *
 * Eager Save/Restore des vollstaendigen FP/SIMD-Zustands eines Tasks:
 * q0..q31 (32 x 16 B = 512 B) + FPCR/FPSR (2 x 8 B) = 528 B je Bereich (16-aligned).
 * Aufruf aus schedule()/sched_enter() (kernel/sched.c) mit maskierten IRQs; der
 * Bereich (fpctx[tid]) gehoert dem OWNER-Kern des Tasks (partitionierter Scheduler,
 * keine Migration) -> kein Cross-Core-Race auf demselben Bereich.
 *
 * NUR unter GUI_FP uebersetzt: dort setzt start.S CPACR_EL1.FPEN=0b11 (FP an EL0+EL1
 * erlaubt), sodass diese EL1-Zugriffe nicht trappen. Ohne GUI_FP ist die Datei LEER --
 * der Kernel bleibt komplett FP-frei (T1.8-objdump-Guardian im Standard-Verify) und
 * FPEN=0b00 (RC-Haertung) bleibt trap-konsistent.
 *
 * Layout (Offsets in Byte):   0..511  q0..q31
 *                            512      FPCR
 *                            520      FPSR
 */
#ifdef GUI_FP

/* Die C-Flags bauen mit -mgeneral-regs-only (nimmt dem Assembler fp/simd weg) --
 * fuer DIESE reine Save/Restore-Datei explizit wieder freischalten. */
.arch armv8-a+fp+simd

.section ".text"

/* void fpctx_save(void *area)  -- x0 = 16-aligned 528-B-Bereich. Nur x0/x1/x2. */
.global fpctx_save
fpctx_save:
    stp     q0,  q1,  [x0, #16*0]
    stp     q2,  q3,  [x0, #16*2]
    stp     q4,  q5,  [x0, #16*4]
    stp     q6,  q7,  [x0, #16*6]
    stp     q8,  q9,  [x0, #16*8]
    stp     q10, q11, [x0, #16*10]
    stp     q12, q13, [x0, #16*12]
    stp     q14, q15, [x0, #16*14]
    stp     q16, q17, [x0, #16*16]
    stp     q18, q19, [x0, #16*18]
    stp     q20, q21, [x0, #16*20]
    stp     q22, q23, [x0, #16*22]
    stp     q24, q25, [x0, #16*24]
    stp     q26, q27, [x0, #16*26]
    stp     q28, q29, [x0, #16*28]
    stp     q30, q31, [x0, #16*30]
    mrs     x1, fpcr
    mrs     x2, fpsr
    str     x1, [x0, #512]          /* stp x-Paar waere imm7-begrenzt (<=504) -> 2x str */
    str     x2, [x0, #520]
    ret

/* void fpctx_restore(const void *area)  -- x0 = Bereich. Nur x0/x1/x2 (+ alle V-Register). */
.global fpctx_restore
fpctx_restore:
    ldr     x1, [x0, #512]
    ldr     x2, [x0, #520]
    msr     fpcr, x1
    msr     fpsr, x2
    ldp     q0,  q1,  [x0, #16*0]
    ldp     q2,  q3,  [x0, #16*2]
    ldp     q4,  q5,  [x0, #16*4]
    ldp     q6,  q7,  [x0, #16*6]
    ldp     q8,  q9,  [x0, #16*8]
    ldp     q10, q11, [x0, #16*10]
    ldp     q12, q13, [x0, #16*12]
    ldp     q14, q15, [x0, #16*14]
    ldp     q16, q17, [x0, #16*16]
    ldp     q18, q19, [x0, #16*18]
    ldp     q20, q21, [x0, #16*20]
    ldp     q22, q23, [x0, #16*22]
    ldp     q24, q25, [x0, #16*24]
    ldp     q26, q27, [x0, #16*26]
    ldp     q28, q29, [x0, #16*28]
    ldp     q30, q31, [x0, #16*30]
    ret

#endif /* GUI_FP */
