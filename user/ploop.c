/*
 * user/ploop.c  --  unsterbliches, stilles EL0-Testkind (Endlosschleife mit sleep)
 *
 * Wird von pwaiter als Enkelkind gespawnt und endet NIE von selbst. Damit deckt der
 * kill-in-wait-Guardian den entscheidenden Fall ab: ohne den Fix wuerde pwaiter beim wait()
 * auf dieses Kind ENDLOS re-blocken (nie am Safe-Point sterben). Mit dem Fix wird pwaiter am
 * wait-Safe-Point beendet, obwohl das Kind weiterlaeuft.
 */
#include "abi.h"

static long sys3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

void _start(void)
{
    /* Still endlos schlafen -- CPU wird brav abgegeben, kein Log-Rauschen. */
    for (;;) {
        sys3(SYS_SLEEP_MS, 200, 0, 0);
    }
}
