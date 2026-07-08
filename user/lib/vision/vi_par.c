/*
 * user/lib/vision/vi_par.c  --  EL0-Wrapper des VISION-Kernel-Parallel-For.
 *
 * PARSPAWN startet die Worker (Kern 1..n-1); der Aufrufer rechnet Slice 0 selbst und PARJOIN'et.
 * Jeder Worker springt (per Kernel-eret) vi_worker_entry an, fuehrt den Slice aus und meldet sich
 * per SYS_VI_WORKER_DONE fertig. g_slice liegt im GETEILTEN Adressraum -> die Worker sehen es.
 */
#include "abi.h"
#include "ulib.h"
#include "vision/vi_par.h"

static vi_slice_fn g_slice;

/* Ansprungziel der Worker (Kernel setzt x0=arg, x1=wid, x2=n). Kehrt NICHT nach EL0 zurueck. */
static void vi_worker_entry(void *arg, int wid, int n)
{
    g_slice(arg, wid, n);
    sys0(SYS_VI_WORKER_DONE);          /* Kernel: sem_post + Task-Ende */
    for (;;) { }                       /* unerreichbar */
}

void vi_parallel(vi_slice_fn fn, void *arg, int n)
{
    if (n < 2) { fn(arg, 0, (n < 1) ? 1 : n); return; }
    g_slice = fn;
    long got = sys3(SYS_VI_PARSPAWN, (long)(void *)vi_worker_entry, (long)arg, (long)n);
    fn(arg, 0, n);                     /* Aufrufer rechnet Slice 0 (Kern 0, eigener Stack) */
    if (got > 0) { sys0(SYS_VI_PARJOIN); }
}
