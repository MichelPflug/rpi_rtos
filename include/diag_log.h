/*
 * include/diag_log.h  --  Boot-Log auf die SD schreiben (nur #ifdef DIAG_LOG).
 */
#ifndef RPI_RTOS_DIAG_LOG_H
#define RPI_RTOS_DIAG_LOG_H
#ifdef DIAG_LOG

void diag_log_putc(char c);    /* aus uart.c (Kern 0): jedes Konsolen-Byte in den RAM-Puffer anhaengen */
void diag_log_to_sd(void);     /* Puffer nach hdd1:BOOTLOG.TXT schreiben (VFS muss gemountet sein) */
void diag_log_task(void *arg); /* fuer task_create: schreibt den Log periodisch nach (Scheduler laeuft) */

#endif /* DIAG_LOG */
#endif /* RPI_RTOS_DIAG_LOG_H */
