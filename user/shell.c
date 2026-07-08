/*
 * user/shell.c  --  Einfache EL0-Kommando-Shell fuer rpi_rtos
 *
 * Wird vom Kernel nach erfolgreichem Login mit dem Credential (uid/caps) des
 * Benutzers gestartet. Kommuniziert nur ueber SVC-Syscalls. Die Built-ins nutzen
 * die capability-gateten Syscalls -> die Rechte des Benutzers werden sichtbar
 * (z.B. useradd nur als Admin; cat/run nur auf der User-Partition hdd1).
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

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/* Ausgabe-Umleitung (cmd > datei): waehrend ein Befehl mit aktiver Umleitung laeuft, sammelt
 * uwrite (und damit uputn) die Ausgabe in s_rd statt sie auf die Konsole zu schreiben; danach
 * schreibt _start den Puffer per SYS_WRITE_FILE in die Zieldatei. s_rd==0 -> normale Konsole. */
static char *s_rd;
static int   s_rd_len, s_rd_max;

static void uwrite(const char *s)
{
    if (s_rd) {
        while (*s && s_rd_len < s_rd_max) { s_rd[s_rd_len++] = *s++; }
    } else {
        sys3(SYS_WRITE, 1, (long)s, slen(s));
    }
}

static void uputn(long n)
{
    char buf[20];
    int  i = 0;
    if (n == 0) { uwrite("0"); return; }
    while (n > 0 && i < 20) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
    char out[2]; out[1] = '\0';
    while (i > 0) { out[0] = buf[--i]; uwrite(out); }
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* "Zeigen a und b auf DIESELBE Datei?" -- alias-bewusst: FAT32 vergleicht Namen
 * case-INsensitiv und der Pfad-Walker laesst leere Komponenten (//) zusammenfallen.
 * Ein reiner streq waere case-sensitiv -> 'mv FILE.TXT file.txt' wuerde die Datei zuerst
 * ueberschreiben und dann (weil streq sie als verschieden sieht) loeschen = Totalverlust.
 * Hier daher case-faltend + '/'-Folgen zusammenfassend vergleichen. */
static int same_file(const char *a, const char *b)
{
    for (;;) {
        if (*a == '/' && *b == '/') {            /* '/'-Folgen beidseitig kollabieren */
            while (*a == '/') { a++; }
            while (*b == '/') { b++; }
            continue;
        }
        if (lc(*a) != lc(*b)) { return 0; }
        if (!*a) { return 1; }
        a++; b++;
    }
}

static void scpy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int has_colon(const char *s)
{
    while (*s) { if (*s == ':') return 1; s++; }
    return 0;
}

/* Absolut = enthaelt ':' ODER ist ein reiner Mount-Name "hddN". */
static int is_absolute(const char *s)
{
    if (has_colon(s)) return 1;
    return s[0] == 'h' && s[1] == 'd' && s[2] == 'd' &&
           s[3] >= '0' && s[3] <= '9' && s[4] == '\0';
}

/* Aktuelles Arbeitsverzeichnis als "<mount>:<unterpfad>" (Wurzel: endet auf ':'). */
static char cwd[64] = "hdd1:";

/* Loest arg zu einem absoluten Pfad in out auf. arg mit ':'/Mount = absolut;
 * sonst relativ zu cwd. arg == 0 -> cwd selbst. */
static void resolve_path(const char *arg, char *out, int max)
{
    if (!arg || !*arg) { scpy(out, cwd, max); return; }
    if (is_absolute(arg)) { scpy(out, arg, max); return; }
    scpy(out, cwd, max);
    int n = 0;
    while (out[n]) n++;
    if (n > 0 && out[n - 1] != ':' && n < max - 1) { out[n++] = '/'; }
    int i = 0;
    while (arg[i] && n < max - 1) { out[n++] = arg[i++]; }
    out[n] = '\0';
}

/* --- Shell-seitiger Zeilen-Editor (Raw-Modus via SYS_READCHAR) ---
 * Die Shell liest jetzt Zeichen fuer Zeichen und macht Echo/Cursor/History/Tab SELBST --
 * so kann Tab-Vervollstaendigung die Befehlsliste UND das cwd kennen (was der Kernel nicht
 * koennte). console_readline (SYS_READ) bleibt fuer einfache, nicht-interaktive Leser. */
#define LINE_MAX 128

static long sys1(long n) { return sys3(n, 0, 0, 0); }
static void uputc(char c) { char s[1]; s[0] = c; sys3(SYS_WRITE, 1, (long)s, 1); }

/* Bekannte Befehle -- fuer Tab-Vervollstaendigung des ersten Tokens. */
static const char *const CMDS[] = {
    "help", "whoami", "id", "pwd", "cd", "ls", "cat", "cp", "mv",
    "mkdir", "rmdir", "rm", "passwd", "useradd", "run", "wait", "kill", "exit", "logout", 0
};

/* Befehls-Historie (Ringpuffer, EL0-seitig). */
#define HIST_N 8
static char s_hist[HIST_N][LINE_MAX];
static int  s_hist_n, s_hist_head;

static void hist_add(const char *l)
{
    if (!l[0]) { return; }
    if (s_hist_n) {
        const char *nw = s_hist[(s_hist_head - 1 + HIST_N) % HIST_N];
        if (streq(nw, l)) { return; }               /* aufeinanderfolgende Dublette */
    }
    scpy(s_hist[s_hist_head], l, LINE_MAX);
    s_hist_head = (s_hist_head + 1) % HIST_N;
    if (s_hist_n < HIST_N) { s_hist_n++; }
}
static const char *hist_at(int k)                   /* k-ter juengster (1..n) */
{
    if (k < 1 || k > s_hist_n) { return 0; }
    return s_hist[(s_hist_head - k + HIST_N) % HIST_N];
}

/* Ein Zeichen am Cursor einfuegen (Anzeige neu zeichnen). */
static void ed_ins(char *b, int *plen, int *ppos, int max, char ch)
{
    int len = *plen, pos = *ppos;
    if (len >= max - 1) { return; }
    if (pos == len) { b[len++] = ch; pos++; uputc(ch); }
    else {
        for (int k = len; k > pos; k--) { b[k] = b[k - 1]; }
        b[pos] = ch; len++;
        for (int k = pos; k < len; k++) { uputc(b[k]); }
        for (int k = len; k > pos + 1; k--) { uputc('\b'); }
        pos++;
    }
    *plen = len; *ppos = pos;
}
/* Zeichen VOR dem Cursor loeschen. */
static void ed_del(char *b, int *plen, int *ppos)
{
    int len = *plen, pos = *ppos;
    if (pos > 0) {
        for (int k = pos - 1; k + 1 < len; k++) { b[k] = b[k + 1]; }
        len--; pos--;
        uputc('\b');
        for (int k = pos; k < len; k++) { uputc(b[k]); }
        uputc(' ');
        for (int k = len + 1; k > pos; k--) { uputc('\b'); }
    }
    *plen = len; *ppos = pos;
}

static int has_prefix(const char *s, const char *p)
{
    while (*p) { if (*s != *p) { return 0; } s++; p++; }
    return 1;
}
/* Laengstes gemeinsames Praefix aller Kandidaten in lcp sammeln. */
static void lcp_acc(const char *cand, char *lcp, int *have)
{
    if (!*have) { scpy(lcp, cand, LINE_MAX); *have = 1; return; }
    int i = 0;
    while (lcp[i] && cand[i] && lcp[i] == cand[i]) { i++; }
    lcp[i] = '\0';
}

/* Tab-Vervollstaendigung des Tokens, das am Cursor endet. Erstes Token -> Befehlsname;
 * sonst -> Datei/Verzeichnis im implizierten Verzeichnis (relativ zum cwd oder absolut). */
static void tab_complete(char *b, int *plen, int *ppos, int max)
{
    int len = *plen, pos = *ppos;
    int ts = pos;
    while (ts > 0 && b[ts - 1] != ' ') { ts--; }      /* Token-Anfang */
    int first = 1;
    for (int k = 0; k < ts; k++) { if (b[k] != ' ') { first = 0; break; } }
    int pl = pos - ts;
    char partial[LINE_MAX];
    { int j = 0; for (; j < pl && j < LINE_MAX - 1; j++) { partial[j] = b[ts + j]; } partial[j] = '\0'; }

    char lcp[LINE_MAX]; int have = 0, matches = 0;
    int base_len = pl;                                /* Laenge des schon getippten Stuecks */

    if (first) {
        for (int ci = 0; CMDS[ci]; ci++) {
            if (has_prefix(CMDS[ci], partial)) { lcp_acc(CMDS[ci], lcp, &have); matches++; }
        }
    } else {
        /* Pfad: partial in Verzeichnis-Teil + Blatt zerlegen (am letzten '/' oder ':'). */
        int sep = -1;
        for (int k = 0; k < pl; k++) { if (partial[k] == '/' || partial[k] == ':') { sep = k; } }
        char leaf[LINE_MAX], listpath[LINE_MAX];
        if (sep < 0) { scpy(leaf, partial, LINE_MAX); scpy(listpath, cwd, LINE_MAX); }
        else {
            int j = 0;
            for (int k = sep + 1; k < pl && j < LINE_MAX - 1; k++) { leaf[j++] = partial[k]; }
            leaf[j] = '\0';
            char dirstr[LINE_MAX];
            int dn = 0;
            for (int k = 0; k <= sep && dn < LINE_MAX - 1; k++) { dirstr[dn++] = partial[k]; }
            dirstr[dn] = '\0';
            int L = 0; while (dirstr[L]) { L++; }
            if (L > 0 && dirstr[L - 1] == '/') { dirstr[L - 1] = '\0'; }   /* "X/" -> "X" */
            if (is_absolute(dirstr)) { scpy(listpath, dirstr, LINE_MAX); }
            else { resolve_path(dirstr, listpath, LINE_MAX); }
        }
        base_len = (int)slen(leaf);
        static char ls[1024];
        long r = sys3(SYS_LISTDIR, (long)listpath, (long)ls, sizeof(ls) - 1);
        if (r < 0) { return; }
        ls[r] = '\0';
        /* Zeilen -> Namen (bis zum ersten Leerzeichen) gegen das Blatt praefix-pruefen. */
        int i = 0;
        while (ls[i]) {
            char name[LINE_MAX]; int nj = 0;
            while (ls[i] && ls[i] != '\n' && ls[i] != ' ' && nj < LINE_MAX - 1) { name[nj++] = ls[i++]; }
            name[nj] = '\0';
            while (ls[i] && ls[i] != '\n') { i++; }   /* Rest der Zeile (Groesse/<DIR>) ueberspringen */
            if (ls[i] == '\n') { i++; }
            if (nj > 0 && has_prefix(name, leaf)) { lcp_acc(name, lcp, &have); matches++; }
        }
    }

    if (matches == 0) { return; }
    /* gemeinsames Praefix ueber das schon Getippte hinaus einfuegen */
    for (int k = base_len; lcp[k]; k++) { ed_ins(b, &len, &pos, max, lcp[k]); }
    if (matches == 1 && first) { ed_ins(b, &len, &pos, max, ' '); }   /* Befehl eindeutig -> Space */
    *plen = len; *ppos = pos;
}

/* Eine Zeile mit Editor (Echo/Cursor/History/Tab) lesen. Liefert die Laenge. */
static int read_line_edit(char *buf, int max)
{
    int len = 0, pos = 0, browse = 0;
    for (;;) {
        int ci = sys1(SYS_READCHAR);
        if (ci < 0) { continue; }
        char c = (char)ci;
        if (c == 0x1b) {                              /* CSI? */
            int b1 = sys1(SYS_READCHAR);
            if (b1 == '[') {
                int fb = sys1(SYS_READCHAR);
                while (fb >= 0x30 && fb <= 0x3F) { fb = sys1(SYS_READCHAR); }   /* Parameter */
                while (fb >= 0x20 && fb <= 0x2F) { fb = sys1(SYS_READCHAR); }   /* Intermediate */
                if (fb == 'A' || fb == 'B') {
                    int nb = browse + (fb == 'A' ? 1 : -1);
                    if (nb < 0) { nb = 0; }
                    if (nb > s_hist_n) { nb = s_hist_n; }
                    if (nb != browse) {
                        while (pos < len) { uputc(buf[pos]); pos++; }       /* Cursor ans Ende */
                        while (len > 0) { uwrite("\b \b"); len--; pos--; }  /* Zeile loeschen */
                        browse = nb;
                        if (browse == 0) { buf[0] = '\0'; }
                        else {
                            scpy(buf, hist_at(browse), max);
                            len = (int)slen(buf);
                            for (int k = 0; k < len; k++) { uputc(buf[k]); }
                        }
                        pos = len;
                    }
                } else if (fb == 'D') { if (pos > 0) { pos--; uputc('\b'); } }
                else if (fb == 'C') { if (pos < len) { uputc(buf[pos]); pos++; } }
                continue;
            }
            if (b1 < 0) { continue; }
            c = (char)b1;                             /* Einzel-ESC: Folgebyte normal behandeln */
        }
        /* Gemeinsame Verarbeitung -- auch fuer das nach einem Einzel-ESC synthetisierte Byte
         * (sonst wuerde z.B. ESC dann Enter die Zeile NICHT abschicken). */
        if (c == '\r' || c == '\n') {
            while (pos < len) { uputc(buf[pos]); pos++; }
            uputc('\n');
            break;
        }
        if (c == '\t') { tab_complete(buf, &len, &pos, max); continue; }
        if (c == 8 || c == 127) { ed_del(buf, &len, &pos); }
        else if (c >= 32 && c < 127) { ed_ins(buf, &len, &pos, max, c); }
    }
    buf[len] = '\0';
    hist_add(buf);
    return len;
}

/* PID des zuletzt per `run` gestarteten Kindes -> `wait`/`kill` ohne Argument beziehen sich darauf. */
static long s_last_child;

/* Dezimalzahl parsen (nur Ziffern); -1 wenn kein fuehrendes Ziffernzeichen. */
static long atou(const char *s)
{
    if (!s || s[0] < '0' || s[0] > '9') { return -1; }
    long v = 0;
    for (; *s >= '0' && *s <= '9'; ++s) {
        v = v * 10 + (*s - '0');
    }
    return v;
}

/* Einen Befehl ausfuehren. Per `return` (nicht `continue`) beenden, damit der Aufrufer
 * danach immer eine evtl. aktive Ausgabe-Umleitung in die Datei schreiben kann. */
static void run_command(int argc, char *argv[])
{
    char path[64];
    if (streq(argv[0], "help")) {
        uwrite("Befehle: help  whoami  id  pwd  cd <verz>  ls [pfad]  cat <datei>  "
               "cp <q> <z>  mv <q> <z>  mkdir <verz>  rmdir <verz>  rm <datei>  "
               "passwd <neu>  useradd <name> <pw>  run <pfad> [kern]  wait [pid]  kill [pid]  exit\n"
               "Pfade absolut (hdd1:DOCS/X) oder relativ zum cwd. 'cmd > datei' leitet die "
               "Ausgabe um.\n");
    } else if (streq(argv[0], "whoami") || streq(argv[0], "id")) {
        uwrite("uid=");
        uputn(sys3(SYS_WHOAMI, 0, 0, 0));
        uwrite("\n");
    } else if (streq(argv[0], "pwd")) {
        uwrite(cwd);
        uwrite("\n");
    } else if (streq(argv[0], "cd")) {
        if (argc < 2) { uwrite("cd: <verz>\n"); return; }
        char cand[64];
        if (streq(argv[1], "..")) {           /* eine Ebene hoch */
            scpy(cand, cwd, sizeof(cand));
            int n = 0; while (cand[n]) n++;
            int colon = 0; while (cand[colon] && cand[colon] != ':') colon++;
            if (cand[colon] == ':') {
                int slash = -1;
                for (int i = colon + 1; i < n; i++) { if (cand[i] == '/') slash = i; }
                if (slash >= 0) { cand[slash] = '\0'; }   /* letzte Komponente weg */
                else { cand[colon + 1] = '\0'; }          /* zur Wurzel */
            }
        } else {
            resolve_path(argv[1], cand, sizeof(cand));
        }
        if (!has_colon(cand)) {               /* "hddN" -> "hddN:" normalisieren */
            int n = 0; while (cand[n]) n++;
            if (n < (int)sizeof(cand) - 1) { cand[n] = ':'; cand[n + 1] = '\0'; }
        }
        static char tb[64];
        long r = sys3(SYS_LISTDIR, (long)cand, (long)tb, sizeof(tb));
        if (r < 0) { uwrite("cd: kein Verzeichnis\n"); return; }
        scpy(cwd, cand, sizeof(cwd));
    } else if (streq(argv[0], "cat")) {
        if (argc < 2) { uwrite("cat: Datei?\n"); return; }
        resolve_path(argv[1], path, sizeof(path));
        static char b[256];
        long r = sys3(SYS_READ_FILE, (long)path, (long)b, sizeof(b) - 1);
        if (r < 0) {
            uwrite("cat: Fehler (nur hdd1/hdd2-Dateien erlaubt)\n");
        } else {
            if (r > (long)sizeof(b) - 1) { r = (long)sizeof(b) - 1; }   /* r = wahre Groesse -> klemmen (Schnell-Viewer) */
            b[r] = '\0';
            uwrite(b);
            if (r == 0 || b[r - 1] != '\n') { uwrite("\n"); }
        }
    } else if (streq(argv[0], "ls")) {
        resolve_path(argc >= 2 ? argv[1] : 0, path, sizeof(path));
        static char lb[1024];
        long r = sys3(SYS_LISTDIR, (long)path, (long)lb, sizeof(lb) - 1);
        if (r < 0) {
            uwrite("ls: Fehler (nur hdd1/hdd2)\n");
        } else {
            lb[r] = '\0';
            uwrite(lb);
            if (r == 0) { uwrite("(leer)\n"); }
        }
    } else if (streq(argv[0], "mkdir")) {
        if (argc < 2) { uwrite("mkdir: <verz>\n"); return; }
        resolve_path(argv[1], path, sizeof(path));
        long r = sys3(SYS_MKDIR, (long)path, 0, 0);
        uwrite(r == 0 ? "mkdir: ok\n" : "mkdir: fehlgeschlagen\n");
    } else if (streq(argv[0], "rmdir")) {
        if (argc < 2) { uwrite("rmdir: <verz>\n"); return; }
        resolve_path(argv[1], path, sizeof(path));
        long r = sys3(SYS_RMDIR, (long)path, 0, 0);
        uwrite(r == 0 ? "rmdir: ok\n" : "rmdir: fehlgeschlagen (nicht leer/kein Verz.)\n");
    } else if (streq(argv[0], "rm")) {
        if (argc < 2) { uwrite("rm: <datei>\n"); return; }
        resolve_path(argv[1], path, sizeof(path));
        long r = sys3(SYS_DELETE, (long)path, 0, 0);
        uwrite(r == 0 ? "rm: ok\n" : "rm: Fehler (nur hdd1/hdd2-Dateien)\n");
    } else if (streq(argv[0], "cp") || streq(argv[0], "mv")) {
        /* cp = lesen+schreiben (nur hdd1/hdd2, via Datei-Syscalls). mv = cp + Quelle
         * loeschen. Auf die cpbuf-Groesse begrenzt (groessere Dateien -> Warnung). */
        int is_mv = (argv[0][0] == 'm');
        if (argc < 3) { uwrite(is_mv ? "mv: <quelle> <ziel>\n" : "cp: <quelle> <ziel>\n"); return; }
        char src[64], dst[64];
        resolve_path(argv[1], src, sizeof(src));
        resolve_path(argv[2], dst, sizeof(dst));
        static char cpbuf[2048];
        long rn = sys3(SYS_READ_FILE, (long)src, (long)cpbuf, sizeof(cpbuf));
        if (rn < 0) { uwrite("cp: Quelle nicht lesbar (nur hdd1/hdd2)\n"); return; }
        /* rn = WAHRE Dateigroesse: rn > cpbuf heisst, die Datei ist groesser als der Puffer (nur
         * die ersten cpbuf Bytes wurden gelesen) -> praezise Trunkierungserkennung (vorher rief
         * eine exakt 2-KiB-Datei faelschlich "abgeschnitten"). Geschrieben wird nur min(rn, cpbuf). */
        int  truncated = (rn > (long)sizeof(cpbuf));
        long wlen      = truncated ? (long)sizeof(cpbuf) : rn;
        /* mv darf bei Abschneidung NICHT loeschen (sonst Datenverlust >2 KiB). VOR dem
         * Schreiben/Loeschen pruefen -> Quelle bleibt unangetastet. */
        if (truncated && is_mv) {
            uwrite("mv: Datei >2 KiB -- abgebrochen (Quelle unveraendert; cp kopiert die ersten 2 KiB)\n");
            return;
        }
        long wn = sys3(SYS_WRITE_FILE, (long)dst, (long)cpbuf, wlen);
        if (wn < 0) { uwrite("cp: Ziel nicht schreibbar (nur hdd1/hdd2)\n"); return; }
        if (truncated) { uwrite("cp: WARNUNG Datei abgeschnitten auf 2 KiB (>2 KiB)\n"); }
        if (is_mv) {
            if (!same_file(src, dst)) {        /* alias-bewusst: schuetzt vor Selbst-Loeschung */
                long dr = sys3(SYS_DELETE, (long)src, 0, 0);
                uwrite(dr == 0 ? "mv: ok\n" : "mv: kopiert, Quelle aber nicht geloescht\n");
            } else {
                uwrite("mv: ok (Quelle = Ziel, kein Loeschen)\n");
            }
        } else {
            uwrite("cp: ok\n");
        }
    } else if (streq(argv[0], "passwd")) {
        if (argc < 2) { uwrite("passwd: <neues-passwort>\n"); return; }
        long r = sys3(SYS_PASSWD, (long)argv[1], 0, 0);
        uwrite(r == 0 ? "passwd: Passwort geaendert\n" : "passwd: fehlgeschlagen\n");
    } else if (streq(argv[0], "useradd")) {
        if (argc < 3) { uwrite("useradd: <name> <pw>\n"); return; }
        long r = sys3(SYS_USERADD, (long)argv[1], (long)argv[2], 0);
        uwrite(r == 0 ? "useradd: ok\n"
                      : "useradd: fehlgeschlagen (Admin-Recht noetig oder Name vergeben)\n");
    } else if (streq(argv[0], "run")) {
        if (argc < 2) { uwrite("run: <pfad> [kern 0..3]\n"); return; }
        resolve_path(argv[1], path, sizeof(path));
        /* Optionaler Kern (Affinitaet): 'run prog 1' startet auf Kern 1 (SMP). Ungueltige
         * Angabe wird gemeldet (nicht stillschweigend auf Kern 0 umgebogen). */
        long core = 0;
        if (argc >= 3) {
            if (argv[2][0] >= '0' && argv[2][0] <= '3' && argv[2][1] == '\0') {
                core = argv[2][0] - '0';
            } else {
                uwrite("run: Kern muss 0..3 sein\n");
                return;
            }
        }
        long r = sys3(SYS_SPAWN, (long)path, core, 0);
        if (r < 0) { uwrite("run: Fehler (nur hdd1/hdd2-Programme, Kern 0..3)\n"); }
        else { s_last_child = r; uwrite("run: gestartet (pid="); uputn(r); uwrite(")\n"); }
    } else if (streq(argv[0], "wait")) {
        /* Auf ein eigenes Kind warten (Default: das zuletzt gestartete) -> dessen Exit-Code. */
        long tp = (argc >= 2) ? atou(argv[1]) : s_last_child;
        if (tp <= 0) { uwrite("wait: kein Kind (erst 'run')\n"); return; }
        long code = sys3(SYS_WAIT, tp, 0, 0);
        if (code < 0) { uwrite("wait: kein eigenes Kind pid="); uputn(tp); uwrite("\n"); }
        else { uwrite("wait: Kind pid="); uputn(tp); uwrite(" endete, code="); uputn(code); uwrite("\n"); }
    } else if (streq(argv[0], "kill")) {
        /* Ein eigenes Kind beenden (Default: das zuletzt gestartete). */
        long tp = (argc >= 2) ? atou(argv[1]) : s_last_child;
        if (tp <= 0) { uwrite("kill: kein Kind (erst 'run')\n"); return; }
        long r = sys3(SYS_KILL, tp, 0, 0);
        uwrite(r == 0 ? "kill: ok\n" : "kill: fehlgeschlagen (nur eigene Kinder / Admin)\n");
    } else if (streq(argv[0], "exit") || streq(argv[0], "logout")) {
        uwrite("abgemeldet.\n");
        sys3(SYS_EXIT, 0, 0, 0);
    } else {
        uwrite("unbekannt: ");
        uwrite(argv[0]);
        uwrite(" ('help')\n");
    }
}

void _start(void)
{
    long uid = sys3(SYS_WHOAMI, 0, 0, 0);
    uwrite("\nrpi_rtos Shell -- 'help' fuer Befehle (Tab vervollstaendigt, '> datei' leitet um).\n");

    static char line[LINE_MAX];
    static char rdbuf[2048];
    for (;;) {
        uwrite("\x1b[36mrpi[uid");        /* Prompt in Cyan (ANSI) -> Serial + HDMI */
        uputn(uid);
        uwrite(" ");
        uwrite(cwd);                      /* aktuelles Arbeitsverzeichnis */
        uwrite("]$ \x1b[0m");
        long n = read_line_edit(line, sizeof(line));
        if (n <= 0) {
            continue;
        }

        /* Ausgabe-Umleitung erkennen: das erste '>' trennt Befehl von Zieldatei (FAT-Namen
         * enthalten kein '>'). 'ls > f' wie 'ls>f'. */
        char *redir = 0;
        {
            char *g = line;
            while (*g && *g != '>') { g++; }
            if (*g == '>') {
                *g++ = '\0';                          /* Befehlsteil terminieren */
                while (*g == ' ') { g++; }
                redir = g;
                char *e = redir; while (*e) { e++; }
                while (e > redir && e[-1] == ' ') { *--e = '\0'; }
                if (!*redir) { redir = 0; }           /* 'ls >' ohne Datei -> ignorieren */
            }
        }

        /* Befehlsteil in bis zu 3 Tokens (whitespace) zerlegen, in-place. */
        char *argv[3] = { 0, 0, 0 };
        int   argc = 0;
        char *p = line;
        while (*p && argc < 3) {
            while (*p == ' ') { p++; }
            if (!*p) { break; }
            argv[argc++] = p;
            while (*p && *p != ' ') { p++; }
            if (*p) { *p++ = '\0'; }
        }
        if (argc == 0) {
            continue;
        }

        char rp[64];
        if (redir) {
            /* Ziel relativ zum cwd VOR dem Befehl aufloesen (Standard-Shell-Semantik) --
             * sonst wuerde 'cd X > log' das log im NEUEN Verzeichnis anlegen. */
            resolve_path(redir, rp, sizeof(rp));
            s_rd = rdbuf; s_rd_len = 0; s_rd_max = sizeof(rdbuf);
        }
        run_command(argc, argv);
        if (redir) {
            s_rd = 0;                                 /* Konsolen-Ausgabe wieder aktiv */
            long wn = sys3(SYS_WRITE_FILE, (long)rp, (long)rdbuf, s_rd_len);
            if (wn < 0) { uwrite("Umleitung: Fehler (nur hdd1/hdd2 beschreibbar)\n"); }
            else { uwrite("-> "); uwrite(redir); uwrite(" ("); uputn(wn); uwrite(" Bytes)\n"); }
        }
    }
}
