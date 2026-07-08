/*
 * net/dev_agent.c  --  Dev-Remote-Interface (UDP), UDP-Agent 
 *
 * Ganzer Inhalt #ifdef DEV_REMOTE -> ohne das Flag leeres Objekt (Kernel byte-identisch, kein Byte
 * Dev-Interface in Produktion). Bindet den Protokoll-Kern (dev_remote.c) an das System:
 *   - UDP-Socket auf Port 5599 (GENET) in einem eigenen Kern-0-Netz-Task
 *   - Konsolen-Mirror -> OUTPUT (Ring, best-effort)
 *   - Framebuffer -> SCREEN_DATA (Downsample + RLE, mehrpaketig)
 *   - KEY/MOUSE -> gui_input-Queue (SPSC-sicher ueber ein Staging-Ring, drain im Timer-Tick)
 *   - FILE_* -> hdd0/hdd1 (inkl. kernel8.img -> Boot-FAT via privilegiertem Schreibpfad)
 *   - RESTART -> BCM2711-PM-Watchdog-Full-Reset
 *
 * Die Dispatch-Logik (dev_agent_input) haengt nur an einer Ops-Abstraktion -> in QEMU mit Mock-Ops
 * deterministisch verifizierbar (dev_agent_selftest, Boot-Marker). Der reale GENET-Pfad wuerde in
 * QEMU raspi4b einen synchronen External-Abort ausloesen (kein NIC emuliert) -> der Live-Task startet
 * NUR bei gueltigem Firmware-DTB (mmu_ram_from_dtb() == echter Pi4). Plan: docs/architecture/20.
 */
#ifdef DEV_REMOTE

#include <stdint.h>
#include "dev_agent.h"
#include "dev_remote.h"
#include "net.h"
#include "genet.h"
#include "dhcp.h"
#include "uart.h"
#include "fb.h"
#include "gui_input.h"
#include "gui_abi.h"
#include "vfs.h"
#include "sched.h"
#include "mmio.h"
#include "mmu.h"    /* mmu_ram_from_dtb(): 1 = gueltiger Firmware-DTB (echter Pi4), 0 = QEMU-Grobkarte */

/* --- kleine Little-Endian-Leser (untrusted Netz-Daten) --- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void dev_screen_send(dev_agent_t *a, uint16_t req_seq);

/* ======================================================================================
 *  Transport-UNABHAENGIGE Dispatch-Logik (QEMU-verifizierbar ueber die Ops-Abstraktion)
 * ==================================================================================== */

void dev_agent_init(dev_agent_t *a, const dev_ops_t *ops, void *user,
                    uint8_t *file_buf, uint32_t file_cap)
{
    a->ops = ops;
    a->user = user;
    a->file_buf = file_buf;
    a->file_cap = file_cap;
    a->file_partition = 0;
    a->file_path[0] = '\0';
    a->file_active = 0;
    for (unsigned i = 0; i < sizeof(a->file.got); i++) { a->file.got[i] = 0; }
    a->file.total_chunks = 0;
    a->file.recv_count = 0;
}

