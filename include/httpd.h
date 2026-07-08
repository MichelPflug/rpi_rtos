/*
 * include/httpd.h  --  Minimaler HTTP/1.0-Server (ueber tcp_listen)
 *
 * Bewusst minimal: nur GET, HTTP/1.0 + "Connection: close", genau EINE Antwort pro
 * Verbindung (danach aktiver Close). Der recv-Handler ist ZUSTANDSLOS -- er parst die
 * Request-ZEILE aus dem ersten empfangenen Segment (die Header werden ignoriert; ein
 * GET braucht nur Methode + Pfad) und antwortet sofort. Annahme: die Request-Zeile
 * passt in EIN TCP-Segment -- in der Praxis immer der Fall (MSS 1460 >> Request-Zeile);
 * kein Reassembly ueber Segmente (wie der uebrige Stack), kein Keep-Alive, kein Chunked,
 * kein Upload/Body. Der Inhalt wird ueber einen Resolver-Callback geliefert (entkoppelt
 * von der Quelle: In-RAM-Tabelle im Test-Harness, spaeter VFS auf echter HW).
 *
 * Antwortgroesse: die GESAMTE Antwort (Statuszeile + Header + Body) muss in den freien
 * TCP-Sendepuffer passen (~2 KB) -- der Server schreibt einmal und schliesst sofort.
 * Eine Ressource, deren Antwort nicht passt, wird mit 500 abgewiesen (KEINE stille
 * Kuerzung mit falscher Content-Length). Ein '?' im Request-Target beendet den Pfad
 * (Query-String wird verworfen, nicht ausgewertet).
 */
#ifndef RPI_RTOS_HTTPD_H
#define RPI_RTOS_HTTPD_H

#include <stdint.h>
#include "net.h"

/* Inhalts-Resolver: liefert fuer den angefragten Pfad den Body. Rueckgabe 0 = gefunden
 * (body+len gesetzt, ctype optional -> NULL ergibt "text/plain"), -1 = nicht gefunden
 * (404). Der Body muss bis zum Senden gueltig bleiben (statischer/Konstant-Speicher). */
typedef int (*httpd_resolve_fn)(const char *path, const uint8_t **body,
                                uint16_t *len, const char **ctype);

/* HTTP-Server auf port starten; resolve liefert den Inhalt. 0 = ok, -1 = Fehler.
 * Bewusst ein einziger Server (eine globale Resolver-Funktion). */
int httpd_listen(uint16_t port, httpd_resolve_fn resolve);

#endif /* RPI_RTOS_HTTPD_H */
