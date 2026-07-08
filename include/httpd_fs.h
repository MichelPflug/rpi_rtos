/*
 * include/httpd_fs.h  --  VFS-gebundener Inhalts-Resolver fuer den HTTP-Server
 *
 * Bildet einen HTTP-Pfad auf eine Datei unter EINEM Doc-Root-Mount (z.B. "hdd1") ab
 * und liefert sie ueber httpd_resolve_fn an den Server. Der Mount-Praefix ist fest:
 * die VFS-Pfade lauten immer "<mount>:<rel>", sodass eine Request den Mount NICHT
 * wechseln kann -> hdd0 (System / Benutzer-DB) ist ueber HTTP grundsaetzlich
 * unerreichbar. Zusaetzlich weist der Resolver Directory-Traversal ab ("..", ":",
 * "\\", Steuerzeichen). Keine Prozent-Dekodierung (ein literales "%2e%2e" bleibt
 * literal und matcht nichts -> kann den "..":Schutz nicht umgehen).
 *
 * Antwortgroesse: Dateien werden in einen festen Puffer gelesen; passt die Antwort
 * nicht in den TCP-Sendepuffer, lehnt der Server sie mit 500 ab (keine stille Kuerzung).
 */
#ifndef RPI_RTOS_HTTPD_FS_H
#define RPI_RTOS_HTTPD_FS_H

#include <stdint.h>

/* Doc-Root-Mount setzen (z.B. "hdd1"). Default ist "hdd1". */
void httpd_fs_init(const char *mount);

/* httpd_resolve_fn-kompatibel: "/datei" -> Datei, "/" -> Verzeichnis-Index.
 * 0 = gefunden (body/len/ctype gesetzt), -1 = nicht gefunden / abgewiesen (-> 404). */
int httpd_fs_resolve(const char *path, const uint8_t **body,
                     uint16_t *len, const char **ctype);

#endif /* RPI_RTOS_HTTPD_FS_H */