void dev_agent_input(dev_agent_t *a, const uint8_t *pkt, int len)
{
    dev_hdr_t h;
    if (dev_parse_hdr(pkt, len, &h) != 0) { return; }   /* Magic/Laenge -> untrusted verworfen */
    const uint8_t *pl = pkt + DEV_HDR_LEN;
    int pn = len - DEV_HDR_LEN;                          /* >= 0 (dev_parse_hdr sicherte len >= 8) */

    switch (h.type) {
    case DEV_PING:
        a->ops->send(a, DEV_PONG, 0, h.seq, 0, 0);
        break;

    case DEV_KEY:
        for (int i = 0; i < pn; i++) { a->ops->inject_key(a, pl[i]); }
        break;

    case DEV_MOUSE:
        if (pn >= 5) {
            a->ops->inject_mouse(a, (int16_t)rd16(pl), (int16_t)rd16(pl + 2), pl[4]);
        }
        break;

    case DEV_FILE_BEGIN: {
        /* Layout: partition(1) pathlen(1) path[pathlen] total_size(4) chunk_size(4) */
        if (pn < 2) { break; }
        int partition = pl[0];
        uint32_t pathlen = pl[1];
        if (pathlen == 0 || pathlen >= sizeof(a->file_path)) { break; }
        if ((uint32_t)pn < 2u + pathlen + 8u) { break; }
        uint32_t total = rd32(pl + 2 + pathlen);
        uint32_t chunk = rd32(pl + 2 + pathlen + 4);
        if (total == 0 || total > a->file_cap) {
            a->ops->send(a, DEV_RESULT, 0, h.seq, (const uint8_t *)"ERRsize", 7);
            break;
        }
        if (chunk == 0) { chunk = 1; }
        for (uint32_t i = 0; i < pathlen; i++) { a->file_path[i] = (char)pl[2 + i]; }
        a->file_path[pathlen] = '\0';
        a->file_partition = (partition ? 1 : 0);
        dev_file_reset(&a->file, a->file_buf, a->file_cap, total, chunk);
        a->file_active = 1;
        a->ops->send(a, DEV_ACK, 0, h.seq, 0, 0);
        break;
    }

    case DEV_FILE_CHUNK: {
        /* Layout: index(4) data[...]. Idempotent -> auch Duplikate bestaetigen (Host-Retransmit). */
        if (!a->file_active || pn < 4) { break; }
        dev_file_chunk(&a->file, rd32(pl), pl + 4, (uint32_t)(pn - 4));
        a->ops->send(a, DEV_ACK, 0, h.seq, 0, 0);
        break;
    }

    case DEV_FILE_END: {
        if (!a->file_active) {
            a->ops->send(a, DEV_RESULT, 0, h.seq, (const uint8_t *)"ERRnofile", 9);
            break;
        }
        a->file_active = 0;
        if (!dev_file_complete(&a->file)) {
            a->ops->send(a, DEV_RESULT, 0, h.seq, (const uint8_t *)"ERRincomplete", 13);
            break;
        }
        /* Optionale Pruefsumme (Summe der Bytes mod 2^32) verifizieren, falls mitgeschickt. */
        if (pn >= 4) {
            uint32_t want = rd32(pl), sum = 0;
            for (uint32_t i = 0; i < a->file.total_size; i++) { sum += a->file_buf[i]; }
            if (sum != want) {
                a->ops->send(a, DEV_RESULT, 0, h.seq, (const uint8_t *)"ERRcksum", 8);
                break;
            }
        }
        int rc = a->ops->write_file(a, a->file_partition, a->file_path,
                                    a->file_buf, a->file.total_size);
        if (rc == 0) { a->ops->send(a, DEV_RESULT, 0, h.seq, (const uint8_t *)"OK", 2); }
        else         { a->ops->send(a, DEV_RESULT, 0, h.seq, (const uint8_t *)"ERRwrite", 8); }
        break;
    }

    case DEV_SCREEN_REQ:
        dev_screen_send(a, h.seq);
        break;

    case DEV_RESTART:
        a->ops->send(a, DEV_RESULT, 0, h.seq, (const uint8_t *)"OK", 2);
        a->ops->reboot(a);   /* kehrt am Pi4 nicht zurueck; Mock zaehlt nur */
        break;

    default:
        break;
    }
}

/* SCREEN_REQ: Framebuffer holen, auf <= DEV_SCR_MAXPX Pixel downsamplen, RLE, mehrpaketig senden.
 * Paket 0 traegt einen 6-Byte-Kopf (w,h,nchunks als u16), gefolgt von RLE; Paket i>0 nur RLE.
 * Die seq jedes SCREEN_DATA-Pakets ist der Chunk-Index (der Host reassembliert danach). */
