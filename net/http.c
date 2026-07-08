/*
 * net/http.c  --  Minimaler HTTP/1.0-GET-Client (ueber den TCP-Client)
 *
 * http_get() oeffnet aktiv eine TCP-Verbindung, sendet einen GET-Request, sammelt
 * die Antwort (bounds-sicher) bis zum Header-Ende "\r\n\r\n", parst die Statuszeile
 * und ruft den Callback; danach aktiver Close. Genau eine Anfrage zur Zeit.
 */
#include <stdint.h>
#include "net.h"
#include "tcp.h"
#include "http.h"

#define HTTP_REQ_MAX     320
#define HTTP_BUF_MAX     2048
#define HTTP_TIMEOUT_MS  10000   /* Antwort-Deadline (deckt Connect + Response ab) */

static struct {
    int          active;
    int          done;
    http_done_fn cb;
    tcp_pcb_t   *pcb;            /* in-flight Verbindung (Identitaet fuer on_recv/on_conn) */
    uint64_t     start_ms;      /* Start der Anfrage (Antwort-Deadline) */
    char         req[HTTP_REQ_MAX];
    int          reqlen;
    uint8_t      buf[HTTP_BUF_MAX];
    int          len;
} s_h;

/* Bounded String-Append: haengt s an dst[off..] an. Neuer Offset oder -1 (zu lang). */
static int sappend(char *dst, int off, int max, const char *s)
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

