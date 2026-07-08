/*
 * boards/virt/uart_virt.c  --  PL011 (UART0) fuer QEMU `virt`
 *
 * Auf der virt-Maschine liegt die PL011-UART bei 0x09000000 und haengt an der
 * seriellen Konsole. Kein GPIO-Setup noetig (virt hat keinen BCM-GPIO-Block).
 * Implementiert dieselbe uart.h-API wie der BCM2711-Treiber, wird aber statt
 * dessen in den virt-Build gelinkt.
 */
#include <stdint.h>
#include "mmio.h"
#include "uart.h"

#define UART0_BASE  0x09000000UL

#define UART0_DR    (UART0_BASE + 0x00UL)
#define UART0_FR    (UART0_BASE + 0x18UL)
#define UART0_IBRD  (UART0_BASE + 0x24UL)
#define UART0_FBRD  (UART0_BASE + 0x28UL)
#define UART0_LCRH  (UART0_BASE + 0x2CUL)
#define UART0_CR    (UART0_BASE + 0x30UL)
#define UART0_IMSC  (UART0_BASE + 0x38UL)
#define UART0_ICR   (UART0_BASE + 0x44UL)

#define FR_TXFF     (1u << 5)
#define FR_BUSY     (1u << 3)

void uart_init(void)
{
    mmio_write32(UART0_CR, 0);
    while (mmio_read32(UART0_FR) & FR_BUSY) {
    }
    mmio_write32(UART0_ICR, 0x7FF);
    /* Baudrate ist in QEMU ohne Belang; sinnvolle Defaults setzen. */
    mmio_write32(UART0_IBRD, 26);
    mmio_write32(UART0_FBRD, 3);
    mmio_write32(UART0_LCRH, (3u << 5) | (1u << 4));   /* 8N1, FIFO */
    mmio_write32(UART0_IMSC, 0);
    mmio_write32(UART0_CR, (1u << 0) | (1u << 8) | (1u << 9));   /* EN|TXE|RXE */
}

void uart_putc(char c)
{
    while (mmio_read32(UART0_FR) & FR_TXFF) {
    }
    mmio_write32(UART0_DR, (uint32_t)(unsigned char)c);
}

void uart_puts(const char *s)
{
    for (; *s != '\0'; ++s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s);
    }
}

void uart_puthex(unsigned long long value)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_putc(hex[(value >> shift) & 0xF]);
    }
}

void uart_putdec(unsigned long long value)
{
    char buf[20];
    int i = 0;

    if (value == 0) {
        uart_putc('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i-- > 0) {
        uart_putc(buf[i]);
    }
}