#define DEV_SCR_CHUNK 1400                       /* Nutzlast je Paket (< 1472, MTU-sicher) */
#define DEV_SCR_MAXPX 8192                       /* nach Downsampling -> begrenzte Paketzahl */
static uint32_t s_scr_px[DEV_SCR_MAXPX];
static uint8_t  s_scr_rle[DEV_SCR_MAXPX * 6];

static void dev_screen_send(dev_agent_t *a, uint16_t req_seq)
{
    dev_fb_view_t fb;
    if (a->ops->fb_get(a, &fb) != 0 || fb.w <= 0 || fb.h <= 0) {
        a->ops->send(a, DEV_RESULT, 0, req_seq, (const uint8_t *)"ERRnofb", 7);
        return;
    }
    int step = 1;
    while ((fb.w / step) * (fb.h / step) > DEV_SCR_MAXPX) { step++; }
    int dw = fb.w / step, dh = fb.h / step;
    int n = 0;
    for (int y = 0; y < dh; y++) {
        const volatile uint32_t *row = fb.px + (uint32_t)(y * step) * (uint32_t)fb.stride_px;
        for (int x = 0; x < dw; x++) { s_scr_px[n++] = row[x * step] & 0x00FFFFFFu; }
    }
    int rl = dev_rle_encode((const uint32_t *)s_scr_px, n, s_scr_rle, (int)sizeof(s_scr_rle));
    if (rl < 0) { a->ops->send(a, DEV_RESULT, 0, req_seq, (const uint8_t *)"ERRrle", 6); return; }

    int first_cap = DEV_SCR_CHUNK - 6;
    int nchunks = 1;
    if (rl > first_cap) { nchunks += (rl - first_cap + DEV_SCR_CHUNK - 1) / DEV_SCR_CHUNK; }

    uint8_t pkt[DEV_SCR_CHUNK];
    int off = 0;
    for (int c = 0; c < nchunks; c++) {
        int plen;
        if (c == 0) {
            pkt[0] = (uint8_t)(dw & 0xFF);       pkt[1] = (uint8_t)((dw >> 8) & 0xFF);
            pkt[2] = (uint8_t)(dh & 0xFF);       pkt[3] = (uint8_t)((dh >> 8) & 0xFF);
            pkt[4] = (uint8_t)(nchunks & 0xFF);  pkt[5] = (uint8_t)((nchunks >> 8) & 0xFF);
            int take = (rl < first_cap) ? rl : first_cap;
            for (int i = 0; i < take; i++) { pkt[6 + i] = s_scr_rle[off + i]; }
            off += take; plen = 6 + take;
        } else {
            int rem = rl - off;
            int take = (rem < DEV_SCR_CHUNK) ? rem : DEV_SCR_CHUNK;
            for (int i = 0; i < take; i++) { pkt[i] = s_scr_rle[off + i]; }
            off += take; plen = take;
        }
        a->ops->send(a, DEV_SCREEN_DATA, 0, (uint16_t)c, pkt, plen);
    }
}

/* ======================================================================================
 *  Realer System-Pfad (nur am Pi4 aktiv) -- Ops-Implementierungen + Netz-Task
 * ==================================================================================== */

#define DEV_FILE_CAP (2u * 1024u * 1024u)        /* haelt kernel8.img (Netz-Update) mit Reserve */
static uint8_t     s_file_buf[DEV_FILE_CAP];
static dev_agent_t g_agent;
static netif_t     s_nif;                         /* BSS-genullt -> genet_init setzt Fallback-MAC */

/* Peer-Endpunkt (der Controller); aus dem ersten empfangenen Paket gelernt. */
static struct { netif_t *nif; ip4_addr_t ip; uint16_t port; int valid; } s_peer;

