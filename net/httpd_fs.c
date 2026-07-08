/*
 * net/httpd_fs.c  --  VFS-gebundener Inhalts-Resolver fuer den HTTP-Server
 *
 * Bildet den (bereits von httpd geparsten, query-freien) HTTP-Pfad auf eine Datei
 * unter einem festen Doc-Root-Mount ab. Sicherheitskern: der Mount-Praefix ist fest
 * ("<mount>:<rel>"), eine Request kann den Mount also nie wechseln -> hdd0 ist
 * unerreichbar; zusaetzlich weist path_safe Directory-Traversal/Injection ab.
 */
#include <stdint.h>
#include "httpd_fs.h"
#include "vfs.h"

#define FS_FILEMAX  2048      /* deckt die Antwort-Grenze des Servers; groesser -> 500 */
#define FS_PATHMAX  128       /* "<mount>:<rel>" */

static const char *s_mount = "hdd1";
static uint8_t      s_body[FS_FILEMAX];

void httpd_fs_init(const char *mount)
{
    if (mount) {
        s_mount = mount;
    }
}

/* Case-insensitiver ASCII-Vollvergleich (fuer Datei-Endungen). */
static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca + 32); }
        if (cb >= 'A' && cb <= 'Z') { cb = (char)(cb + 32); }
        if (ca != cb) { return 0; }
        a++; b++;
    }
    return *a == *b;
}

/* Doc-Root sicher? Verbietet Escape/Injection. rel ist nicht-leer und ohne fuehrenden '/'. */
static int path_safe(const char *rel)
{
    if (rel[0] == '/') {
        return 0;                                  /* "//..." -> leere Komponente */
    }
    for (int i = 0; rel[i]; i++) {
        unsigned char c = (unsigned char)rel[i];
        if (c < 0x20) { return 0; }                /* Steuerzeichen */
        if (c == ':' || c == '\\') { return 0; }   /* Mount-Injection / Backslash */
        /* ".." NUR als ganze Pfad-Komponente abweisen (echtes Directory-Traversal) --
         * nicht jeden Doppelpunkt im Namen, z.B. die LFN-Datei "release..notes.txt".
         * fat32 behandelt nur eine Komponente, die exakt ".." ist, als Eltern-Verweis. */
        if (c == '.' && rel[i + 1] == '.' &&
            (i == 0 || rel[i - 1] == '/') &&
            (rel[i + 2] == '/' || rel[i + 2] == '\0')) {
            return 0;
        }
    }
    return 1;
}

/* Content-Type aus der Datei-Endung (letztes '.'). */
static const char *ctype_for(const char *rel)
{
    int dot = -1;
    for (int i = 0; rel[i]; i++) {
        if (rel[i] == '.') { dot = i; }
    }
    if (dot < 0) {
        return "application/octet-stream";
    }
    const char *e = rel + dot + 1;
    if (ieq(e, "htm") || ieq(e, "html")) { return "text/html"; }
    if (ieq(e, "txt"))                   { return "text/plain"; }
    if (ieq(e, "css"))                   { return "text/css"; }
    if (ieq(e, "js"))                    { return "application/javascript"; }
    if (ieq(e, "json"))                  { return "application/json"; }
    return "application/octet-stream";
}

int httpd_fs_resolve(const char *path, const uint8_t **body,
                     uint16_t *len, const char **ctype)
{
    if (!path || path[0] != '/') {
        return -1;
    }
    const char *rel = path + 1;                    /* fuehrenden '/' entfernen */

    /* "/" -> Verzeichnis-Index des Doc-Root. */
    if (rel[0] == '\0') {
        int n = vfs_listdir(s_mount, (char *)s_body, FS_FILEMAX);
        if (n < 0) {
            return -1;
        }
        *body = s_body;
        *len  = (uint16_t)n;
        *ctype = "text/plain";
        return 0;
    }

    if (!path_safe(rel)) {
        return -1;                                 /* Escape-/Injection-Versuch -> 404 */
    }

    /* VFS-Pfad "<mount>:<rel>" bounded bauen. Der erste ':' ist IMMER der hier
     * eingefuegte -> der Mount bleibt fix, unabhaengig vom rel-Inhalt. */
    char vp[FS_PATHMAX];
    int o = 0;
    for (int i = 0; s_mount[i]; i++) {
        if (o >= FS_PATHMAX - 1) { return -1; }
        vp[o++] = s_mount[i];
    }
    if (o >= FS_PATHMAX - 1) { return -1; }
    vp[o++] = ':';
    for (int i = 0; rel[i]; i++) {
        if (o >= FS_PATHMAX - 1) { return -1; }
        vp[o++] = rel[i];
    }
    vp[o] = '\0';

    int n = vfs_read_file(vp, s_body, FS_FILEMAX);
    if (n < 0) {
        return -1;                                 /* nicht gefunden -> 404 */
    }
    if (n > FS_FILEMAX) {
        return -1;   /* n = wahre Groesse > Body-Puffer -> nicht ausliefern (keine Content-Length-Luege) */
    }
    *body = s_body;
    *len  = (uint16_t)n;
    *ctype = ctype_for(rel);
    return 0;
}
