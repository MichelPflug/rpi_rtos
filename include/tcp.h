/*
 * include/tcp.h  --  Minimaler TCP (passiver Server + aktiver Client)
 *
 * Unterstuetzt: passiven Open (LISTEN) UND aktiven Open (tcp_connect), Drei-Wege-
 * Handshake, In-Order-Daten mit kumulativem ACK, Retransmit unbestaetigter Daten,
 * aktiven + passiven Close. **Sendeseitige Flusskontrolle**: das vom Peer angekuendigte
 * Empfangsfenster wird respektiert -- tcp_write puffert, gesendet wird nur, soweit das
 * Fenster es zulaesst (Sliding-Window: ACKs oeffnen es weiter); ein geschlossenes Fenster
 * wird per 1-Byte-Persist-Probe sondiert. **Empfangsseitig**: Reassembly-Puffer -- out-of-
 * order eintreffende Segmente werden gepuffert und erst IN REIHENFOLGE an die App zugestellt;
 * das angekuendigte Fenster spiegelt den freien Reassembly-Puffer (mit Echo-Backpressure).
 * **SACK** (RFC 2018) wird ausgehandelt (SACK-Permitted in beiden SYN) und in BEIDE Richtungen
 * genutzt: empfangsseitig ANGEKUENDIGT (ACKs tragen SACK-Bloecke der gepufferten Out-of-Order-
 * Strecken) und sendeseitig AUSGEWERTET (ein Scoreboard markiert vom Peer SACKte Bytes -> der
 * Retransmit ueberspringt sie und sendet nur die Luecken). KEINE Fenster-Skalierung. Polling: tcp_tick().
 */
#ifndef RPI_RTOS_TCP_H
#define RPI_RTOS_TCP_H

#include <stdint.h>
#include "net.h"

typedef struct tcp_pcb tcp_pcb_t;

/* Wird bei empfangenen In-Order-Daten aufgerufen; im Handler darf tcp_write()
 * (z.B. zum Echo) verwendet werden. */
typedef void (*tcp_recv_fn)(tcp_pcb_t *pcb, const uint8_t *data, uint16_t len);

/* Ergebnis eines aktiven Open (tcp_connect): ok=1 verbunden (in pcb darf jetzt
 * tcp_write), ok=0 fehlgeschlagen (RST/Timeout; pcb danach NICHT mehr benutzen). */
typedef void (*tcp_connected_fn)(tcp_pcb_t *pcb, int ok);

int  tcp_listen(uint16_t port, tcp_recv_fn handler);   /* 0 = ok */
int  tcp_unlisten(uint16_t port);                      /* Listener-Slot freigeben; 0 = ok, -1 = nicht da */

/* Aktiver Open: baut eine Verbindung zu rip:rport auf (ephemerer lokaler Port).
 * on_conn meldet Erfolg/Fehlschlag, on_recv liefert empfangene Daten. Liefert den
 * PCB (im SYN_SENT) oder 0 (Pool voll). */
tcp_pcb_t *tcp_connect(netif_t *nif, ip4_addr_t rip, uint16_t rport,
                       tcp_connected_fn on_conn, tcp_recv_fn on_recv);

int  tcp_write(tcp_pcb_t *pcb, const uint8_t *data, uint16_t len);

/* Freier Platz im Sendepuffer (Bytes, die tcp_write JETZT annehmen wuerde): in
 * ESTABLISHED/CLOSE_WAIT = TCP_SNDBUF - ausstehende Bytes, sonst 0. Erlaubt einem
 * Sender, eine Nachricht VOR dem Schreiben gegen die Puffergrenze zu pruefen, statt
 * sich auf das stille Kuerzen (Rest verwerfen) von tcp_write zu verlassen. */
int  tcp_sndbuf_free(const tcp_pcb_t *pcb);

/* Zuletzt vom Peer angekuendigtes Empfangsfenster (Bytes) -- 0 vor dem Handshake.
 * Nur informativ (z.B. Logging/Diagnose der Sende-Flusskontrolle). */
uint32_t tcp_snd_wnd(const tcp_pcb_t *pcb);

/* Test-Zugriff (white-box) auf Congestion Control (RFC 5681): cwnd / ssthresh in Bytes.
 * Der Loopback-Test prueft damit die cwnd-Uebergaenge (Slow Start / RTO-Kollaps / Fast Recovery).
 * NUR im Selbsttest-Build vorhanden -- kein Debug-Export im RC-Produktions-Image. */
#ifdef RTOS_SELFTEST
uint32_t tcp_dbg_cwnd(const tcp_pcb_t *pcb);
uint32_t tcp_dbg_ssthresh(const tcp_pcb_t *pcb);
#endif

/* Aktiver Close: sendet FIN (ESTABLISHED -> FIN_WAIT_1 -> ... -> TIME_WAIT). Der
 * Empfangsweg bleibt offen (Half-Close), bis der Peer ebenfalls FIN schickt. */
void tcp_close(tcp_pcb_t *pcb);

void tcp_tick(netif_t *nif);                           /* Retransmit/Close-Timer */

/* White-Box-Selbsttest des Empfangs-Reassembly (out-of-order Zustellung in Reihenfolge):
 * 1 = bestanden. In QEMU nicht per Host-Interop ausloesbar (SLIRP ordnet nicht um), daher
 * treibt der Test den Reassembly-Pfad direkt. */
int  tcp_reasm_selftest(void);

/* End-to-End-Conformance-Test ueber eine in-guest Loopback-"Leitung": ein virtueller Peer
 * erzeugt rohe TCP-Segmente (frei waehlbare Reihenfolge + Fenstergroesse) und treibt den
 * ECHTEN Pfad tcp_input/tcp_output/tcp_tick. Prueft, was Host-Interop NICHT kann (SLIRP
 * ordnet nicht um, kuendigt 65535 an): Out-of-Order-Empfang -> in-order Zustellung, und
 * sendeseitige Flusskontrolle gegen ein kleines Peer-Fenster. 1 = bestanden. */
int  tcp_looptest_run(void);

#endif /* RPI_RTOS_TCP_H */