static void real_send(dev_agent_t *a, uint8_t type, uint8_t flags, uint16_t seq,
                      const uint8_t *pl, int len)
{
    (void)a;
    if (!s_peer.valid) { return; }
    static uint8_t buf[DEV_HDR_LEN + DEV_SCR_CHUNK + 8];
    if (len < 0) { len = 0; }
    if (len > (int)sizeof(buf) - DEV_HDR_LEN) { len = (int)sizeof(buf) - DEV_HDR_LEN; }
    dev_build_hdr(buf, (int)sizeof(buf), type, flags, seq);
    for (int i = 0; i < len; i++) { buf[DEV_HDR_LEN + i] = pl[i]; }
    udp_send(s_peer.nif, s_peer.ip, s_peer.port, DEV_PORT, buf, (uint16_t)(DEV_HDR_LEN + len));
}

static int real_write_file(dev_agent_t *a, int partition, const char *path,
                           const uint8_t *buf, uint32_t len)
{
    (void)a;
    char full[80];
    const char *pfx = partition ? "hdd1:" : "hdd0:";
    int k = 0;
    for (const char *p = pfx;  *p && k < (int)sizeof(full) - 1; p++) { full[k++] = *p; }
    for (const char *p = path; *p && k < (int)sizeof(full) - 1; p++) { full[k++] = *p; }
    full[k] = '\0';
    /* hdd0 (Boot-Partition, kernel8.img) ist read-only gemountet -> privilegierter Kernel-Pfad;
     * hdd1 ist normal beschreibbar. */
    int r = (partition == 0) ? vfs_write_file_priv(full, buf, len)
                             : vfs_write_file(full, buf, len);
    return (r < 0) ? -1 : 0;
}

/* --- Konsolen-Mirror-Ring: Producer = putc_raw (Kern 0, unter UART-Lock, IRQs maskiert);
 *     Consumer = dev_net_task. Best-effort (voll -> verwerfen). --- */
#define DEV_OUT_RING 4096u
static volatile uint8_t  s_out[DEV_OUT_RING];
static volatile uint32_t s_out_head, s_out_tail;

void dev_console_tee(char c)
{
    uint32_t hh = s_out_head;
    uint32_t nn = (hh + 1u) & (DEV_OUT_RING - 1u);
    if (nn == s_out_tail) { return; }
    s_out[hh] = (uint8_t)c;
    __asm__ volatile("dmb ish" ::: "memory");
    s_out_head = nn;
}

static void dev_output_flush(void)
{
    if (!s_peer.valid) { return; }
    uint8_t body[1024];
    int n = 0;
    while (n < (int)sizeof(body)) {
        uint32_t t = s_out_tail;
        if (t == s_out_head) { break; }
        body[n++] = s_out[t];
        s_out_tail = (t + 1u) & (DEV_OUT_RING - 1u);
    }
    if (n > 0) { real_send(&g_agent, DEV_OUTPUT, 0, 0, body, n); }
}

/* --- Eingabe-Staging-Ring: Producer = dev_net_task (Kern-0-Thread); Consumer = gui_input_tick
 *     (Timer-IRQ, Kern 0). So bleibt gui_input_push single-producer (nur aus dem Timer-Tick). --- */
#define DEV_IN_RING 64u
static gui_event_t       s_in[DEV_IN_RING];
static volatile uint32_t s_in_head, s_in_tail;
static int               s_mx, s_my;             /* virtueller Dev-Cursor (fuer MOUSE-Deltas) */

static void dev_in_push(const gui_event_t *ev)
{
    uint32_t hh = s_in_head;
    uint32_t nn = (hh + 1u) & (DEV_IN_RING - 1u);
    if (nn == s_in_tail) { return; }
    s_in[hh] = *ev;
    __asm__ volatile("dmb ish" ::: "memory");
    s_in_head = nn;
}

void dev_input_drain(void)   /* aus gui_input_tick (Timer-IRQ, Kern 0) */
{
    int n = 0;
    while (n++ < 16) {
        uint32_t t = s_in_tail;
        if (t == s_in_head) { break; }
        __asm__ volatile("dmb ish" ::: "memory");
        gui_event_t ev = s_in[t];
        s_in_tail = (t + 1u) & (DEV_IN_RING - 1u);
        gui_input_push(&ev);
    }
}

