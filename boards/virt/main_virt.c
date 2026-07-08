/*
 * boards/virt/main_virt.c  --  Netz-Test-Harness fuer QEMU `virt`
 *
 * Verifiziert den portablen TCP/IP-Stack mit ECHTER Interop ueber virtio-net:
 *   - bezieht die IP per DHCP vom SLIRP-Server (dhcp_start/dhcp_tick),
 *   - pingt das Gateway (ARP + ICMP gegen einen echten Peer),
 *   - betreibt einen UDP-Echo-Server (Port 5555) UND einen TCP-Echo-Server
 *     (Port 5556), die der Host ueber hostfwd (udp 5555 / tcp 5556) erreicht.
 *
 * Reines Polling (kein IRQ/Timer). Diese Datei stellt die Einsprungfunktion
 * kmain() (von start.S aufgerufen) und ersetzt im virt-Build kernel/kmain.c.
 */
#include <stdint.h>
#include "aarch64.h"
#include "uart.h"
#include "kmem.h"
#include "net.h"
#include "virtio_net.h"
#include "dhcp.h"
#include "tcp.h"
#include "dns.h"
#include "http.h"
#include "httpd.h"

uint64_t g_entry_el;            /* von start.S gesetzt */
uint64_t g_dtb_ptr;             /* von start.S gesetzt (im virt-Harness ungenutzt) */
extern char virt_vectors[];     /* aus trap_virt.S */
void mmu_virt_init(void);       /* aus mmu_virt.c */

/* Diagnose-Handler fuer unerwartete Exceptions (siehe trap_virt.S). */
void virt_trap(uint64_t idx)
{
    uart_puts("\n*** virt-Trap (Vektor-Index ");
    uart_putdec(idx);
    uart_puts(") ***\n  ESR_EL1 = ");
    uart_puthex(READ_SYSREG(esr_el1));
    uart_puts("\n  ELR_EL1 = ");
    uart_puthex(READ_SYSREG(elr_el1));
    uart_puts("\n  FAR_EL1 = ");
    uart_puthex(READ_SYSREG(far_el1));
    uart_puts("\n  angehalten.\n");
}

static netif_t g_nif;

/* UDP-Echo-Server: empfangene Daten unveraendert an den Absender zurueck. */
static void echo_handler(netif_t *nif, ip4_addr_t src, uint16_t sport,
                         const uint8_t *data, uint16_t len)
{
    uart_puts("[udp] 5555: ");
    uart_putdec(len);
    uart_puts(" Byte von ");
    net_print_ip(src);
    uart_putc(':');
    uart_putdec(sport);
    uart_puts(" -> echo\n");
    udp_send(nif, src, sport, 5555, data, len);
}

/* TCP-Echo-Server: empfangene Daten unveraendert zurueckschreiben. */
static void tcp_echo(tcp_pcb_t *pcb, const uint8_t *data, uint16_t len)
{
    uart_puts("[tcp] 5556: ");
    uart_putdec(len);
    uart_puts(" Byte -> echo\n");
    tcp_write(pcb, data, len);
}

static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) { return 0; }
        a++; b++;
    }
    return *a == *b;
}

/* HTTP-Server-Resolver: kleine In-RAM-Routentabelle (der virt-Harness hat kein VFS).
 * Auf echter HW koennte derselbe Server an die VFS gebunden werden. */
static int http_resolve(const char *path, const uint8_t **body,
                        uint16_t *len, const char **ctype)
{
    static const char root[] =
        "rpi_rtos httpd\nGET /status fuer den Status.\nmarker=rpi_rtos-httpd-root-7b2e\n";
    static const char status[] = "rpi_rtos-httpd-status: ok\n";

    uart_puts("[httpd] GET ");
    uart_puts(path);
    uart_puts("\n");

    if (streq(path, "/")) {
        *body = (const uint8_t *)root;
        *len  = (uint16_t)(sizeof(root) - 1);
        *ctype = "text/plain";
        return 0;
    }
    if (streq(path, "/status")) {
        *body = (const uint8_t *)status;
        *len  = (uint16_t)(sizeof(status) - 1);
        *ctype = "text/plain";
        return 0;
    }
    if (streq(path, "/big")) {
        /* Antwort > TCP-Sendepuffer -> der Server muss sauber mit 500 ablehnen,
         * statt den Body still zu kuerzen (Content-Length-Luege). */
        static const char big[2100];
        *body = (const uint8_t *)big;
        *len  = (uint16_t)sizeof(big);
        *ctype = "text/plain";
        return 0;
    }
    return -1;   /* -> 404 */
}

