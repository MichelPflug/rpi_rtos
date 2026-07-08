/*
 * arch/aarch64/linker.ld  --  rpi_rtos M0
 *
 * Erzeugt ein flaches Image, das ab 0x80000 geladen wird (Pi-AArch64-Konvention).
 * _start (.text.boot) liegt garantiert ganz vorne.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;                    /* Load-/Link-Adresse des Kernels */

    .text : {
        KEEP(*(.text.boot))         /* Boot-Code zuerst -> _start @ 0x80000 */
        *(.text*)
    }

    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    . = ALIGN(16);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* 64 KiB Boot-Stack des Primaerkerns direkt hinter dem Kernel (waechst nach unten). */
    . = ALIGN(16);
    . += 0x10000;
    __stack_top = .;

    /* Sekundaerkern-Stacks: Kern 1..3, je 16 KiB. Kern N nutzt den Bereich
     * [__stack_top + (N-1)*16K, __stack_top + N*16K); Top = __stack_top + N*16K. */
    . += 3 * 0x4000;
    __sec_stack_top = .;

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame_hdr) }
}