/* --- Konsolen-Eingabe-Ring (ferngesteuerte Tasten -> Shell UND GUI): Producer = dev_net_task,
 *     Consumer = console_getc(_nb) (Shell-Lese-Syscall bzw. gui_input_tick-Tastatur-Poll). Ein
 *     injiziertes Byte erreicht so beides -- die Shell direkt, GUI-Apps ueber gui_input_tick, das
 *     ebenfalls console_getc_nb pollt. --- */
#define DEV_KBD_RING 256u
static volatile uint8_t  s_kbd[DEV_KBD_RING];
static volatile uint32_t s_kbd_head, s_kbd_tail;

static void dev_kbd_inject(uint8_t c)
{
    uint32_t hh = s_kbd_head;
    uint32_t nn = (hh + 1u) & (DEV_KBD_RING - 1u);
    if (nn == s_kbd_tail) { return; }   /* voll -> verwerfen */
    s_kbd[hh] = c;
    __asm__ volatile("dmb ish" ::: "memory");
    s_kbd_head = nn;
}

int dev_console_inject_get(void)   /* aus console_getc(_nb); -1 = nichts anliegend */
{
    uint32_t t = s_kbd_tail;
    if (t == s_kbd_head) { return -1; }
    __asm__ volatile("dmb ish" ::: "memory");
    uint8_t c = s_kbd[t];
    s_kbd_tail = (t + 1u) & (DEV_KBD_RING - 1u);
    return (int)c;
}

static void real_inject_key(dev_agent_t *a, uint8_t key)
{
    (void)a;
    dev_kbd_inject(key);   /* -> Konsolen-Eingabe (Shell direkt + GUI via gui_input_tick) */
}

static void real_inject_mouse(dev_agent_t *a, int16_t dx, int16_t dy, uint8_t buttons)
{
    (void)a;
    const fb_t *fb = fb_get();
    int maxx = fb ? (int)fb->width  - 1 : 639;
    int maxy = fb ? (int)fb->height - 1 : 479;
    s_mx += dx; s_my += dy;
    if (s_mx < 0) { s_mx = 0; } if (s_mx > maxx) { s_mx = maxx; }
    if (s_my < 0) { s_my = 0; } if (s_my > maxy) { s_my = maxy; }
    gui_event_t ev = { 0, 0, 0, 0, 0, 0 };
    ev.type    = GUI_EV_MOUSE;
    ev.buttons = buttons;
    ev.x       = (int16_t)s_mx;
    ev.y       = (int16_t)s_my;
    dev_in_push(&ev);
}

static int real_fb_get(dev_agent_t *a, dev_fb_view_t *out)
{
    (void)a;
    const fb_t *fb = fb_get();
    if (!fb || !fb->base || fb->width == 0 || fb->height == 0) { return -1; }
    out->px = (const volatile uint32_t *)fb->base;
    out->w  = (int)fb->width;
    out->h  = (int)fb->height;
    out->stride_px = (int)(fb->pitch / 4u);
    return 0;
}

/* BCM2711 Power-Management-Watchdog: Full-Reset (der Device-Bereich 0xFC000000+ ist bereits
 * als Device gemappt -> direkt via mmio erreichbar). Nur auf HW aufgerufen (RESTART-Paket). */
#define DEV_PM_BASE        0xFE100000UL
#define DEV_PM_RSTC        (DEV_PM_BASE + 0x1CUL)
#define DEV_PM_WDOG        (DEV_PM_BASE + 0x24UL)
#define DEV_PM_PASSWD      0x5A000000u
#define DEV_PM_WRCFG_FULL  0x00000020u
#define DEV_PM_WRCFG_CLR   0xFFFFFFCFu

static void real_reboot(dev_agent_t *a)
{
    (void)a;
    uart_puts("    [dev] RESTART -> PM-Watchdog Full-Reset\n");
    mmio_write32(DEV_PM_WDOG, DEV_PM_PASSWD | 10u);
    uint32_t r = mmio_read32(DEV_PM_RSTC);
    r = (r & DEV_PM_WRCFG_CLR) | DEV_PM_WRCFG_FULL;
    mmio_write32(DEV_PM_RSTC, DEV_PM_PASSWD | r);
    for (;;) { __asm__ volatile("wfe"); }
}

