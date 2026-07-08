/*
 * boards/virt/linker_virt.ld  --  QEMU `virt` Netz-Test-Harness
 *
 * QEMU virt legt den RAM bei 0x40000000 an und laedt das per `-kernel`
 * uebergebene ELF gemaess seiner Programm-Header; Eintritt = e_entry (_start).
 * Wir linken 1 MiB in den RAM hinein.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40100000;                 /* Link-/Ladeadresse im virt-RAM */

    .text : {
        KEEP(*(.text.boot))         /* _start ganz vorne */
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

    /* 64 KiB Boot-Stack hinter dem Image. */
    . = ALIGN(16);
    . += 0x10000;
    __stack_top = .;

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame_hdr) }
}
