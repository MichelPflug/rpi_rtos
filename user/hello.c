/*
 * user/hello.c  --  Beispiel-User-Programm fuer rpi_rtos (laeuft auf EL0)
 *
 * Wird auf USER_BASE (0x10000000) gelinkt, statisch, ohne libc. Kommuniziert
 * ausschliesslich ueber SVC-Syscalls mit dem Kernel. Mehrere Instanzen dieses
 * Programms laufen als isolierte Prozesse (eigener Adressraum, eigene PID).
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

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* Zeilenpuffer: eine ganze Zeile sammeln und beim '\n' in EINEM sys_write ausgeben. So
 * bleibt die Zeile dank des kernelseitigen UART-Locks cross-core ungeteilt -- sonst wuerde
 * die Ausgabe dieses (evtl. auf einem Sekundaerkern laufenden) Prozesses mit der eines
 * anderen Kerns Zeichen fuer Zeichen verschraenken. */
static char lbuf[160];
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
            lflush();                  /* ganze Zeile (inkl. \n) als ein write */
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
        if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = tmp[--i]; }
        else { i--; }
    }
}

static long write_file(const char *path, const char *data, long len)
{
    return sys3(SYS_WRITE_FILE, (long)path, (long)data, len);
}

static long read_file(const char *path, char *buf, long max)
{
    return sys3(SYS_READ_FILE, (long)path, (long)buf, max);
}

void _start(void)
{
    long pid = sys3(SYS_GETPID, 0, 0, 0);
    long cpu = sys3(SYS_GETCPU, 0, 0, 0);

    uwrite("  >>> [Prozess ");
    uputn(pid);
    uwrite("] Start auf EL0 (eigener Adressraum), CPU=");
    uputn(cpu);
    uwrite("\n");

    for (int i = 1; i <= 3; i++) {
        uwrite("  >>> [Prozess ");
        uputn(pid);
        uwrite("] runde ");
        uputn(i);
        uwrite("\n");
        sys3(SYS_SLEEP_MS, 300, 0, 0);     /* CPU abgeben */
    }

    long me = sys3(SYS_WHOAMI, 0, 0, 0);

    /* Datei-I/O (hdd1) -- jetzt VFS-lock-geschuetzt, daher von JEDEM Kern sicher. */
    const char *line = "Zeile von der User-App auf hdd1.\r\n";
    long wn = write_file("hdd1:USERLOG.TXT", line, (long)slen(line));
    uwrite("  >>> [Prozess ");
    uputn(pid);
    uwrite("] schrieb hdd1:USERLOG.TXT (");
    uputn(wn);
    uwrite(" Bytes)\n");

    char rb[80];
    long rn = read_file("hdd1:USERLOG.TXT", rb, 79);
    if (rn >= 0) {
        rb[rn] = '\0';
        uwrite("  >>> [Prozess ");
        uputn(pid);
        uwrite("] las zurueck: ");
        uwrite(rb);
    }

    /* Sicherheitsprobe: Eine EL0-App darf NICHT auf die System-Partition hdd0 zugreifen. */
    long sec = read_file("hdd0:USERS.DB", rb, 79);
    uwrite("  >>> [Prozess ");
    uputn(pid);
    uwrite(sec < 0 ? "] hdd0-Zugriff verweigert (korrekt)\n"
                   : "] hdd0 LESBAR(!)\n");

    if (cpu == 0) {
        /* Rechte-Demo: privilegiertes SYS_USERADD. Die Benutzer-DB ist jetzt durch s_userlock
         * cross-core serialisiert; im Demo bleibt der Aufruf dennoch auf Kern 0 (der Kern-1-
         * Prozess ist non-admin und wuerde ohnehin vor jedem DB-Zugriff abgewiesen). */
        long ua = sys3(SYS_USERADD, (long)"guest", (long)"guestpw", 0);
        uwrite("  >>> [Prozess ");
        uputn(pid);
        uwrite("] uid=");
        uputn(me);
        uwrite(ua == 0 ? " SYS_USERADD ok (Admin)\n"
                       : " SYS_USERADD verweigert (kein Admin)\n");
    } else {
        uwrite("  >>> [Prozess ");
        uputn(pid);
        uwrite("] uid=");
        uputn(me);
        uwrite(" FS-I/O auf Sekundaerkern (CPU=");
        uputn(cpu);
        uwrite(") ok\n");
    }

    uwrite("  >>> [Prozess ");
    uputn(pid);
    uwrite("] fertig -> exit\n");
    sys3(SYS_EXIT, 0, 0, 0);

    for (;;) {
    }
}
