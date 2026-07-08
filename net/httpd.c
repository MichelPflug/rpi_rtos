/*
 * net/httpd.c  --  Minimaler HTTP/1.0-Server (ueber tcp_listen)
 *
 * tcp_listen(port) liefert eingehende In-Order-Daten an on_data(). Der Handler ist
 * ZUSTANDSLOS: er parst die Request-Zeile ("GET <pfad> HTTP/x") aus dem ersten Segment,
 * loest den Inhalt ueber den Resolver auf und sendet die komplette Antwort (Statuszeile
 * + Header + Body), dann aktiver Close. Kein pro-Verbindung-Puffer -> keine Identitaets-
 * /Reuse-Probleme: jede Verbindung wird einmalig anhand ihres ersten Segments bedient.
 */
#include <stdint.h>
#include "net.h"
#include "tcp.h"
#include "httpd.h"

#define HTTPD_PATH_MAX  256
#define HTTPD_HDR_MAX   320      /* nur Statuszeile + Header; Body wird separat gesendet */

static httpd_resolve_fn s_resolve;

/* uint32 -> Dezimal nach buf[0..max). Liefert die Ziffernzahl (>=1) oder 0 (zu klein). */
static int u32_dec(char *buf, int max, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && n < 10);
    if (n > max) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}

/* Bounded String-Append: haengt s an dst[off..] an. Neuer Offset oder -1 (zu lang). */
static int sapp(char *dst, int off, int max, const char *s)
{
    if (off < 0) {
        return -1;
    }
    int i = 0;
    while (s[i]) {
        if (off + i >= max) {
            return -1;
        }
        dst[off + i] = s[i];
        i++;
    }
    return off + i;
}

/* "HTTP/1.0 <status>\r\nContent-Type: <ctype>\r\nContent-Length: <len>\r\nConnection:
 * close\r\n\r\n" + Body senden, dann aktiver Close.
 *
 * Single-write-then-close: die GANZE Antwort (Header + Body) muss in EINEN Schwung in
 * den freien Sendepuffer passen -- es gibt keinen Mechanismus, weitere Bytes nachzulegen,
 * waehrend der sndbuf abfliesst (direkt danach folgt FIN). Passt der Body nicht, waere
 * eine Teilauslieferung mit voller Content-Length eine Wire-Luege (Client haengt) -> wir
 * antworten stattdessen mit 500 (dessen winziger Body immer passt). */
static void send_response(tcp_pcb_t *pcb, const char *status, const char *ctype,
                          const uint8_t *body, uint16_t len)
{
    static const char too_big[] = "500 Internal Server Error\n";
    int avail = tcp_sndbuf_free(pcb);
    char hdr[HTTPD_HDR_MAX];

    for (int attempt = 0; attempt < 2; attempt++) {
        int o = 0;
        o = sapp(hdr, o, HTTPD_HDR_MAX, "HTTP/1.0 ");
        o = sapp(hdr, o, HTTPD_HDR_MAX, status);              /* z.B. "200 OK" */
        o = sapp(hdr, o, HTTPD_HDR_MAX, "\r\nContent-Type: ");
        o = sapp(hdr, o, HTTPD_HDR_MAX, ctype ? ctype : "text/plain");
        o = sapp(hdr, o, HTTPD_HDR_MAX, "\r\nContent-Length: ");
        if (o >= 0) {
            int d = u32_dec(hdr + o, HTTPD_HDR_MAX - o, (uint32_t)len);
            o = (d > 0) ? o + d : -1;
        }
        o = sapp(hdr, o, HTTPD_HDR_MAX, "\r\nConnection: close\r\n\r\n");
        if (o < 0) {
            break;                       /* Header sprengten den Puffer (sollte nie) */
        }
        if (o + (int)len <= avail) {     /* komplette Antwort passt -> senden */
            tcp_write(pcb, (const uint8_t *)hdr, (uint16_t)o);
            if (body && len) {
                tcp_write(pcb, body, len);
            }
            tcp_close(pcb);              /* HTTP/1.0: nach der Antwort schliessen */
            return;
        }
        /* Antwort zu gross fuer diesen minimalen Server -> EINMAL auf 500 umstellen. */
        status = "500 Internal Server Error";
        ctype  = "text/plain";
        body   = (const uint8_t *)too_big;
        len    = (uint16_t)(sizeof(too_big) - 1);
    }
    tcp_close(pcb);                      /* selbst 500 passt nicht / Header-Overflow */
}

/* recv-Handler: parst die Request-Zeile aus dem ersten Segment und antwortet. */
static void on_data(tcp_pcb_t *pcb, const uint8_t *data, uint16_t len)
{
    /* Nur GET. Methode = die ersten 4 Bytes "GET ". Alles andere -> 405. (Trailing-
     * Daten einer bereits beantworteten/geschlossenen Verbindung landen hier ebenfalls;
     * tcp_write/tcp_close sind dann no-ops, da der PCB nicht mehr ESTABLISHED ist.) */
    if (len < 5 || data[0] != 'G' || data[1] != 'E' || data[2] != 'T' || data[3] != ' ') {
        send_response(pcb, "405 Method Not Allowed", "text/plain",
                      (const uint8_t *)"405 Method Not Allowed\n", 23);
        return;
    }
    /* Pfad: ab Offset 4 bis zum naechsten Leerzeichen/Zeilenende. Ein '?' beendet den
     * Pfad ebenfalls -> der Query-String wird abgeschnitten (verworfen; dieser Server
     * wertet keine Query-Parameter aus), sonst verfehlte '/x?a=b' den Resolver-Match. */
    char path[HTTPD_PATH_MAX];
    int pi = 0;
    int i = 4;
    while (i < (int)len && data[i] != ' ' && data[i] != '\r' &&
           data[i] != '\n' && data[i] != '?') {
        if (pi >= HTTPD_PATH_MAX - 1) {
            send_response(pcb, "414 URI Too Long", "text/plain",
                          (const uint8_t *)"414 URI Too Long\n", 17);
            return;
        }
        path[pi++] = (char)data[i++];
    }
    path[pi] = '\0';
    if (pi == 0) {
        send_response(pcb, "400 Bad Request", "text/plain",
                      (const uint8_t *)"400 Bad Request\n", 16);
        return;
    }

    const uint8_t *body = 0;
    uint16_t       blen = 0;
    const char    *ctype = 0;
    if (s_resolve && s_resolve(path, &body, &blen, &ctype) == 0) {
        send_response(pcb, "200 OK", ctype, body, blen);
    } else {
        static const char nf[] = "404 Not Found\n";
        send_response(pcb, "404 Not Found", "text/plain",
                      (const uint8_t *)nf, (uint16_t)(sizeof(nf) - 1));
    }
}

int httpd_listen(uint16_t port, httpd_resolve_fn resolve)
{
    net_enter();
    s_resolve = resolve;
    int r = tcp_listen(port, on_data);   /* reentrant unter dem Big-Net-Lock */
    net_leave();
    return r;
}
