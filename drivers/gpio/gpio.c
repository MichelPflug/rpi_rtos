/*
 * drivers/gpio/gpio.c  --  BCM2711 GPIO-Treiber
 *
 * Low-Peripheral-Modus, Basis 0xFE200000. Der BCM2711 nutzt fuer Pull-Up/-Down
 * die GPIO_PUP_PDN_CNTRL_REGn (2 Bit/Pin) statt des alten GPPUD-Schemas.
 */
#include <stdint.h>
#include "mmio.h"
#include "gpio.h"

#define GPIO_BASE     0xFE200000UL
#define GPFSEL0       (GPIO_BASE + 0x00)   /* +4*n: 10 Pins/Reg, 3 Bit/Pin */
#define GPSET0        (GPIO_BASE + 0x1C)   /* +4*n: 32 Pins/Reg */
#define GPCLR0        (GPIO_BASE + 0x28)
#define GPLEV0        (GPIO_BASE + 0x34)
#define GPIO_PUP_PDN0 (GPIO_BASE + 0xE4)   /* +4*n: 16 Pins/Reg, 2 Bit/Pin */

#define GPIO_MAX_PIN  57                    /* BCM2711: GPIO 0..57 */

void gpio_set_function(unsigned pin, gpio_func_t func)
{
    if (pin > GPIO_MAX_PIN) {
        return;
    }
    uintptr_t reg = GPFSEL0 + (pin / 10) * 4;
    unsigned shift = (pin % 10) * 3;
    uint32_t v = mmio_read32(reg);
    v &= ~(7u << shift);
    v |= ((uint32_t)func & 7u) << shift;
    mmio_write32(reg, v);
}

gpio_func_t gpio_get_function(unsigned pin)
{
    if (pin > GPIO_MAX_PIN) {
        return GPIO_FUNC_INPUT;
    }
    uintptr_t reg = GPFSEL0 + (pin / 10) * 4;
    unsigned shift = (pin % 10) * 3;
    return (gpio_func_t)((mmio_read32(reg) >> shift) & 7u);
}

void gpio_set_pull(unsigned pin, gpio_pull_t pull)
{
    if (pin > GPIO_MAX_PIN) {
        return;
    }
    uintptr_t reg = GPIO_PUP_PDN0 + (pin / 16) * 4;
    unsigned shift = (pin % 16) * 2;
    uint32_t v = mmio_read32(reg);
    v &= ~(3u << shift);
    v |= ((uint32_t)pull & 3u) << shift;
    mmio_write32(reg, v);
}

void gpio_set(unsigned pin)
{
    if (pin > GPIO_MAX_PIN) {
        return;
    }
    mmio_write32(GPSET0 + (pin / 32) * 4, 1u << (pin % 32));
}

void gpio_clear(unsigned pin)
{
    if (pin > GPIO_MAX_PIN) {
        return;
    }
    mmio_write32(GPCLR0 + (pin / 32) * 4, 1u << (pin % 32));
}

void gpio_write(unsigned pin, int value)
{
    if (value) {
        gpio_set(pin);
    } else {
        gpio_clear(pin);
    }
}

int gpio_level(unsigned pin)
{
    if (pin > GPIO_MAX_PIN) {
        return 0;
    }
    return (int)((mmio_read32(GPLEV0 + (pin / 32) * 4) >> (pin % 32)) & 1u);
}