static const dev_ops_t s_real_ops = {
    real_send, real_write_file, real_inject_key, real_inject_mouse, real_fb_get, real_reboot
};

/* UDP-Empfang (laeuft im Netz-Lock aus eth_input -> udp_input): Peer lernen + dispatchen. */
static void dev_udp_rx(netif_t *nif, ip4_addr_t src_ip, uint16_t src_port,
                       const uint8_t *data, uint16_t len)
{
    s_peer.nif = nif; s_peer.ip = src_ip; s_peer.port = src_port; s_peer.valid = 1;
    dev_agent_input(&g_agent, data, (int)len);
}

/* Statische Netzkonfiguration des Dev-Interface (Nutzer-Vorgabe) -> kein DHCP, deterministisch
 * erreichbar unter DEV_STATIC_IP:5599. Der Host verbindet sich genau hierhin. Bei Bedarf aendern. */
#define DEV_STATIC_IP   IP4(192, 168, 10, 244)
#define DEV_STATIC_MASK IP4(255, 255, 255, 0)
#define DEV_STATIC_GW   IP4(192, 168, 10, 1)

static void dev_net_task(void *arg)
{
    (void)arg;
    if (genet_init(&s_nif) != 0) {
        uart_puts("    [dev] net: kein GENET/PHY-Link -> Live-Agent inaktiv\n");
        return;   /* Task endet sauber (kein Link/kein Kabel) */
    }
    /* Statische IP setzen (kein DHCP): der Stack sendet mit s_nif.ip als Quelle, loest den
     * Same-Subnet-Controller per ARP auf. Reines On-Subnet-UDP -> Gateway wird nicht gebraucht. */
    s_nif.ip      = DEV_STATIC_IP;
    s_nif.netmask = DEV_STATIC_MASK;
    s_nif.gateway = DEV_STATIC_GW;
    udp_bind(DEV_PORT, dev_udp_rx);
    uart_puts("    [dev] net: Agent aktiv, statische IP ");
    net_print_ip(s_nif.ip);
    uart_puts(" UDP-Port 5599\n");
    for (;;) {
        genet_poll();
        dev_output_flush();
        sched_yield();
    }
}

/* ======================================================================================
 *  Dispatch-Selbsttest mit Mock-Ops (synthetisch, deterministisch, QEMU-verifizierbar)
 * ==================================================================================== */

static struct {
    int     sent_n;
    uint8_t last_type; uint16_t last_seq;
    uint8_t last_pl[64]; int last_len;
    int     wrote, wrote_part; char wrote_path[64]; uint32_t wrote_len, wrote_sum;
    char    keys[32]; int nkeys;
    int     m_dx, m_dy; uint8_t m_btn; int mouse_n;
    int     reboot_n;
    int     scr_w, scr_h, scr_nchunks, scr_seen;
    uint8_t scr_rle[256]; int scr_rle_n;
} MK;

