/*
 * include/diag_blink.h  --  Blind-Boot-Diagnose ueber gelatchte Header-GPIOs (nur #ifdef DIAG_BLINK).
 */
#ifndef RPI_RTOS_DIAG_BLINK_H
#define RPI_RTOS_DIAG_BLINK_H
#ifdef DIAG_BLINK

void diag_latch(unsigned pin);        /* GPIO 'pin' als Output + dauerhaft HIGH (direktes GPIO-Register) */
void diag_heartbeat_task(void *arg);  /* fuer task_create: GPIO21/Pin40 langsam toggeln = Scheduler laeuft */

#endif /* DIAG_BLINK */
#endif /* RPI_RTOS_DIAG_BLINK_H */