/* --- Aktiver TCP-Client: verbindet sich zum Host (SLIRP-Gateway) und sendet --- */
static void client_recv(tcp_pcb_t *pcb, const uint8_t *data, uint16_t len)
{
    uart_puts("[tcpc] Antwort vom Host: ");
    for (uint16_t i = 0; i < len; i++) {
        uart_putc((char)data[i]);
    }
    /* Round-Trip komplett -> Verbindung aktiv schliessen (FIN). */
    uart_puts("\n[tcpc] schliesse Verbindung (aktiver Close)\n");
    tcp_close(pcb);
}

/* HTTP-GET-Ergebnis: Statuscode + Body-Anfang ausgeben. */
static void http_done(int status, const uint8_t *body, uint16_t len, int ok)
{
    if (!ok) {
        uart_puts("[http] GET fehlgeschlagen\n");
        return;
    }
    uart_puts("[http] status=");
    uart_putdec((uint32_t)status);
    uart_puts(" body: ");
    for (uint16_t i = 0; i < len && i < 64; i++) {
        uart_putc((char)body[i]);
    }
    uart_puts("\n");
}

/* DNS-Ergebnis: aufgeloeste IP (oder Fehlschlag) ausgeben. */
static void dns_done(const char *name, ip4_addr_t ip, int ok)
{
    if (ok) {
        uart_puts("[dns] ");
        uart_puts(name);
        uart_puts(" -> ");
        net_print_ip(ip);
        uart_puts("\n");
    } else {
        uart_puts("[dns] Aufloesung fehlgeschlagen: ");
        uart_puts(name);
        uart_puts("\n");
    }
}

static void client_connected(tcp_pcb_t *pcb, int ok)
{
    if (!ok) {
        uart_puts("[tcpc] Connect fehlgeschlagen\n");
        return;
    }
    static const char msg[] = "rpi_rtos-client-probe-5a7c\n";
    uart_puts("[tcpc] verbunden (Peer-Fenster=");
    uart_putdec(tcp_snd_wnd(pcb));               /* vom SYN-ACK gelerntes Empfangsfenster */
    uart_puts(") -> sende Probe\n");
    tcp_write(pcb, (const uint8_t *)msg, (uint16_t)(sizeof(msg) - 1));
}