static void mk_send(dev_agent_t *a, uint8_t type, uint8_t flags, uint16_t seq,
                    const uint8_t *pl, int len)
{
    (void)a; (void)flags;
    MK.sent_n++; MK.last_type = type; MK.last_seq = seq;
    MK.last_len = (len < 0) ? 0 : (len > 64 ? 64 : len);
    for (int i = 0; i < MK.last_len; i++) { MK.last_pl[i] = pl[i]; }
    if (type == DEV_SCREEN_DATA) {
        int start = 0;
        if (seq == 0 && len >= 6) {
            MK.scr_w = pl[0] | (pl[1] << 8);
            MK.scr_h = pl[2] | (pl[3] << 8);
            MK.scr_nchunks = pl[4] | (pl[5] << 8);
            start = 6;
        }
        for (int i = start; i < len && MK.scr_rle_n < (int)sizeof(MK.scr_rle); i++) {
            MK.scr_rle[MK.scr_rle_n++] = pl[i];
        }
        MK.scr_seen++;
    }
}
static int mk_write(dev_agent_t *a, int part, const char *path, const uint8_t *buf, uint32_t len)
{
    (void)a;
    MK.wrote = 1; MK.wrote_part = part; MK.wrote_len = len;
    uint32_t s = 0; for (uint32_t i = 0; i < len; i++) { s += buf[i]; } MK.wrote_sum = s;
    int k = 0; for (; path[k] && k < 63; k++) { MK.wrote_path[k] = path[k]; } MK.wrote_path[k] = '\0';
    return 0;
}
static void mk_key(dev_agent_t *a, uint8_t key)   { (void)a; if (MK.nkeys < 31) { MK.keys[MK.nkeys++] = (char)key; } }
static void mk_mouse(dev_agent_t *a, int16_t dx, int16_t dy, uint8_t btn)
{ (void)a; MK.m_dx = dx; MK.m_dy = dy; MK.m_btn = btn; MK.mouse_n++; }
static const uint32_t MK_FB[8] = { 0x111111, 0x111111, 0x111111, 0x111111,
                                   0x111111, 0x111111, 0x111111, 0x111111 };
static int mk_fb(dev_agent_t *a, dev_fb_view_t *out)
{ (void)a; out->px = (const volatile uint32_t *)MK_FB; out->w = 4; out->h = 2; out->stride_px = 4; return 0; }
static void mk_reboot(dev_agent_t *a) { (void)a; MK.reboot_n++; }
static const dev_ops_t s_mock_ops = { mk_send, mk_write, mk_key, mk_mouse, mk_fb, mk_reboot };

static int mkpkt(uint8_t *buf, uint8_t type, uint16_t seq, const uint8_t *pl, int pn)
{
    dev_build_hdr(buf, DEV_HDR_LEN + pn, type, 0, seq);
    for (int i = 0; i < pn; i++) { buf[DEV_HDR_LEN + i] = pl[i]; }
    return DEV_HDR_LEN + pn;
}

