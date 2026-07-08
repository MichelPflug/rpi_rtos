/*
 * include/elf.h  --  Minimaler ELF64-Loader
 */
#ifndef RPI_RTOS_ELF_H
#define RPI_RTOS_ELF_H

#include <stdint.h>

/* Laedt die PT_LOAD-Segmente aus 'buf' (Laenge len). Die User-VA p_vaddr muss in
 * der User-Region liegen; physisch geschrieben wird nach user_phys_base +
 * (p_vaddr - USER_BASE). Liefert den Einsprungpunkt (User-VA) in *entry.
 * 0 = ok, <0 = Fehler. */
int elf_load(const void *buf, uint32_t len, uint64_t user_phys_base, uint64_t *entry);

#endif /* RPI_RTOS_ELF_H */
