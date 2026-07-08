/*
 * include/http.h  --  Minimaler HTTP/1.0-GET-Client (ueber den TCP-Client)
 *
 * Komponiert den vollen TCP-Client: aktiver Open -> GET-Request senden -> Antwort
 * empfangen -> aktiver Close. Bewusst minimal: nur GET, HTTP/1.0 + "Connection:
 * close", genau EINE Anfrage zur Zeit, Antwort wird bis "\r\n\r\n" (Header-Ende)
 * gesammelt -> Statuszeile + bisher empfangener Body an den Callback (kein Chunked-
 * Decoding, keine Redirects, keine Content-Length-Vervollstaendigung ueber Segmente).
 */
#ifndef RPI_RTOS_HTTP_H
#define RPI_RTOS_HTTP_H

#include <stdint.h>
#include "net.h"

/* Ergebnis: ok=1 -> status = HTTP-Statuscode (z.B. 200), body/body_len = ab Header-
 * Ende empfangene Nutzlast. ok=0 -> Fehlschlag (Connect/Parsing), status undefiniert. */
typedef void (*http_done_fn)(int status, const uint8_t *body, uint16_t body_len, int ok);

/* GET path von ip:port (Host-Header = host). 0 = Anfrage gestartet, -1 = belegt/Fehler.
 * cb wird genau einmal (asynchron) aufgerufen. */
int http_get(netif_t *nif, ip4_addr_t ip, uint16_t port,
             const char *host, const char *path, http_done_fn cb);

/* Antwort-Deadline treiben: aus dem Poll-Loop aufrufen. Loest die Single-flight-Sperre,
 * falls eine Anfrage nach erfolgreichem Connect haengt (Server stumm / PCB still verworfen);
 * meldet dann Fehlschlag ueber den Callback. */
void http_tick(void);

#endif /* RPI_RTOS_HTTP_H */
