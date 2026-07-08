/*
 * drivers/spi/spi.c  --  BCM2711 SPI0-Master (Polling)
 *
 * SPI0-Pins (ALT0): GPIO7=CE1, GPIO8=CE0, GPIO9=MISO, GPIO10=MOSI, GPIO11=SCLK.
 * Takt: SCLK = Core-Clock (250 MHz) / CDIV; CDIV ist eine Zweierpotenz.
 */
#include <stdint.h>
#include "mmio.h"
#include "aarch64.h"
#include "spinlock.h"
#include "gpio.h"
#include "spi.h"

#define SPI0_BASE 0xFE204000UL
#define SPI_CS    (SPI0_BASE + 0x00)
#define SPI_FIFO  (SPI0_BASE + 0x04)
#define SPI_CLK   (SPI0_BASE + 0x08)

#define CS_TA     (1u << 7)
#define CS_CLEAR  (3u << 4)     /* TX+RX-FIFO leeren */
#define CS_CPHA   (1u << 2)
#define CS_CPOL   (1u << 3)
#define CS_DONE   (1u << 16)
#define CS_RXD    (1u << 17)
#define CS_TXD    (1u << 18)

/* Core-Clock-Quelle des SPI-Teilers. 250 MHz ist der uebliche Wert; auf dem
 * BCM2711 bestimmt die Firmware den Core-Takt -- fuer praezise SCLK in
 * boot/config.txt `core_freq`/`core_freq_min` fixieren ( >>HW<< ). */
#define SPI_CORE_CLOCK 250000000u
#define SPI_TIMEOUT    2000000u

static uint32_t s_cs_base;

/* Serialisiert SPI0-Zugriffe (geteiltes TX/RX-FIFO + die statische s_cs_base) ueber alle Kerne.*/
static spinlock_t s_spilock = SPINLOCK_INIT;

static uint64_t spi_lock(void)
{
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_spilock);
    return f;
}
static void spi_unlock(uint64_t f)
{
    spin_unlock(&s_spilock);
    WRITE_SYSREG(daif, f);
}

static uint32_t clk_to_cdiv(uint32_t hz)
{
    if (hz == 0) {
        return 0;   /* 0 => 65536 (langsamster Takt) */
    }
    /* Aufrundende Division: cdiv >= core/hz garantieren, damit SCLK <= hz bleibt
     * (clock_hz ist die Obergrenze = Maximaltakt des Slaves). */
    uint32_t want = (SPI_CORE_CLOCK + hz - 1) / hz;
    uint32_t cdiv = 2;
    while (cdiv < want && cdiv < 65536) {
        cdiv <<= 1;             /* auf Zweierpotenz aufrunden */
    }
    if (cdiv >= 65536) {
        return 0;
    }
    return cdiv;
}

void spi_init(const spi_config_t *cfg)
{
    uint64_t f = spi_lock();

    /* Pinmux: GPIO7..11 auf ALT0. */
    for (unsigned pin = 7; pin <= 11; pin++) {
        gpio_set_function(pin, GPIO_FUNC_ALT0);
    }

    s_cs_base = (uint32_t)(cfg->cs & 3u);
    if (cfg->mode & 1u) {
        s_cs_base |= CS_CPHA;
    }
    if (cfg->mode & 2u) {
        s_cs_base |= CS_CPOL;
    }

    mmio_write32(SPI_CLK, clk_to_cdiv(cfg->clock_hz));
    mmio_write32(SPI_CS, s_cs_base | CS_CLEAR);   /* FIFOs leeren, TA aus */

    spi_unlock(f);
}

int spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    uint64_t f = spi_lock();

    mmio_write32(SPI_CS, s_cs_base | CS_CLEAR | CS_TA);   /* Transfer aktiv */

    /* Iterationsbudget mit der Laenge skalieren (langsamer Takt + lange Transfers). */
    uint32_t budget = SPI_TIMEOUT + (uint32_t)len * 100000u;
    uint32_t wr = 0, rd = 0, guard = budget;
    while ((wr < len || rd < len) && guard--) {
        uint32_t cs = mmio_read32(SPI_CS);
        while (wr < len && (cs & CS_TXD)) {
            mmio_write32(SPI_FIFO, tx ? tx[wr] : 0);
            wr++;
            cs = mmio_read32(SPI_CS);
        }
        while (rd < len && (cs & CS_RXD)) {
            uint8_t b = (uint8_t)mmio_read32(SPI_FIFO);
            if (rx) {
                rx[rd] = b;
            }
            rd++;
            cs = mmio_read32(SPI_CS);
        }
    }

    /* Abschluss abwarten, dann Transfer beenden. */
    guard = budget;
    while (!(mmio_read32(SPI_CS) & CS_DONE) && guard--) {
    }
    mmio_write32(SPI_CS, s_cs_base);   /* TA aus */

    int r = (wr == len && rd == len) ? 0 : -1;
    spi_unlock(f);
    return r;
}
