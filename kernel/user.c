/*
 * kernel/user.c  --  Benutzerverwaltung: Konten, PBKDF2-Auth, Capabilities
 *
 * DB-Datei hdd0:USERS.DB (Systempartition), eine Zeile je Konto:
 *   name:saltHex(32):hashHex(64):uidDez:capsDez
 * Passwort-Hash = PBKDF2-HMAC-SHA256(passwort, salt, ITERS). Klartext wird nie
 * gespeichert. Aenderungen gehen ueber vfs_write_file_priv (privilegiert).
 *
 * Hinweis: Der minimale FAT32-Treiber kennt keine Unterverzeichnisse, daher
 * liegt die DB als Root-Datei (Architektur sah hdd0:/sys/users/ vor).
 */
#include <stdint.h>
#include "user.h"
#include "crypto.h"
#include "vfs.h"
#include "kmem.h"
#include "uart.h"
#include "usbkbd.h"
#include "aarch64.h"
#include "spinlock.h"

/* Cross-core-Lock fuer die Benutzer-DB: s_users/s_nusers/s_next_uid (in-memory) UND der
 * CSPRNG (rng_fill, nur hier genutzt) sind global. Mit SMP koennen Datei-Syscalls
 * (SYS_USERADD/SYS_PASSWD) von mehreren Kernen kommen -> jede DB-Operation laeuft unter
 * diesem Lock (IRQs maskiert: der Lock wird aus preemptierbarem Syscall-Kontext genommen).
 * Verschachtelt mit dem VFS-Lock: IMMER user (aussen) -> fs (innen, in save_db/vfs), nie
 * umgekehrt -> keine Lock-Ordnungs-Deadlocks. */
static spinlock_t s_userlock = SPINLOCK_INIT;

static uint64_t user_lock(void)
{
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_userlock);
    return f;
}
static void user_unlock(uint64_t f)
{
    spin_unlock(&s_userlock);
    WRITE_SYSREG(daif, f);
}

#define DB_PATH      "hdd0:USERS.DB"
#define BAK_PATH     "hdd0:USERS.BAK"   /* Backup fuer crash-sicheres Schreiben (save_db_to) */
#define DB_MAGIC     "#RTOSDB1:"        /* Integritaets-Header: Magic + 10-stellige Nutzlastlaenge + '\n' */
#define DB_MAGIC_LEN 9
#define DB_HDR_LEN   20                 /* 9 (Magic) + 10 (Laenge, zero-padded dezimal) + 1 ('\n') */
#define MAX_USERS    16
#define SALT_LEN     16
#define HASH_LEN     32
#define PBKDF2_ITERS 50000       /* Latenz/Sicherheits-Kompromiss; produktiv hoeher (OWASP ~600k) */

typedef struct {
    char     name[USER_NAME_MAX + 1];
    uint32_t uid;
    uint32_t caps;
    uint8_t  salt[SALT_LEN];
    uint8_t  hash[HASH_LEN];
    uint8_t  must_change;     /* 1 = Passwort muss beim naechsten Login gesetzt werden
                               * (Default-/Erstpasswort) -> login_console erzwingt es */
} urec_t;

static urec_t   s_users[MAX_USERS];
static int      s_nusers;
static uint32_t s_next_uid;
static char     s_dbbuf[4096];

/* --- kleine String-/Hex-Helfer --- */
static uint32_t slen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Zentrale Passwort-Mindestpolicy: >= 4 Zeichen und nicht das bekannte Default 'admin'.
 * Wird von JEDEM Aenderungspfad (user_change_password -> auch SYS_PASSWD/Shell und der
 * erzwungene Login-Wechsel) durchgesetzt, damit die Staerke nicht nur kosmetisch ist. */
static int weak_password(const char *pw)
{
    return !pw || slen(pw) < 4 || streq(pw, "admin");
}

static int name_ok(const char *n)
{
    uint32_t l = slen(n);
    if (l == 0 || l > USER_NAME_MAX) {
        return 0;
    }
    for (uint32_t i = 0; i < l; i++) {
        if (n[i] == ':' || n[i] == '\n' || n[i] == '\r') {
            return 0;
        }
    }
    return 1;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void to_hex(const uint8_t *in, int n, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2]     = h[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = h[in[i] & 0xF];
    }
}

