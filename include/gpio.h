/*
 * include/gpio.h  --  BCM2711 GPIO (einfach konfigurierbar)
 *
 * Funktionsauswahl (Input/Output/ALT0..5), Pull-Widerstaende (BCM2711-Schema)
 * und Pegel setzen/lesen. Basis fuer das Pinmuxing von SPI/I2C/PWM.
 */
#ifndef RPI_RTOS_GPIO_H
#define RPI_RTOS_GPIO_H

/* Funktionscodes wie im GPFSEL-Feld (3 Bit). */
typedef enum {
    GPIO_FUNC_INPUT  = 0,
    GPIO_FUNC_OUTPUT = 1,
    GPIO_FUNC_ALT0   = 4,
    GPIO_FUNC_ALT1   = 5,
    GPIO_FUNC_ALT2   = 6,
    GPIO_FUNC_ALT3   = 7,
    GPIO_FUNC_ALT4   = 3,
    GPIO_FUNC_ALT5   = 2,
} gpio_func_t;

typedef enum {
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP   = 1,
    GPIO_PULL_DOWN = 2,
} gpio_pull_t;

void gpio_set_function(unsigned pin, gpio_func_t func);
gpio_func_t gpio_get_function(unsigned pin);
void gpio_set_pull(unsigned pin, gpio_pull_t pull);
void gpio_set(unsigned pin);             /* Ausgang -> high */
void gpio_clear(unsigned pin);           /* Ausgang -> low  */
void gpio_write(unsigned pin, int value);
int  gpio_level(unsigned pin);           /* aktuellen Pegel lesen (0/1) */

#endif /* RPI_RTOS_GPIO_H */
