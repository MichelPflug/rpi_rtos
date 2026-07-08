/*
 * include/vi_parallel.h  --  VISION-gegateter Kernel-Parallel-For.
 *
 * Startet kurzzeitig Co-Threads (Kernel-Tasks, die per enter_user-Muster mit GETEILTEM
 * ttbr0 des Aufrufers nach EL0 eret'en) auf den anderen A72-Kernen -> echte Shared-Memory-
 * Parallelitaet fuer die EL0-Inferenz. Implementierung: kernel/vi_parallel.c (ganz #ifdef
 * VISION; ohne das Flag leeres Objekt -> Kernel byte-identisch, wie fpctx.S).
 */
#ifndef RPI_RTOS_VI_PARALLEL_H
#define RPI_RTOS_VI_PARALLEL_H

#include <stdint.h>

/* Startet Worker fuer wid=1..n-1 (Kern 1..n-1), die die EL0-Fn 'entry' mit (arg, wid, n)
 * ausfuehren. Rueckgabe: Anzahl tatsaechlich gestarteter Worker. Danach rechnet der Aufrufer
 * selbst wid=0 und ruft vi_par_join(). Nur EIN Parallel-For gleichzeitig (App ruft seriell). */
int  vi_par_spawn(uint64_t entry, uint64_t arg, int n, uint32_t uid, uint32_t caps);

/* Blockiert, bis alle gestarteten Worker fertig gemeldet haben (Barrier). */
void vi_par_join(void);

/* Von einem Worker (via SYS_VI_WORKER_DONE) gerufen: Fertigmeldung + Task-Ende (kehrt nie zurueck). */
void vi_par_worker_done(void);

/* A4.1-Seam: ein Kamera-Frame nach user_buf greifen (Bytes / -1). In QEMU ist KEIN UVC-Geraet
 * vorhanden -> -1; der echte UVC-Klassentreiber (Enumeration + Stream + iso/bulk ueber usb_hc)
 * ist die Pi4-Bring-up-Aufgabe und ersetzt genau diese Backend-Funktion. */
int  vi_cam_grab(uint64_t user_buf, unsigned long max);

#endif /* RPI_RTOS_VI_PARALLEL_H */
