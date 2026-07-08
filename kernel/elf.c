/*
 * kernel/elf.c  --  Minimaler ELF64-Loader (statisch gelinktes ET_EXEC)
 */
#include <stdint.h>
#include "uart.h"
#include "proc.h"
#include "elf.h"

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

#define PT_LOAD       1
#define ET_EXEC       2
#define EM_AARCH64    183

int elf_load(const void *buf, uint32_t len, uint64_t user_phys_base, uint64_t *entry)
{
    if (len < sizeof(struct elf64_ehdr)) {
        return -1;
    }
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)buf;

    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) {
        uart_puts("[elf] kein ELF-Magic\n");
        return -1;
    }
    if (eh->e_ident[4] != 2 /* ELFCLASS64 */ || eh->e_ident[5] != 1 /* LE */) {
        uart_puts("[elf] nicht 64-bit/little-endian\n");
        return -1;
    }
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_AARCH64) {
        uart_puts("[elf] kein AArch64-ET_EXEC\n");
        return -1;
    }

    /* Program-Header-Tabelle gegen die Dateigroesse absichern (overflow-sicher:
     * e_phoff>len wird VOR der Subtraktion len-e_phoff geprueft). */
    if (eh->e_phentsize < sizeof(struct elf64_phdr) ||
        eh->e_phoff > len ||
        (uint64_t)eh->e_phnum * eh->e_phentsize > (uint64_t)len - eh->e_phoff) {
        uart_puts("[elf] ungueltige Program-Header-Tabelle\n");
        return -1;
    }

    const uint8_t *base = (const uint8_t *)buf;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(base + eh->e_phoff +
                                        (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        /* Standard-ELF-Invariante: Datei-Anteil nie groesser als Speicher-Anteil. */
        if (ph->p_filesz > ph->p_memsz) {
            uart_puts("[elf] p_filesz > p_memsz\n");
            return -1;
        }
        /* Zielbereich muss komplett in der User-Kachel liegen (overflow-sicher). */
        if (ph->p_vaddr < USER_BASE || ph->p_vaddr > USER_STACK_TOP ||
            ph->p_memsz > USER_STACK_TOP - ph->p_vaddr) {
            uart_puts("[elf] Segment ausserhalb der User-Region\n");
            return -1;
        }
        /* Quellbereich muss komplett in der Datei liegen (overflow-sicher). */
        if (ph->p_offset > len || ph->p_filesz > (uint64_t)len - ph->p_offset) {
            uart_puts("[elf] Segment-Offset jenseits der Datei\n");
            return -1;
        }

        /* Ziel physisch: phys-Basis + (User-VA - USER_BASE), identitaetsgemappt. */
        uint8_t       *dst = (uint8_t *)(uintptr_t)(user_phys_base +
                                                   (ph->p_vaddr - USER_BASE));
        const uint8_t *src = base + ph->p_offset;
        uint64_t       b;
        for (b = 0; b < ph->p_filesz; b++) {
            dst[b] = src[b];
        }
        for (; b < ph->p_memsz; b++) {            /* .bss nullen */
            dst[b] = 0;
        }
    }

    *entry = eh->e_entry;
    return 0;
}