void kmain(void)
{
    uart_init();
    uart_puts("\n========================================\n");
    uart_puts("   rpi_rtos  -  Netz-Harness (QEMU virt)\n");
    uart_puts("========================================\n");
    uart_puts("Eintritt auf EL");
    uart_putdec(g_entry_el);
    uart_puts(", jetzt EL");
    uart_putdec(current_el());
    uart_puts("\n");

    WRITE_SYSREG(vbar_el1, (uint64_t)(uintptr_t)virt_vectors);
    isb();

    uart_puts("[1] MMU (RAM Normal-NC, Periph Device)...\n");
    mmu_virt_init();

    uart_puts("[2] virtio-net initialisieren...\n");
    memset(&g_nif, 0, sizeof(g_nif));
    if (virtio_net_init(&g_nif) != 0) {
        uart_puts("    FEHLER: kein virtio-net. Harness haelt an.\n");
        for (;;) {
            wfe();
        }
    }

    udp_bind(5555, echo_handler);
    tcp_listen(5556, tcp_echo);
    httpd_listen(80, http_resolve);
    uart_puts("[3] UDP-Echo (5555) + TCP-Echo (5556) + HTTP-Server (80) aktiv.\n");

    /* Empfangs-Reassembly (out-of-order -> in-order) white-box pruefen -- SLIRP ordnet
     * keine Segmente um, daher nicht per Host-Interop ausloesbar. */
    uart_puts("[3b] TCP-Reassembly-Selbsttest (out-of-order): ");
    uart_puts(tcp_reasm_selftest() ? "ok\n" : "FEHLER\n");

    /* E2E ueber eine in-guest Loopback-Leitung: virtueller Peer mit umgeordneten Segmenten
     * + kleinem Fenster treibt den ECHTEN tcp_input/tcp_output-Pfad (was Host-Interop nicht
     * kann). Laeuft VOR DHCP, solange noch keine echten PCBs/Verkehr existieren. */
    uart_puts("[3c] TCP-Loopback-Conformance (OOO + Fenster + FIN + Persist + RTX + SACK + RTO + Nagle + WScale + PAWS + CC + KA + SWS): ");
    uart_puts(tcp_looptest_run() ? "ok\n" : "FEHLER\n");

    uart_puts("[4] DHCP-Client starten (IP vom SLIRP-Server beziehen)...\n");
    dhcp_start(&g_nif);

    uart_puts("[5] Poll-Loop: DHCP + Dienste + Gateway-Ping.\n");

    int bound_announced = 0;
    int renew_done = 0;
    int netlock_announced = 0;
    uint16_t seq = 0;
    int sent = 0;
    const int MAXPING = 4;
    uint32_t spin = 0;

    for (;;) {
        virtio_net_poll();
        dhcp_tick(&g_nif);
        tcp_tick(&g_nif);
        dns_tick(&g_nif);
        http_tick();

        if (dhcp_bound() && !bound_announced) {
            bound_announced = 1;
            uart_puts("[net] gebunden: ");
            net_print_ip(g_nif.ip);
            uart_puts(" GW ");
            net_print_ip(g_nif.gateway);
            uart_puts("\n");
            arp_request(&g_nif, g_nif.gateway);   /* ARP fuer Gateway vorwaermen */
            /* Aktiver Open zum Host (SLIRP-Gateway 10.0.2.2:5557). Das erste SYN kann
             * mangels ARP-Cache verfallen -> tcp_tick wiederholt es nach dem RTO. */
            tcp_connect(&g_nif, g_nif.gateway, 5557, client_connected, client_recv);
            /* DNS-Aufloesung gegen den SLIRP-DNS-Server (10.0.2.3). Erste Anfrage kann
             * mangels ARP-Cache verfallen -> dns_tick wiederholt sie. */
            dns_resolve(&g_nif, IP4(10, 0, 2, 3), "example.com", dns_done);
            /* HTTP-GET gegen den Host (SLIRP-Gateway 10.0.2.2:5558) -- komponiert den
             * vollen TCP-Client (connect/send/recv/close) + HTTP-Parsing. */
            http_get(&g_nif, g_nif.gateway, 5558, "rpi-host", "/", http_done);
        }

        /* Ping-/Renew-/Guardian-Kadenz: Busy-Zaehler nur zur groben Verlangsamung. 200k
         * (statt 1M) -> die spaeten, kadenz-getriebenen Punkte (Ping, DHCP-Renew, netlocktest)
         * feuern zuverlaessig innerhalb des Harness-Fensters (der T1.11-Net-Lock erhoehte die
         * Pro-Iteration-Kosten -> 1M lief unter Last aus dem 4-s-Fenster). */
        if (dhcp_bound() && ++spin >= 200000u) {
            spin = 0;
            if (sent < MAXPING) {
                uint16_t s = (uint16_t)(seq + 1);
                if (icmp_send_echo(&g_nif, g_nif.gateway, 0x1234, s) >= 0) {
                    seq = s;
                    sent++;
                    uart_puts("[ping] echo request -> ");
                    net_print_ip(g_nif.gateway);
                    uart_puts(" seq=");
                    uart_putdec(seq);
                    uart_puts("\n");
                }
            }
            /* Lease-Erneuerung gegen den SLIRP-Server exerzieren (unicast REQUEST mit ciaddr).
             * Der natuerliche T1-Trigger liegt bei Lease/2 -- fuer den Test sofort nach dem
             * ersten Ping anstossen (Gateway-ARP + Bindung stehen dann bereits; so wird die
             * Erneuerung zuverlaessig innerhalb des QEMU-Fensters erreicht, auch wenn der Host
             * gerade ausgelastet ist und der Poll-Loop nur wenige Pings schafft). */
            if (!renew_done && sent >= 1) {
                renew_done = 1;
                dhcp_renew(&g_nif);
            }
        }

        /* T1.11-Guardian: nachdem echter Verkehr lief (RX/TX/Ticks), einmalig den Zustand
         * des Big-Net-Locks melden. max-Tiefe>=2 belegt, dass verriegelte Einstiege
         * verschachtelt AUFTRETEN -- bereits beim Setup (httpd_listen->tcp_listen) UND zur
         * Laufzeit (verschachtelte Sender, z.B. dns_resolve->udp_send->ip_send->arp_request
         * = Tiefe 3) -> das Lock MUSS reentrant sein. Die Obergrenze (<=6) faengt ein
         * geleicktes enter (fehlendes leave) ab; unbalanciert=nein belegt, dass jeder enter
         * durch genau ein leave geschlossen wird. DASS die Reentranz LAST-TRAGEND ist, zeigt
         * die Mutation "nicht-reentrant" separat (Self-Deadlock am ersten verschachtelten
         * Aufruf httpd_listen->tcp_listen -> Harness haengt). */
        if (sent >= 1 && !netlock_announced) {
            netlock_announced = 1;
            uart_puts("[netlocktest] Big-Net-Lock reentrant: max-Tiefe=");
            uart_putdec(net_lock_maxdepth());
            uart_puts(" unbalanciert=");
            uart_puts(net_lock_unbalanced() ? "JA(FEHLER)\n" : "nein\n");
        }
    }
}
