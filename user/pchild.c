/*
 * user/pchild.c  --  Kind-Prozess fuer den wait/kill-Test (laeuft auf EL0)
 *
 * Zeigt seine eigene PID und die Eltern-PID (SYS_GETPPID -> beweist die Eltern-Kind-Beziehung),
 * laeuft dann eine kurze, unterbrechbare Schleife (SYS_SLEEP = Safe-Point fuer SYS_KILL) und
 * beendet sich mit Exit-Code 42. Der Elternprozess (die Shell) erntet den Code per SYS_WAIT
 * bzw. beendet das Kind vorzeitig per SYS_KILL (dann liefert wait() den Kill-Code 137).
 */
#include "abi.h"

static long sys3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2)
                     : "memory", "cc");
    return x0;
}

static char lbuf[96];
static int  lpos;

static void lflush(void)
{
    if (lpos > 0) {
        sys3(SYS_WRITE, 1, (long)lbuf, lpos);
        lpos = 0;
    }
}

static void uwrite(const char *s)
{
    for (; *s; ++s) {
        if (lpos < (int)sizeof(lbuf)) {
            lbuf[lpos++] = *s;
        }
        if (*s == '\n') {
            lflush();
        }
    }
}

static void uputn(long n)
{
    char tmp[20];
    int  i = 0;
    if (n == 0) {
        if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = '0'; }
        return;
    }
    while (n > 0 && i < 20) {
        tmp[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0) {
        if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = tmp[--i]; } else { i--; }
    }
}

void _start(void)
{
    long pid  = sys3(SYS_GETPID, 0, 0, 0);
    long ppid = sys3(SYS_GETPPID, 0, 0, 0);

    uwrite("  [child ");
    uputn(pid);
    uwrite("] ppid=");
    uputn(ppid);
    uwrite(" laeuft\n");

    /* Kurze, unterbrechbare Schleife: jeder SYS_SLEEP ist ein Safe-Point, an dem ein
     * ausstehendes SYS_KILL greift (dann wird "fertig" nie erreicht). */
    for (int i = 0; i < 8; i++) {
        sys3(SYS_SLEEP_MS, 100, 0, 0);
    }

    uwrite("  [child ");
    uputn(pid);
    uwrite("] fertig -> exit(42)\n");
    sys3(SYS_EXIT, 42, 0, 0);

    for (;;) {
    }
}
