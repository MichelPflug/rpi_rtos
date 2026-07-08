/*
 * user/pwaiter.c  --  Testprozess fuer "kill eines in wait() blockierten Prozesses" (EL0)
 *
 * Spawnt ein UNSTERBLICHES Enkelkind (PLOOP) und blockiert in SYS_WAIT darauf. Wird dieser Prozess
 * waehrend des Wartens per SYS_KILL beendet, MUSS er am wait-Safe-Point sterben -- obwohl das
 * Enkelkind ewig weiterlaeuft. Ohne den Fix wuerde er endlos re-blocken und NIE sterben (Hang);
 * dann kann die Shell ihn per wait() auch nicht ernten. Der Guardian prueft genau das (2. Kill).
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

static char lbuf[96];
static int  lpos;
static void lflush(void) { if (lpos > 0) { sys3(SYS_WRITE, 1, (long)lbuf, lpos); lpos = 0; } }
static void uwrite(const char *s)
{
    for (; *s; ++s) {
        if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = *s; }
        if (*s == '\n') { lflush(); }
    }
}
static void uputn(long n)
{
    char tmp[20]; int i = 0;
    if (n == 0) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = '0'; } return; }
    while (n > 0 && i < 20) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (i > 0) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = tmp[--i]; } else { i--; } }
}

void _start(void)
{
    long pid = sys3(SYS_GETPID, 0, 0, 0);
    long gc  = sys3(SYS_SPAWN, (long)"hdd1:PLOOP.ELF", 0, 0);   /* unsterbliches Enkelkind */

    uwrite("  [pwaiter ");
    uputn(pid);
    uwrite("] spawnte ");
    uputn(gc);
    uwrite(", wartet\n");

    long code = sys3(SYS_WAIT, gc, 0, 0);   /* blockiert bis Enkelkind endet -- ODER Kill greift hier */

    /* Diese Zeile darf NUR erscheinen, wenn wait() ungestoert durchlief (kein Kill-in-wait). */
    uwrite("  [pwaiter ");
    uputn(pid);
    uwrite("] kind-geerntet code=");
    uputn(code);
    uwrite("\n");
    sys3(SYS_EXIT, 7, 0, 0);

    for (;;) {
    }
}
