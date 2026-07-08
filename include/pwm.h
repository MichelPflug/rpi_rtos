/*
 * include/pwm.h  --  BCM2711 PWM0 (einfach konfigurierbar)
 *
 * HINWEIS: In QEMU raspi4b ist der PWM-Block NICHT emuliert (Zugriff auf
 * 0xFE20C000 -> External-Abort, empirisch bestaetigt). Dieser Treiber ist
 * real-HW-only und in QEMU nicht ausfuehrbar (nur Code-Review).
 */
#ifndef RPI_RTOS_PWM_H
#define RPI_RTOS_PWM_H

#include <stdint.h>

typedef struct {
    uint32_t clock_hz;   /* PWM-Basistakt (vom Clock-Manager erzeugt) */
} pwm_config_t;

/* Richtet den PWM-Takt (Clock-Manager) und GPIO12/13 (ALT0) ein. REAL-HW-ONLY. */
void pwm_init(const pwm_config_t *cfg);

/* Kanal 0/1: Periode = range, Pulsbreite = data (Mark-Space-Modus). REAL-HW-ONLY. */
void pwm_set(unsigned channel, uint32_t range, uint32_t data);

#endif /* RPI_RTOS_PWM_H */