static int dev_agent_selftest(void)
{
    for (unsigned i = 0; i < sizeof(MK); i++) { ((uint8_t *)&MK)[i] = 0; }
    static uint8_t tbuf[64];
    dev_agent_t ta;
    dev_agent_init(&ta, &s_mock_ops, 0, tbuf, sizeof(tbuf));

    uint8_t pkt[64];
    int ok = 1;

    /* PING -> PONG (seq gespiegelt) */
    dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_PING, 0x77, 0, 0));
    ok = ok && MK.last_type == DEV_PONG && MK.last_seq == 0x77;

    /* KEY "Hi" -> zwei injizierte Zeichen */
    { const uint8_t k[] = { 'H', 'i' }; dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_KEY, 1, k, 2)); }
    ok = ok && MK.nkeys == 2 && MK.keys[0] == 'H' && MK.keys[1] == 'i';

    /* MOUSE dx=+5 dy=-3 btn=1 (Little-Endian int16) */
    { uint8_t m[5] = { 5, 0, 0xFD, 0xFF, 1 }; dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_MOUSE, 2, m, 5)); }
    ok = ok && MK.mouse_n == 1 && MK.m_dx == 5 && MK.m_dy == -3 && MK.m_btn == 1;

    /* FILE: begin(hdd1,"T.BIN",total=5,chunk=2) + Chunks OUT-OF-ORDER (2,0,1) + end(cksum=15)
     *   -> reassembliert {1,2,3,4,5} -> write_file(part=1,"T.BIN",5) -> RESULT "OK" */
    {
        uint8_t b[16]; int n = 0;
        b[n++] = 1; b[n++] = 5;                                  /* partition=1, pathlen=5 */
        b[n++] = 'T'; b[n++] = '.'; b[n++] = 'B'; b[n++] = 'I'; b[n++] = 'N';
        b[n++] = 5; b[n++] = 0; b[n++] = 0; b[n++] = 0;          /* total_size = 5 */
        b[n++] = 2; b[n++] = 0; b[n++] = 0; b[n++] = 0;          /* chunk_size = 2 */
        dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_FILE_BEGIN, 10, b, n));
    }
    ok = ok && MK.last_type == DEV_ACK && MK.last_seq == 10;
    { uint8_t c[5] = { 2, 0, 0, 0, 5 };       dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_FILE_CHUNK, 11, c, 5)); }  /* idx2 = [5]   */
    { uint8_t c[6] = { 0, 0, 0, 0, 1, 2 };    dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_FILE_CHUNK, 12, c, 6)); }  /* idx0 = [1,2] */
    { uint8_t c[6] = { 1, 0, 0, 0, 3, 4 };    dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_FILE_CHUNK, 13, c, 6)); }  /* idx1 = [3,4] */
    { uint8_t e[4] = { 15, 0, 0, 0 };         dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_FILE_END,   14, e, 4)); }
    ok = ok && MK.wrote == 1 && MK.wrote_part == 1 && MK.wrote_len == 5 && MK.wrote_sum == 15
            && MK.wrote_path[0] == 'T' && MK.wrote_path[1] == '.'
            && MK.last_type == DEV_RESULT && MK.last_pl[0] == 'O' && MK.last_pl[1] == 'K';

    /* SCREEN_REQ -> SCREEN_DATA: 4x2 einfarbig -> 1 Chunk, RLE = 1 Lauf (count 8, pixel 0x111111) */
    dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_SCREEN_REQ, 20, 0, 0));
    ok = ok && MK.scr_w == 4 && MK.scr_h == 2 && MK.scr_nchunks == 1 && MK.scr_seen == 1
            && MK.scr_rle_n >= 6 && MK.scr_rle[0] == 8 && MK.scr_rle[1] == 0
            && MK.scr_rle[2] == 0x11 && MK.scr_rle[3] == 0x11 && MK.scr_rle[4] == 0x11 && MK.scr_rle[5] == 0x00;

    /* RESTART -> RESULT "OK" + reboot() aufgerufen (Mock zaehlt, kein echter Reset) */
    dev_agent_input(&ta, pkt, mkpkt(pkt, DEV_RESTART, 30, 0, 0));
    ok = ok && MK.reboot_n == 1;

    return ok;
}

void dev_agent_start(void)
{
    dev_agent_init(&g_agent, &s_real_ops, 0, s_file_buf, DEV_FILE_CAP);
    s_mx = 320; s_my = 240;   /* virtueller Cursor mittig starten */

    int ok = dev_agent_selftest();
    uart_begin();
    if (ok) {
        uart_puts("    [dev] agent: dispatch ping=ok key=ok mouse=ok file(ooo)=ok screen=ok restart=ok\n");
    } else {
        uart_puts("    [dev] agent: dispatch FEHLER\n");
    }
    uart_end();

    /* Live-Netz-Agent NUR am echten Pi4 starten: in QEMU raspi4b wuerde JEDER GENET-Zugriff einen
     * synchronen External-Abort ausloesen (kein NIC emuliert). Diskriminator = ein GUELTIGER
     * Firmware-DTB (mmu_ram_from_dtb()): QEMU uebergibt zwar einen rohen x0 (g_dtb_ptr != 0), aber
     * ohne gueltigen /memory-DTB -> Grobkarte. Der Dispatch (oben) ist bereits verifiziert; der reale
     * UDP-Pfad laeuft ausschliesslich auf HW. */
    if (mmu_ram_from_dtb()) {
        task_create(dev_net_task, 0, 6, "devrem");
    } else {
        uart_begin();
        uart_puts("    [dev] net: QEMU (kein DTB) -> GENET uebersprungen; Live-Agent nur am Pi4\n");
        uart_end();
    }
}

#endif /* DEV_REMOTE */
