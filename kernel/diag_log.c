/*
 * kernel/diag_log.c  --  Boot-Log-Mitschnitt in RAM -> hdd1:BOOTLOG.TXT. Ganz #ifdef DIAG_LOG.
 *
 * uart.c tee't (nur Kern 0) jedes ausgegebene Byte hierher; nach vfs_init [5] wird der Puffer auf die
 * SD geschrieben (und spaeter mehrfach ueberschrieben, damit der Log den letzten Boot-Stand zeigt --
 * bleibt der Kernel spaeter haengen, persistiert der letzte erfolgreiche Schnitt).
 */
#ifdef DIAG_LOG

#include <stdint.h>
#include "diag_log.h"
#include "vfs.h"
#include "sched.h"

#define DIAG_LOG_CAP 32768u        /* der fruehe Boot passt locker in 32 KiB Text */
static char              s_buf[DIAG_LOG_CAP];
static volatile uint32_t s_len;
static volatile int      s_writing;   /* waehrend des SD-Schreibens NICHT re-teen (kein Selbst-Wachstum) */

void diag_log_putc(char c)
{
    if (s_writing) { return; }
    uint32_t n = s_len;
    if (n < DIAG_LOG_CAP) {
        s_buf[n] = c;
        s_len = n + 1u;
    }
    /* voll -> Rest verwerfen (best-effort Diagnose) */
}

void diag_log_to_sd(void)
{
    s_writing = 1;
    uint32_t n = s_len;
    /* hdd0 (Boot-Partition, wo der Nutzer zuerst schaut): read-only gemountet -> privilegierter
     * Kernel-Schreibpfad (derselbe, mit dem der Boot die Benutzer-DB auf hdd0 anlegt). */
    (void)vfs_write_file_priv("hdd0:BOOTLOG.TXT", s_buf, n);
    /* hdd1 (User-Partition) zusaetzlich als Fallback (normaler Schreibpfad). */
    (void)vfs_write_file("hdd1:BOOTLOG.TXT", s_buf, n);
    s_writing = 0;
}

void diag_log_task(void *arg)
{
    (void)arg;
    /* Ein paar Nachschuesse (~2 s Takt), damit auch ein Haenger NACH dem Boot noch im Log landet;
     * danach Ruhe (keine endlose SD-Schreiblast). */
    for (int i = 0; i < 15; i++) {
        task_sleep_ticks(200);     /* ~2 s bei 100 Hz */
        diag_log_to_sd();
    }
}

#endif /* DIAG_LOG */