/* Index NACH dem ersten "\r\n\r\n" in buf[0..len), oder -1. */
static int find_header_end(const uint8_t *b, int len)
{
    for (int i = 0; i + 3 < len; i++) {
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}

/* Statuscode aus "HTTP/1.x SSS ..." (erstes Leerzeichen, dann 3 Ziffern). -1 bei Fehler. */
static int parse_status(const uint8_t *b, int len)
{
    int i = 0;
    while (i < len && b[i] != ' ' && b[i] != '\r' && b[i] != '\n') {
        i++;
    }
    if (i >= len || b[i] != ' ') {
        return -1;
    }
    i++;
    if (i + 3 > len) {
        return -1;
    }
    int code = 0;
    for (int k = 0; k < 3; k++) {
        if (b[i + k] < '0' || b[i + k] > '9') {
            return -1;
        }
        code = code * 10 + (b[i + k] - '0');
    }
    return code;
}

static void fail(void)
{
    if (!s_h.active) {
        return;
    }
    s_h.active = 0;
    if (s_h.cb) {
        s_h.cb(0, 0, 0, 0);
    }
}

static void on_recv(tcp_pcb_t *pcb, const uint8_t *data, uint16_t dlen)
{
    /* Nur Daten der AKTUELLEN Verbindung annehmen. Ein verspaetetes Body-Segment der
     * bereits (halb-)geschlossenen Vorgaenger-Verbindung wird im FIN_WAIT-Zweig von
     * tcp_input weiterhin an diesen Handler geliefert -- es darf NICHT in den Puffer
     * einer inzwischen vom Callback neu gestarteten Anfrage einsickern. Der reine
     * Pointer-Vergleich ist sicher (kein Dereferenzieren des fremden PCB). */
    if (!s_h.active || s_h.done || pcb != s_h.pcb) {
        return;
    }
    int space = HTTP_BUF_MAX - s_h.len;
    int n = ((int)dlen < space) ? (int)dlen : space;
    for (int i = 0; i < n; i++) {
        s_h.buf[s_h.len + i] = data[i];
    }
    s_h.len += n;

    int hdr_end = find_header_end(s_h.buf, s_h.len);
    if (hdr_end < 0) {
        if (s_h.len >= HTTP_BUF_MAX) {     /* Puffer voll ohne Header-Ende -> aufgeben */
            s_h.done = 1;
            tcp_close(pcb);
            s_h.active = 0;
            if (s_h.cb) { s_h.cb(0, 0, 0, 0); }
        }
        return;                            /* sonst auf mehr Daten warten */
    }
    int status = parse_status(s_h.buf, s_h.len);
    s_h.done = 1;
    tcp_close(pcb);                         /* Verbindung aktiv schliessen */
    s_h.active = 0;                         /* erlaubt dem cb, erneut http_get zu rufen */
    if (s_h.cb) {
        s_h.cb(status, s_h.buf + hdr_end, (uint16_t)(s_h.len - hdr_end), status > 0);
    }
}

static void on_conn(tcp_pcb_t *pcb, int ok)
{
    if (pcb != s_h.pcb) {        /* Connect-Ergebnis einer veralteten Verbindung */
        return;
    }
    if (!ok) {
        fail();
        return;
    }
    tcp_write(pcb, (const uint8_t *)s_h.req, (uint16_t)s_h.reqlen);
}

/* Antwort-Deadline. Feuert der Server nach erfolgreichem Connect keine (vollstaendige)
 * Antwort -- oder hat TCP den PCB still verworfen (in-window RST / Retransmit-Erschoepfung
 * / Idle-Reap) ohne den Handler zu rufen -- bliebe die Anfrage sonst ewig "active" und
 * jeder weitere http_get() liefe ins -1. http_tick() (aus dem Poll-Loop) loest die Sperre
 * nach HTTP_TIMEOUT_MS und meldet Fehlschlag. Es ruft bewusst KEIN tcp_close auf s_h.pcb:
 * der Slot koennte zwischenzeitlich von TCP recycelt worden sein; eine noch lebende, aber
 * haengende Verbindung erntet tcp_tick() selbst per Idle-Reap. */
/* Big-Net-Lock-Wrapper (T1.11) -- beide Funktionen haben fruehe returns. */
static void http_tick_inner(void);
static int  http_get_inner(netif_t *nif, ip4_addr_t ip, uint16_t port,
                           const char *host, const char *path, http_done_fn cb);

void http_tick(void)
{
    net_enter();
    http_tick_inner();
    net_leave();
}

static void http_tick_inner(void)
{
    if (!s_h.active || s_h.done) {
        return;
    }
    if (net_now_ms() - s_h.start_ms > HTTP_TIMEOUT_MS) {
        s_h.done   = 1;
        s_h.active = 0;
        if (s_h.cb) {
            s_h.cb(0, 0, 0, 0);
        }
    }
}

int http_get(netif_t *nif, ip4_addr_t ip, uint16_t port,
             const char *host, const char *path, http_done_fn cb)
{
    net_enter();
    int r = http_get_inner(nif, ip, port, host, path, cb);
    net_leave();
    return r;
}

static int http_get_inner(netif_t *nif, ip4_addr_t ip, uint16_t port,
             const char *host, const char *path, http_done_fn cb)
{
    if (s_h.active) {
        return -1;                          /* nur eine Anfrage zur Zeit */
    }
    /* "GET <path> HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n" */
    int o = 0;
    o = sappend(s_h.req, o, HTTP_REQ_MAX, "GET ");
    o = sappend(s_h.req, o, HTTP_REQ_MAX, path);
    o = sappend(s_h.req, o, HTTP_REQ_MAX, " HTTP/1.0\r\nHost: ");
    o = sappend(s_h.req, o, HTTP_REQ_MAX, host);
    o = sappend(s_h.req, o, HTTP_REQ_MAX, "\r\nConnection: close\r\n\r\n");
    if (o < 0) {
        return -1;                          /* Request zu lang */
    }
    s_h.reqlen   = o;
    s_h.active   = 1;
    s_h.done     = 0;
    s_h.len      = 0;
    s_h.cb       = cb;
    s_h.start_ms = net_now_ms();
    /* tcp_connect feuert die Callbacks NICHT synchron (sendet nur SYN, returnt den PCB),
     * daher ist s_h.pcb gesetzt, bevor on_conn/on_recv jemals laufen koennen. */
    s_h.pcb = tcp_connect(nif, ip, port, on_conn, on_recv);
    if (!s_h.pcb) {
        s_h.active = 0;                     /* PCB-Pool voll */
        return -1;
    }
    return 0;
}
