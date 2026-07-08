/*
 * drivers/i2c/i2c.c  --  BCM2711 BSC1 (I2C-Master, Polling)
 *
 * BSC1 @ 0xFE804000, Pins (ALT0): GPIO2=SDA, GPIO3=SCL.
 * SCL = Core-Clock (150 MHz) / DIV; DIV-Reset 0x5DC => 100 kHz.
 */
#include <stdint.h>
#include "mmio.h"
#include "aarch64.h"
#include "spinlock.h"
#include "gpio.h"
#include "i2c.h"

#define BSC1_BASE 0xFE804000UL
#define BSC_C     (BSC1_BASE + 0x00)
#define BSC_S     (BSC1_BASE + 0x04)
#define BSC_DLEN  (BSC1_BASE + 0x08)
#define BSC_A     (BSC1_BASE + 0x0C)
#define BSC_FIFO  (BSC1_BASE + 0x10)
#define BSC_DIV   (BSC1_BASE + 0x14)

#define C_I2CEN   (1u << 15)
#define C_ST      (1u << 7)
#define C_CLEAR   (1u << 4)
#define C_READ    (1u << 0)

#define S_TA      (1u << 0)
#define S_DONE    (1u << 1)
#define S_TXD     (1u << 4)
#define S_RXD     (1u << 5)
#define S_ERR     (1u << 8)
#define S_CLKT    (1u << 9)

/* BSC-Funktionstakt-Quelle. 150 MHz passt zum DIV-Reset 0x5DC (=100 kHz); auf
 * dem BCM2711 kann der Core-Takt driften -- fuer praezise SCL `core_freq` in
 * boot/config.txt fixieren ( >>HW<< ). */
#define I2C_CORE_CLOCK 150000000u
#define I2C_TIMEOUT    2000000u

/* Serialisiert BSC1-Zugriffe (geteiltes 16-Byte-FIFO + Statusregister) ueber alle Kerne. */
static spinlock_t s_i2clock = SPINLOCK_INIT;

static uint64_t i2c_lock(void)
{
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_i2clock);
    return f;
}
static void i2c_unlock(uint64_t f)
{
    spin_unlock(&s_i2clock);
    WRITE_SYSREG(daif, f);
}

void i2c_init(const i2c_config_t *cfg)
{
    uint64_t f = i2c_lock();

    gpio_set_function(2, GPIO_FUNC_ALT0);   /* SDA1 */
    gpio_set_function(3, GPIO_FUNC_ALT0);   /* SCL1 */

    uint32_t div = 0x5DC;                   /* 100 kHz Default */
    if (cfg->clock_hz) {
        div = I2C_CORE_CLOCK / cfg->clock_hz;
        if (div < 2) {
            div = 2;            /* DIV<2 -> & 0xFFFE = 0 -> HW deutet 0 als 32768 */
        }
        if (div > 0xFFFE) {
            div = 0xFFFE;
        }
    }
    mmio_write32(BSC_DIV, div & 0xFFFE);    /* DIV ist 16-bit, gerade */
    mmio_write32(BSC_C, C_I2CEN);

    i2c_unlock(f);
}

/* Gemeinsamer Transfer; read != 0 => Lesen. */
static int i2c_xfer(uint8_t addr, uint8_t *buf, uint32_t len, int read)
{
    mmio_write32(BSC_S, S_DONE | S_ERR | S_CLKT);   /* Statusflags loeschen */
    mmio_write32(BSC_A, addr & 0x7F);
    mmio_write32(BSC_DLEN, len);
    mmio_write32(BSC_C, C_I2CEN | C_CLEAR | C_ST | (read ? C_READ : 0));

    uint32_t i = 0, guard = I2C_TIMEOUT;
    while (!(mmio_read32(BSC_S) & S_DONE) && guard--) {
        uint32_t s = mmio_read32(BSC_S);
        if (read) {
            while ((s & S_RXD) && i < len) {
                buf[i++] = (uint8_t)mmio_read32(BSC_FIFO);
                s = mmio_read32(BSC_S);
            }
        } else {
            while ((s & S_TXD) && i < len) {
                mmio_write32(BSC_FIFO, buf[i++]);
                s = mmio_read32(BSC_S);
            }
        }
    }

    /* Nach DONE verbleibende RX-Bytes abholen. */
    if (read) {
        while ((mmio_read32(BSC_S) & S_RXD) && i < len) {
            buf[i++] = (uint8_t)mmio_read32(BSC_FIFO);
        }
    }

    uint32_t s = mmio_read32(BSC_S);
    if (!(s & S_DONE) || (s & (S_ERR | S_CLKT)) || i != len) {
        return -1;   /* Timeout, kein ACK oder Clock-Stretch-Timeout */
    }
    return 0;
}

int i2c_write(uint8_t addr, const uint8_t *buf, uint32_t len)
{
    uint64_t f = i2c_lock();
    int r = i2c_xfer(addr, (uint8_t *)buf, len, 0);
    i2c_unlock(f);
    return r;
}

int i2c_read(uint8_t addr, uint8_t *buf, uint32_t len)
{
    uint64_t f = i2c_lock();
    int r = i2c_xfer(addr, buf, len, 1);
    i2c_unlock(f);
    return r;
}
