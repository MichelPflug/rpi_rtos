/*
 * user/lib/vision/vi_par.h  --  EL0-Seite des Kernel-Parallel-For.
 *
 * Ruft die Slice-Fn fn(arg, wid, n) fuer wid=0..n-1 verteilt ueber bis zu 4 A72-Kerne auf:
 * wid=0 auf dem Aufrufer-Kern, wid=1..n-1 als Co-Threads (geteilter Adressraum) auf den anderen
 * Kernen (VISION-Kernel-Syscalls). Kehrt erst zurueck, wenn ALLE Slices fertig sind. n<2 -> single-core.
 */
#ifndef RPI_RTOS_VI_PAR_H
#define RPI_RTOS_VI_PAR_H

typedef void (*vi_slice_fn)(void *arg, int wid, int n);

void vi_parallel(vi_slice_fn fn, void *arg, int n);

#endif /* RPI_RTOS_VI_PAR_H */
