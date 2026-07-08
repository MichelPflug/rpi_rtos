/*
 * include/spi.h  --  BCM2711 SPI0 (Master, Polling, einfach konfigurierbar)
 */
#ifndef RPI_RTOS_SPI_H
#define RPI_RTOS_SPI_H

#include <stdint.h>

typedef struct {
    uint32_t clock_hz;   /* gewuenschte SCLK-Frequenz */
    uint8_t  mode;       /* SPI-Modus 0..3 (Bit1=CPOL, Bit0=CPHA) */
    uint8_t  cs;         /* Chip-Select 0..2 */
} spi_config_t;

/* Konfiguriert SPI0 (GPIO7-11 auf ALT0, Takt, Modus). */
void spi_init(const spi_config_t *cfg);

/* Vollduplex-Transfer von len Bytes. tx oder rx darf NULL sein. 0 = ok, -1 = Timeout. */
int  spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len);

#endif /* RPI_RTOS_SPI_H */
