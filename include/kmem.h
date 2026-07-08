/*
 * include/kmem.h  --  Freestanding-Speicherhelfer
 *
 * Mit -ffreestanding/-nostdlib gibt es keine libc. clang/gcc duerfen fuer
 * Struct-/Array-Kopien dennoch Aufrufe von memcpy/memset/memmove emittieren;
 * diese Symbole stellen wir hier zentral bereit (lib/kmem.c).
 */
#ifndef RPI_RTOS_KMEM_H
#define RPI_RTOS_KMEM_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

#endif /* RPI_RTOS_KMEM_H */
