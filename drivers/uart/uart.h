/*
 * drivers/uart/uart.h  --  PL011 (UART0) Treiber-Schnittstelle
 */
#ifndef RPI_RTOS_UART_H
#define RPI_RTOS_UART_H

#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
char uart_getc(void);             /* blockierend: ein Zeichen vom Serial lesen */
int  uart_getc_nb(void);          /* nicht blockierend: Zeichen oder -1 (FIFO leer) */
void uart_puts(const char *s);
void uart_write(const char *buf, uint32_t len);  /* len Rohbytes string-atomar (EL0 sys_write) */
void uart_puthex(unsigned long long value);
void uart_putdec(unsigned long long value);

/* Lock-freie Notausgabe (nur fuer panic()/Fault-Halt): nimmt den UART-Lock NICHT, um einen
 * Self-Deadlock zu vermeiden, falls der fehlerhafte Kern ihn gerade haelt. */
void uart_panic_puts(const char *s);
void uart_panic_hex(unsigned long long value);

/* Mehrere put*-Aufrufe zu EINER cross-core-atomaren Ausgabe klammern (reentrant; mit
 * uart_end paaren). Fuer Zeilen, die aus Text + Zahlen zusammengesetzt sind. */
void uart_begin(void);
void uart_end(void);

/* Schaltet den UART-Ausgabe-Spinlock scharf. VOR mmu_init NICHT aufrufen: der Lock nutzt LDAXR/STXR,
 * die erst mit MMU+Caches AN funktionieren. Frueh (single-core) ist kein Lock noetig. Aus kmain direkt
 * nach mmu_init aufrufen. */
void uart_lock_online(void);

/* Registriert einen Ausgabe-Spiegel: jedes ueber uart_putc ausgegebene Zeichen
 * wird zusaetzlich an fn geleitet (z.B. fbcon_putc fuer die HDMI-Konsole).
 * fn == NULL schaltet die Spiegelung ab. */
void uart_set_mirror(void (*fn)(char c));

#endif /* RPI_RTOS_UART_H */
