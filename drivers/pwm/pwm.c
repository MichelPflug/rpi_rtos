/*
 * drivers/pwm/pwm.c  --  BCM2711 PWM0 (real-HW-only)
 *
 * ============================ WICHTIGER HINWEIS ============================
 * QEMU raspi4b emuliert den PWM-Block NICHT (Zugriff auf 0xFE20C000 loest einen
 * External-Abort aus, empirisch bestaetigt). Auch der Clock-Manager (0xFE101000)
 * ist in QEMU nicht modelliert. Dieser Treiber ist daher REAL-HW-ONLY: im
 * raspi4b-Build kompiliert, in der QEMU-Demo NICHT aufgerufen. Nur Code-Review.
 * ==========================================================================
 *
 * PWM0 @ 0xFE20C000, Pinmux (ALT0): GPIO12=PWM0_0, GPIO13=PWM0_1.
 * Takt vom Clock-Manager (CM_PWM @ 0xFE1010A0/A4), Quelle: Oszillator 54 MHz.
 */
#include <stdint.h>
#include "mmio.h"
#include "gpio.h"
#include "pwm.h"

#define PWM0_BASE  0xFE20C000UL
#define PWM_CTL    (PWM0_BASE + 0x00)
#define PWM_STA    (PWM0_BASE + 0x04)
#define PWM_RNG1   (PWM0_BASE + 0x10)
#define PWM_DAT1   (PWM0_BASE + 0x14)
#define PWM_RNG2   (PWM0_BASE + 0x20)
#define PWM_DAT2   (PWM0_BASE + 0x24)

/* CTL-Bits Kanal 1; Kanal 2 = +8. */
#define PWM_PWEN1  (1u << 0)
#define PWM_MODE1  (1u << 1)
#define PWM_MSEN1  (1u << 7)

/* Clock-Manager (CM) fuer PWM. */
#define CM_BASE    0xFE101000UL
#define CM_PWMCTL  (CM_BASE + 0x0A0)
#define CM_PWMDIV  (CM_BASE + 0x0A4)
#define CM_PASSWD  0x5A000000u
#define CM_ENAB    (1u << 4)
#define CM_BUSY    (1u << 7)
#define CM_SRC_OSC 1u           /* Quelle 1 = Oszillator (Pi4: 54 MHz) */
#define OSC_HZ     54000000u

void pwm_init(const pwm_config_t *cfg)
{
    /* 1) PWM-Clock im Clock-Manager einstellen (dokumentierte CM-Sequenz):
     *    zuerst NUR ENAB loeschen (Quelle dabei NICHT umschalten -- darf nicht
     *    geaendert werden, solange der Generator laeuft), auf BUSY=0 warten,
     *    DANN Quelle+Divider setzen. Alle Polls sind bounded (kein Hang). */
    uint32_t guard;

    uint32_t cur = mmio_read32(CM_PWMCTL);
    mmio_write32(CM_PWMCTL, CM_PASSWD | (cur & ~CM_ENAB));   /* ENAB=0, SRC unveraendert */
    guard = 1000000;
    while ((mmio_read32(CM_PWMCTL) & CM_BUSY) && guard--) {
    }

    /* DIVI ist 12-bit -> kleinster Basistakt ~ OSC/4095 (~13,2 kHz); darunter
     * wird still geklemmt (hoehere Frequenz als angefragt). */
    uint32_t divi = 1;
    if (cfg->clock_hz) {
        divi = OSC_HZ / cfg->clock_hz;
        if (divi < 1) {
            divi = 1;
        }
        if (divi > 0xFFF) {
            divi = 0xFFF;
        }
    }
    mmio_write32(CM_PWMDIV, CM_PASSWD | (divi << 12));
    mmio_write32(CM_PWMCTL, CM_PASSWD | CM_SRC_OSC | CM_ENAB);
    guard = 1000000;
    while (!(mmio_read32(CM_PWMCTL) & CM_BUSY) && guard--) {
    }

    /* 2) Pinmux GPIO12/13 -> ALT0 (PWM0). */
    gpio_set_function(12, GPIO_FUNC_ALT0);
    gpio_set_function(13, GPIO_FUNC_ALT0);

    /* 3) PWM-Block zuruecksetzen (beide Kanaele aus). */
    mmio_write32(PWM_CTL, 0);
}

void pwm_set(unsigned channel, uint32_t range, uint32_t data)
{
    if (channel == 0) {
        mmio_write32(PWM_RNG1, range);
        mmio_write32(PWM_DAT1, data);
        uint32_t ctl = mmio_read32(PWM_CTL);
        ctl &= ~0xFFu;                       /* Kanal-1-Bits */
        ctl |= PWM_PWEN1 | PWM_MSEN1;        /* aktiv, Mark-Space-Modus */
        mmio_write32(PWM_CTL, ctl);
    } else {
        mmio_write32(PWM_RNG2, range);
        mmio_write32(PWM_DAT2, data);
        uint32_t ctl = mmio_read32(PWM_CTL);
        ctl &= ~0xFF00u;                     /* Kanal-2-Bits */
        ctl |= (PWM_PWEN1 | PWM_MSEN1) << 8;
        mmio_write32(PWM_CTL, ctl);
    }
}
