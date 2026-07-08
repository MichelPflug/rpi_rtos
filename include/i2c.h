/*
 * include/i2c.h  --  BCM2711 BSC1 (I2C-Master, Polling, einfach konfigurierbar)
 */
#ifndef RPI_RTOS_I2C_H
#define RPI_RTOS_I2C_H

#include <stdint.h>

typedef struct {
    uint32_t clock_hz;   /* SCL-Frequenz (0 => 100 kHz Default) */
} i2c_config_t;

/* Konfiguriert BSC1 (GPIO2=SDA, GPIO3=SCL auf ALT0, Takt). */
void i2c_init(const i2c_config_t *cfg);

/* 7-Bit-Adresse. 0 = ok, -1 = Fehler (kein ACK / Timeout / Clock-Stretch). */
int  i2c_write(uint8_t addr, const uint8_t *buf, uint32_t len);
int  i2c_read(uint8_t addr, uint8_t *buf, uint32_t len);

#endif /* RPI_RTOS_I2C_H */
