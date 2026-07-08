/*
 * user/user.ld  --  Linker-Script fuer User-Programme (rpi_rtos)
 *
 * Statisch gelinktes ET_EXEC auf USER_BASE = 0x10000000. Der Loader nutzt
 * e_entry (= _start), die Reihenfolge der .text spielt daher keine Rolle.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x10000000;

    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    . = ALIGN(16);
    .bss    : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame_hdr) }
}