static int from_hex(const char *in, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int hi = hexv(in[i * 2]), lo = hexv(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static uint32_t parse_dec(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/* 1, wenn s nichtleer und vollstaendig dezimal ist. */
static int is_dec(const char *s)
{
    if (!*s) {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
    }
    return 1;
}

static int put_dec(char *out, uint32_t v)
{
    char t[10];
    int n = 0;
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) {
        out[i] = t[n - 1 - i];
    }
    return n;
}

/* Konstantzeit-Vergleich (gegen Timing-Seitenkanaele beim Hash-Check). */
static int ct_eq(const uint8_t *a, const uint8_t *b, int n)
{
    uint8_t d = 0;
    for (int i = 0; i < n; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

static urec_t *find_user(const char *name)
{
    for (int i = 0; i < s_nusers; i++) {
        if (streq(s_users[i].name, name)) {
            return &s_users[i];
        }
    }
    return 0;
}

/* --- Serialisierung --- */
static int build_db(void)
{
    int pos = DB_HDR_LEN;                              /* Records HINTER dem Integritaets-Header */
    for (int i = 0; i < s_nusers; i++) {
        urec_t *u = &s_users[i];
        uint32_t nl = slen(u->name);
        /* grobe Platzpruefung: Name + 2 + 32 + 1 + 64 + 1 + 10 + 1 + 10 + 1 + 1 + 1 (must_change) */
        if (pos + (int)nl + 134 > (int)sizeof(s_dbbuf)) {
            return -1;
        }
        memcpy(&s_dbbuf[pos], u->name, nl); pos += (int)nl;
        s_dbbuf[pos++] = ':';
        to_hex(u->salt, SALT_LEN, &s_dbbuf[pos]); pos += SALT_LEN * 2;
        s_dbbuf[pos++] = ':';
        to_hex(u->hash, HASH_LEN, &s_dbbuf[pos]); pos += HASH_LEN * 2;
        s_dbbuf[pos++] = ':';
        pos += put_dec(&s_dbbuf[pos], u->uid);
        s_dbbuf[pos++] = ':';
        pos += put_dec(&s_dbbuf[pos], u->caps);
        s_dbbuf[pos++] = ':';                          /* 6. Feld: must_change (0/1) */
        pos += put_dec(&s_dbbuf[pos], u->must_change);
        s_dbbuf[pos++] = '\n';
    }
    /* Header vorne einsetzen: DB_MAGIC + 10-stellige Nutzlastlaenge (Records-Bytes) + '\n'.
     * Die Laenge erlaubt dem Leser, eine trunkierte (torn-write) Datei zu erkennen. */
    uint32_t payload = (uint32_t)(pos - DB_HDR_LEN);
    memcpy(s_dbbuf, DB_MAGIC, DB_MAGIC_LEN);
    for (int d = 0; d < 10; d++) {
        s_dbbuf[DB_MAGIC_LEN + 9 - d] = (char)('0' + payload % 10);
        payload /= 10;
    }
    s_dbbuf[DB_HDR_LEN - 1] = '\n';
    return pos;                                        /* Gesamtlaenge inkl. Header */
}

/* Crash-sicheres Schreiben OHNE rename (FAT32 kann kein atomares rename): erst das Backup
 * VOLLSTAENDIG schreiben, DANN das Primaer. Invariante: es wird immer nur EINE Datei zugleich
 * geschrieben, die andere bleibt vollstaendig -> mindestens eine ist stets eine konsistente DB.
 * Torn-Write ins Primaer -> Backup traegt den neuen Stand; Torn-Write ins Backup -> Primaer
 * traegt noch den alten Stand. Backup-first (nicht primary-first) ist bewusst: bei einem
 * FEHLGESCHLAGENEN Save (Primaer-Write scheitert) bevorzugt der Leser das unveraenderte Primaer
 * -> der Effektivzustand ist sofort konsistent zum RAM-Rollback des Aufrufers.
 *
 * BEKANNTE GRENZEN (bewusst, RC-vertretbar; siehe docs/architecture/14-rc-roadmap.md):
 *  - (a) Schlaegt der Primaer-Write NACH erfolgreichem Backup-Write fehl, liegt das Backup
 *    'voraus' (neuer, vom Aufrufer zurueckgerollter Stand). Er wird nur live, wenn das Primaer
 *    SPAETER zusaetzlich korrumpiert und der Fallback greift -- und betrifft nur admin-intendierte
 *    Konten/Passwoerter, nie ein bekanntes Default. Ein spaeterer erfolgreicher Save heilt es.
 *  - (b) Zwei vollstaendige FAT32-Schreibvorgaenge laufen unter s_userlock (IRQs maskiert) ->
 *    verdoppelte Worst-Case-IRQ-off-Latenz eines DB-Writes. DB-Schreiben ist selten (Login-
 *    Passwortwechsel, useradd) und nicht im RT-Hotpath. */
static int save_db_to(const char *primary, const char *backup)
{
    int n = build_db();
    if (n < 0) {
        return -1;
    }
    if (vfs_write_file_priv(backup, s_dbbuf, (uint32_t)n) < 0) {
        return -1;
    }
    if (vfs_write_file_priv(primary, s_dbbuf, (uint32_t)n) < 0) {
        return -1;
    }
    return 0;
}

static int save_db(void)
{
    return save_db_to(DB_PATH, BAK_PATH);
}

/* Eine Zeile (null-terminiert, ohne '\n') in einen Record parsen. */
static int parse_line(char *line)
{
    if (s_nusers >= MAX_USERS) {
        return -1;
    }
    /* Felder durch ':' trennen (in-place). 6. Feld (must_change) ist optional ->
     * 5-Feld-Altzeilen bleiben lesbar (must_change defaultet dann auf 0). */
    char *f[6];
    int nf = 0;
    f[nf++] = line;
    for (char *p = line; *p && nf < 6; p++) {
        if (*p == ':') {
            *p = '\0';
            f[nf++] = p + 1;
        }
    }
    if (nf != 5 && nf != 6) {
        return -1;
    }
    if (!name_ok(f[0]) || slen(f[1]) != SALT_LEN * 2 || slen(f[2]) != HASH_LEN * 2 ||
        !is_dec(f[3]) || !is_dec(f[4]) || (nf == 6 && !is_dec(f[5]))) {
        return -1;                         /* uid/caps/must_change muessen rein dezimal sein */
    }
    if (find_user(f[0])) {
        return -1;                         /* Namens-Dublette in der DB verwerfen */
    }
    urec_t *u = &s_users[s_nusers];
    memset(u, 0, sizeof(*u));
    memcpy(u->name, f[0], slen(f[0]));
    if (from_hex(f[1], u->salt, SALT_LEN) != 0 ||
        from_hex(f[2], u->hash, HASH_LEN) != 0) {
        return -1;
    }
    u->uid  = parse_dec(f[3]);
    u->caps = parse_dec(f[4]);
    u->must_change = (nf == 6) ? (parse_dec(f[5]) ? 1 : 0) : 0;
    if (u->uid != 0xFFFFFFFFu && u->uid + 1 > s_next_uid) {
        s_next_uid = u->uid + 1;           /* wrap-sicher fortschalten */
    }
    s_nusers++;
    return 0;
}

/* Parst die Record-Zeilen im Bereich s_dbbuf[from, to) (hinter dem Integritaets-Header).
 * Rueckgabe: 0 = alle nicht-leeren Zeilen gueltig, -1 = mindestens eine Zeile ungueltig.
 * Eine ungueltige Zeile bedeutet Korruption (build_db erzeugt nie so etwas) -> der Aufrufer
 * verwirft die Datei als korrupt, statt die kaputte Zeile still zu ueberspringen (sonst wuerde
 * ein Konto lautlos verschwinden UND der Backup-Fallback nie greifen). */
static int parse_records(int from, int to)
{
    int ok = 0;
    int i = from;
    while (i < to) {
        int start = i;
        while (i < to && s_dbbuf[i] != '\n') {
            i++;
        }
        s_dbbuf[i] = '\0';                 /* Zeilenende terminieren */
        if (i > start) {
            if (parse_line(&s_dbbuf[start]) != 0) {
                ok = -1;                   /* ungueltige Zeile -> Datei gilt als korrupt */
            }
        }
        i++;
    }
    return ok;
}

/* Laedt EINE DB-Datei in s_users (setzt s_nusers/s_next_uid vorher zurueck). Rueckgabe:
 *   >0 = geladen (Anzahl Konten), 0 = genuin abwesend/leer, -1 = vorhanden aber korrupt. */
static int load_db(const char *path)
{
    s_nusers = 0;
    s_next_uid = 0;

    int n = vfs_read_file(path, s_dbbuf, sizeof(s_dbbuf) - 1);
    if (n == VFS_ENOENT) {
        return 0;                          /* Datei GENUIN nicht vorhanden -> Erstboot-Kandidat */
    }
    if (n < 0) {
        return -1;                         /* Lese-/Korruptionsfehler bei vorhandener Datei */
    }
    if (n == 0) {
        /* Datei existiert, ist aber 0 Byte. Eine gueltige DB ist immer >= DB_HDR_LEN (Header).
         * NICHT als Erstboot behandeln (sonst Downgrade auf Default-admin durch Truncate-auf-0). */
        return -1;
    }
    if (n > (int)(sizeof(s_dbbuf) - 1)) {
        return -1;                         /* groesser als Puffer -> nur Teil gelesen -> korrupt */
    }
    /* Integritaets-Header pruefen: Magic + 10-stellige Nutzlastlaenge + '\n'. */
    if (n < DB_HDR_LEN || memcmp(s_dbbuf, DB_MAGIC, DB_MAGIC_LEN) != 0
        || s_dbbuf[DB_HDR_LEN - 1] != '\n') {
        return -1;                         /* Header fehlt/kaputt */
    }
    uint32_t declared = 0;
    for (int i = DB_MAGIC_LEN; i < DB_HDR_LEN - 1; i++) {
        if (s_dbbuf[i] < '0' || s_dbbuf[i] > '9') {
            return -1;
        }
        declared = declared * 10 + (uint32_t)(s_dbbuf[i] - '0');
    }
    if ((uint32_t)n != DB_HDR_LEN + declared) {
        return -1;                         /* deklarierte != tatsaechliche Laenge -> Torn-Write */
    }
    if (parse_records(DB_HDR_LEN, n) != 0) {
        return -1;                         /* ungueltige Zeile -> Datei korrupt -> Backup-Fallback */
    }
    if (s_nusers == 0) {
        return -1;                         /* Header ok, aber keine gueltige Zeile */
    }
    return s_nusers;
}

/* Laedt die DB mit Backup-Fallback (crash-sicheres Gegenstueck zu save_db_to). Rueckgabe:
 *   1 = aus Primaer geladen, 2 = Primaer unbrauchbar -> aus Backup wiederhergestellt (+ geheilt),
 *   0 = beide genuin abwesend (Erstboot), -1 = beide vorhanden-aber-korrupt (fail-closed). */
static int db_load_recover(const char *primary, const char *backup)
{
    int r = load_db(primary);
    if (r > 0) {
        return 1;
    }
    int rb = load_db(backup);
    if (rb > 0) {
        /* Nur das PRIMAER aus dem geladenen (Backup-)Stand heilen -- das gute Backup wird
         * NICHT angefasst. save_db_to hier zu verwenden waere falsch: es schreibt das Backup
         * zuerst und wuerde die einzige intakte Kopie einem Torn-Write aussetzen (Crash ->
         * beide korrupt -> Lockout). build_db serialisiert die soeben aus dem Backup geladenen
         * s_users neu (parse_records hat s_dbbuf zerlegt). */
        int hn = build_db();
        if (hn > 0) {
            (void)vfs_write_file_priv(primary, s_dbbuf, (uint32_t)hn);
        }
        return 2;
    }
    if (r == 0 && rb == 0) {
        return 0;                            /* beide abwesend -> Erstboot */
    }
    return -1;                               /* mind. eine korrupt, keine gueltige Kopie */
}

/* Interner Add ohne Rechtepruefung (fuer Default-admin + user_add). must_change=1
 * markiert das Konto so, dass login_console beim ersten Login einen Passwortwechsel erzwingt. */
static int add_internal(const char *name, const char *password, uint32_t caps, int must_change)
{
    if (s_nusers >= MAX_USERS || !name_ok(name) || find_user(name)) {
        return -1;
    }
    urec_t *u = &s_users[s_nusers];
    memset(u, 0, sizeof(*u));
    memcpy(u->name, name, slen(name));
    u->uid = s_next_uid++;
    u->caps = caps;
    u->must_change = must_change ? 1 : 0;
    rng_fill(u->salt, SALT_LEN);
    pbkdf2_sha256((const uint8_t *)password, slen(password),
                  u->salt, SALT_LEN, PBKDF2_ITERS, u->hash, HASH_LEN);
    s_nusers++;
    return 0;
}

/* 1, wenn das Konto mit dieser uid beim Login einen Passwortwechsel erzwingen muss. */
int user_must_change(uint32_t uid)
{
    uint64_t f = user_lock();
    int r = 0;
    for (int i = 0; i < s_nusers; i++) {
        if (s_users[i].uid == uid) { r = s_users[i].must_change ? 1 : 0; break; }
    }
    user_unlock(f);
    return r;
}

/* Aendert das Passwort des Kontos mit der gegebenen uid (neues Salt + PBKDF2).
 * Persistiert die DB; bei Schreibfehler Rollback. 0 = ok, -1 = Fehler. */
int user_change_password(uint32_t uid, const char *newpw)
{
    if (weak_password(newpw)) {        /* Policy zentral hier -> auch SYS_PASSWD/Shell deckt sie ab */
        return -1;
    }
    /* Phase 1 (unter Lock): existiert die uid? + frisches Salt ziehen (CSPRNG). */
    uint8_t salt[SALT_LEN];
    uint64_t f = user_lock();
    int found = 0;
    for (int i = 0; i < s_nusers; i++) {
        if (s_users[i].uid == uid) { found = 1; break; }
    }
    if (found) {
        rng_fill(salt, SALT_LEN);
    }
    user_unlock(f);
    if (!found) {
        return -1;
    }
    /* Phase 2 (OHNE Lock): teures PBKDF2. */
    uint8_t hash[HASH_LEN];
    pbkdf2_sha256((const uint8_t *)newpw, slen(newpw),
                  salt, SALT_LEN, PBKDF2_ITERS, hash, HASH_LEN);
    /* Phase 3 (unter Lock): festschreiben + persistieren (Re-Find + Rollback). */
    uint64_t f2 = user_lock();
    int ret = -1;
    urec_t *u = 0;
    for (int i = 0; i < s_nusers; i++) {
        if (s_users[i].uid == uid) { u = &s_users[i]; break; }
    }
    if (u) {
        urec_t saved = *u;                       /* Snapshot fuer Rollback */
        memcpy(u->salt, salt, SALT_LEN);
        memcpy(u->hash, hash, HASH_LEN);
        u->must_change = 0;                      /* Wechsel erfolgt -> Flag loeschen */
        ret = (save_db() != 0) ? (*u = saved, -1) : 0;
    }
    user_unlock(f2);
    return ret;
}

int user_init(void)
{
    /* DB mit Backup-Fallback laden: Primaer USERS.DB, bei Unbrauchbarkeit USERS.BAK.
     * db_load_recover fuellt bei Erfolg s_users/s_nusers. */
    int r = db_load_recover(DB_PATH, BAK_PATH);
    if (r == 1) {
        uart_puts("    [user] DB geladen (");
        uart_putdec((unsigned)s_nusers);
        uart_puts(" Konten)\n");
        return 0;
    }
    if (r == 2) {
        /* Primaer war unbrauchbar (Torn-Write/Korruption) -> aus dem Backup wiederhergestellt
         * und das Primaer geheilt. Kein Credential-Verlust, kein Default-Downgrade. */
        uart_puts("    [user] Primaer-DB unbrauchbar -> aus Backup USERS.BAK wiederhergestellt (");
        uart_putdec((unsigned)s_nusers);
        uart_puts(" Konten)\n");
        return 0;
    }
    if (r == 0) {
        /* Beide Dateien genuin abwesend -> echter Erstboot: Default-Konto anlegen
         * (must_change=1 erzwingt den Passwortwechsel beim ersten interaktiven Login). */
        s_nusers = 0;
        s_next_uid = 0;
        if (add_internal("admin", "admin", USER_CAP_ADMIN, /*must_change=*/1) != 0 || save_db() != 0) {
            uart_puts("    [user] FEHLER: DB konnte nicht angelegt werden\n");
            return -1;
        }
        uart_puts("    [user] DB erstellt: Default-Konto 'admin' (Passwort 'admin', Wechsel erzwungen)\n");
        return 0;
    }

    /* r == -1: mindestens eine Datei war VORHANDEN-aber-korrupt und es gibt keine gueltige
     * Kopie. NICHT mit Default-'admin/admin' ueberschreiben (sonst Credential-Downgrade auf
     * bekannte Default-Credentials fuer jeden, der beide Dateien korrumpieren kann).
     * Fail-closed: melden, NICHT anlegen -- das kmain-Gate haelt den Boot an. */
    uart_puts("    [user] FEHLER: USERS.DB und USERS.BAK unbrauchbar (korrupt) -- fail-closed\n");
    return -1;
}

#ifdef RTOS_SELFTEST
/* Selbsttest der crash-sicheren DB-Wiederherstellung (nur im Selbsttest-Build). Legt zwei
 * Test-Konten in eigene Test-Dateien (hdd1), korrumpiert das Primaer und prueft, dass
 * db_load_recover aus dem Backup wiederherstellt (Marker-Konto ueberlebt). Laesst s_users
 * zurueckgesetzt, damit das anschliessende echte user_init sauber neu laedt.
 * Rueckgabe: 1 = Recovery ok, 0 = fehlgeschlagen. */
#define TEST_DB  "hdd1:USRDBTST.DB"
#define TEST_BAK "hdd1:USRDBTST.BAK"
int user_selftest_db_recovery(void)
{
    /* 1) Valide 2-Konten-DB in die Test-Dateien schreiben (Backup + Primaer). */
    s_nusers = 0;
    s_next_uid = 0;
    if (add_internal("dbadmin", "dbadminpw", USER_CAP_ADMIN, 0) != 0 ||
        add_internal("recmark", "recmarkpw", 0, 0) != 0 ||
        save_db_to(TEST_DB, TEST_BAK) != 0) {
        s_nusers = 0; s_next_uid = 0;
        return 0;
    }
    /* 2) Das Primaer korrumpieren (Muell ohne gueltigen Header). Das Backup bleibt intakt. */
    static const char junk[] = "XX-korrupt-ohne-header-XXXXXXXXXXXXXXXXXX\n";
    if (vfs_write_file_priv(TEST_DB, junk, sizeof(junk) - 1) < 0) {
        s_nusers = 0; s_next_uid = 0;
        return 0;
    }
    /* 3) Mit Fallback laden: muss aus dem Backup wiederherstellen (r == 2) und BEIDE Konten
     *    tragen (Marker 'recmark' ueberlebt -> aus Backup, nicht frisch als admin neu). */
    int r  = db_load_recover(TEST_DB, TEST_BAK);
    int ok = (r == 2) && (find_user("recmark") != 0) && (find_user("dbadmin") != 0);
    /* 4) Aufraeumen: Test-Dateien loeschen, In-Memory-Zustand zuruecksetzen. */
    vfs_delete(TEST_DB);
    vfs_delete(TEST_BAK);
    s_nusers = 0;
    s_next_uid = 0;
    return ok;
}
#endif /* RTOS_SELFTEST */

/* Authentifiziert und liefert den Record (oder 0). Laufzeit unabhaengig von der
 * Konto-Existenz (Dummy-PBKDF2) gegen Timing-Enumeration. */
/* Authentifiziert MIT lock-freiem PBKDF2: salt/hash (+ uid/caps) unter dem Lock schnappen,
 * dann das teure PBKDF2 OHNE Lock rechnen (IRQs an -> Kern bleibt praeemptierbar, g_ticks
 * laeuft), dann Konstantzeit-Vergleich. 0 + uid/caps bei Erfolg, sonst -1. Laufzeit
 * unabhaengig von der Konto-Existenz (Dummy-PBKDF2) gegen Timing-Enumeration. */
static int auth_check(const char *name, const char *password,
                      uint32_t *uid_out, uint32_t *caps_out)
{
    uint8_t  salt[SALT_LEN], hash[HASH_LEN];
    uint32_t uid = 0, caps = 0;
    int      found;

    uint64_t f = user_lock();
    urec_t *u = find_user(name);
    found = (u != 0);
    if (found) {
        memcpy(salt, u->salt, SALT_LEN);
        memcpy(hash, u->hash, HASH_LEN);
        uid  = u->uid;
        caps = u->caps;
    } else {
        memset(salt, 0, SALT_LEN);          /* Dummy-Salt */
    }
    user_unlock(f);

    uint8_t calc[HASH_LEN];
    pbkdf2_sha256((const uint8_t *)password, slen(password),
                  salt, SALT_LEN, PBKDF2_ITERS, calc, HASH_LEN);   /* lock-frei */
    if (!found || !ct_eq(calc, hash, HASH_LEN)) {
        return -1;
    }
    if (uid_out)  { *uid_out  = uid; }
    if (caps_out) { *caps_out = caps; }
    return 0;
}

int user_authenticate(const char *name, const char *password)
{
    uint32_t uid = 0, caps = 0;
    return (auth_check(name, password, &uid, &caps) == 0) ? (int)uid : -1;
}

int user_login(const char *name, const char *password,
               uint32_t *uid_out, uint32_t *caps_out)
{
    return auth_check(name, password, uid_out, caps_out);
}

int user_add(const char *name, const char *password, uint32_t caps, uint32_t actor_caps)
{
    if (!(actor_caps & USER_CAP_ADMIN)) {
        return -1;                         /* nur Admin (vor dem Lock: kein Contending) */
    }
    if (!name_ok(name)) {
        return -1;
    }
    /* Phase 1 (unter Lock, kurz): validieren + Salt aus dem CSPRNG ziehen. */
    uint8_t salt[SALT_LEN];
    uint64_t f = user_lock();
    int err = (s_nusers >= MAX_USERS || find_user(name) != 0);
    if (!err) {
        rng_fill(salt, SALT_LEN);
    }
    user_unlock(f);
    if (err) {
        return -1;
    }
    /* Phase 2 (OHNE Lock): teures PBKDF2 -> IRQs an, Kern praeemptierbar, g_ticks laeuft. */
    uint8_t hash[HASH_LEN];
    pbkdf2_sha256((const uint8_t *)password, slen(password),
                  salt, SALT_LEN, PBKDF2_ITERS, hash, HASH_LEN);
    /* Phase 3 (unter Lock): festschreiben + persistieren, mit Re-Check gegen Races. */
    uint64_t f2 = user_lock();
    int ret = -1;
    if (s_nusers < MAX_USERS && find_user(name) == 0) {
        urec_t *u = &s_users[s_nusers];
        memset(u, 0, sizeof(*u));
        memcpy(u->name, name, slen(name));
        u->uid  = s_next_uid++;
        u->caps = caps;
        u->must_change = 1;            /* admin setzt ein Erstpasswort -> Nutzer muss es
                                        * beim ersten Login selbst aendern */
        memcpy(u->salt, salt, SALT_LEN);
        memcpy(u->hash, hash, HASH_LEN);
        s_nusers++;
        ret = (save_db() != 0) ? (s_nusers--, -1) : 0;
    }
    user_unlock(f2);
    return ret;
}

int user_delete(const char *name, uint32_t actor_caps)
{
    if (!(actor_caps & USER_CAP_ADMIN)) {
        return -1;
    }
    uint64_t f = user_lock();
    int ret = 0;
    urec_t *u = find_user(name);
    if (!u || u->uid == 0) {
        ret = -1;                          /* Stamm-Admin (uid 0) nicht loeschbar */
    } else {
        int idx = (int)(u - s_users);
        urec_t saved = s_users[idx];       /* Snapshot fuer Rollback */
        for (int i = idx; i < s_nusers - 1; i++) {
            s_users[i] = s_users[i + 1];
        }
        s_nusers--;
        if (save_db() != 0) {
            for (int i = s_nusers; i > idx; i--) {
                s_users[i] = s_users[i - 1];
            }
            s_users[idx] = saved;
            s_nusers++;
            ret = -1;
        }
    }
    user_unlock(f);
    return ret;
}

void user_list(void)
{
    uint64_t f = user_lock();
    for (int i = 0; i < s_nusers; i++) {
        uart_puts("      uid=");
        uart_putdec(s_users[i].uid);
        uart_puts(s_users[i].caps & USER_CAP_ADMIN ? " [admin] " : "         ");
        uart_puts(s_users[i].name);
        uart_puts("\n");
    }
    user_unlock(f);
}

int user_count(void)
{
    uint64_t f = user_lock();
    int n = s_nusers;
    user_unlock(f);
    return n;
}

/* --- Interaktiver Login ueber die serielle Konsole --- */
static void wipe(void *p, int n)                /* nicht wegoptimierbares Nullen */
{
    volatile char *v = (volatile char *)p;
    while (n--) {
        *v++ = 0;
    }
}

static void read_line(char *buf, int max, int echo)
{
    int  i = 0;
    char c;
    /* Fuehrende Zeilenenden ueberspringen -> absorbiert ein nach CR uebrig
     * gebliebenes LF; funktioniert so fuer CR-, LF- und CRLF-Terminals. */
    do {
        c = console_getc();                     /* Serial ODER USB-Tastatur */
    } while (c == '\r' || c == '\n');

    for (;;) {
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            break;
        }
        if (c == 8 || c == 127) {               /* Backspace / DEL */
            if (i > 0) {
                i--;
                uart_puts("\b \b");
            }
        } else if (i < max - 1 && c >= 32 && c < 127) {
            buf[i++] = c;
            uart_putc(echo ? c : '*');          /* Passwort maskieren */
        }
        c = console_getc();
    }
    buf[i] = '\0';
}

int login_console(uint32_t *uid_out, uint32_t *caps_out)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        char name[USER_NAME_MAX + 1];
        char pw[64];
        uart_puts("login: ");
        read_line(name, sizeof(name), 1);
        uart_puts("password: ");
        read_line(pw, sizeof(pw), 0);

        uint32_t uid = 0, caps = 0;
        int ok = (user_login(name, pw, &uid, &caps) == 0);
        wipe(pw, sizeof(pw));                    /* Klartext-Passwort sofort loeschen */
        if (ok) {
            uart_puts("Login erfolgreich: ");
            uart_puts(name);
            uart_puts(" (uid=");
            uart_putdec(uid);
            uart_puts(caps & USER_CAP_ADMIN ? ", admin)\n" : ")\n");

            /* Erzwungener Passwortwechsel bei Default-/Erstpasswort: ohne neues Passwort
             * gibt es keine Shell. user_change_password loescht das must_change-Flag und
             * persistiert -> beim naechsten Login nicht mehr verlangt. */
            if (user_must_change(uid)) {
                uart_puts("Passwort-Wechsel erforderlich (Standard-/Erstpasswort).\n");
                int changed = 0;
                for (int t = 0; t < 3 && !changed; t++) {
                    char np[64];
                    uart_puts("Neues Passwort: ");
                    read_line(np, sizeof(np), 0);            /* maskiert */
                    if (weak_password(np)) {
                        uart_puts("Zu schwach (min. 4 Zeichen, nicht 'admin').\n");
                        wipe(np, sizeof(np));
                        continue;
                    }
                    int cr = user_change_password(uid, np);
                    wipe(np, sizeof(np));
                    if (cr == 0) { uart_puts("Passwort gesetzt.\n"); changed = 1; }
                    else         { uart_puts("Aenderung fehlgeschlagen.\n"); }
                }
                if (!changed) {
                    uart_puts("Passwort-Wechsel erforderlich -- Login abgebrochen.\n");
                    continue;                                /* zurueck zum login:-Prompt */
                }
            }

            if (uid_out) {
                *uid_out = uid;
            }
            if (caps_out) {
                *caps_out = caps;
            }
            return 0;
        }
        uart_puts("Login fehlgeschlagen.\n");
    }
    return -1;
}
