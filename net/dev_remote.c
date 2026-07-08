/*
 * net/dev_remote.c  --  Dev-Remote-Interface (UDP), Protokoll-Kern (D1).
 *
 * Ganzer Inhalt #ifdef DEV_REMOTE -> ohne das Flag leeres Objekt (der Kernel bleibt byte-identisch,
 * kein Byte Dev-Interface in Produktion). Reine Protokoll-Logik (untrusted Netz-Daten -> alles
 * bounds-checked).
 */
#ifdef DEV_REMOTE

#include <stdint.h>
#include "dev_remote.h"
#include "uart.h"

int dev_parse_hdr(const uint8_t *pkt, int len, dev_hdr_t *out)
{
    if (!pkt || len < DEV_HDR_LEN) { return -1; }
    uint32_t magic = (uint32_t)pkt[0] | ((uint32_t)pkt[1] << 8)
                   | ((uint32_t)pkt[2] << 16) | ((uint32_t)pkt[3] << 24);
    if (magic != DEV_MAGIC) { return -1; }
    out->type  = pkt[4];
    out->flags = pkt[5];
    out->seq   = (uint16_t)(pkt[6] | (pkt[7] << 8));
    return 0;
}

int dev_build_hdr(uint8_t *buf, int cap, uint8_t type, uint8_t flags, uint16_t seq)
{
    if (!buf || cap < DEV_HDR_LEN) { return -1; }
    buf[0] = (uint8_t)(DEV_MAGIC & 0xFF);
    buf[1] = (uint8_t)((DEV_MAGIC >> 8) & 0xFF);
    buf[2] = (uint8_t)((DEV_MAGIC >> 16) & 0xFF);
    buf[3] = (uint8_t)((DEV_MAGIC >> 24) & 0xFF);
    buf[4] = type;
    buf[5] = flags;
    buf[6] = (uint8_t)(seq & 0xFF);
    buf[7] = (uint8_t)((seq >> 8) & 0xFF);
    return DEV_HDR_LEN;
}

void dev_file_reset(dev_file_t *f, uint8_t *buf, uint32_t cap, uint32_t total_size, uint32_t chunk_size)
{
    f->buf = buf;
    f->cap = cap;
    f->total_size = total_size;
    f->chunk_size = chunk_size ? chunk_size : 1u;
    f->total_chunks = (total_size + f->chunk_size - 1u) / f->chunk_size;
    if (f->total_chunks > DEV_FILE_MAX_CHUNKS) { f->total_chunks = DEV_FILE_MAX_CHUNKS; }
    f->recv_count = 0;
    for (unsigned i = 0; i < sizeof(f->got); i++) { f->got[i] = 0; }
}

int dev_file_chunk(dev_file_t *f, uint32_t index, const uint8_t *data, uint32_t len)
{
    if (index >= f->total_chunks || index >= DEV_FILE_MAX_CHUNKS) { return 0; }
    uint32_t off = index * f->chunk_size;
    if (off > f->cap || len > f->cap - off) { return 0; }        /* overflow-frei -> kein OOB */
    if (f->got[index >> 3] & (1u << (index & 7))) { return 0; }  /* Duplikat */
    for (uint32_t k = 0; k < len; k++) { f->buf[off + k] = data[k]; }
    f->got[index >> 3] |= (uint8_t)(1u << (index & 7));
    f->recv_count++;
    return 1;
}

int dev_file_complete(const dev_file_t *f)
{
    return (f->total_chunks > 0) && (f->recv_count == f->total_chunks);
}

int dev_rle_encode(const uint32_t *px, int npx, uint8_t *out, int out_cap)
{
    int o = 0, i = 0;
    while (i < npx) {
        uint32_t p = px[i];
        int run = 1;
        while (i + run < npx && px[i + run] == p && run < 65535) { run++; }
        if (o + 6 > out_cap) { return -1; }
        out[o++] = (uint8_t)(run & 0xFF);
        out[o++] = (uint8_t)((run >> 8) & 0xFF);
        out[o++] = (uint8_t)(p & 0xFF);
        out[o++] = (uint8_t)((p >> 8) & 0xFF);
        out[o++] = (uint8_t)((p >> 16) & 0xFF);
        out[o++] = (uint8_t)((p >> 24) & 0xFF);
        i += run;
    }
    return o;
}

int dev_rle_decode(const uint8_t *in, int in_len, uint32_t *px, int npx_cap)
{
    int i = 0, n = 0;
    while (i + 6 <= in_len) {
        int run = in[i] | (in[i + 1] << 8);
        uint32_t p = (uint32_t)in[i + 2] | ((uint32_t)in[i + 3] << 8)
                   | ((uint32_t)in[i + 4] << 16) | ((uint32_t)in[i + 5] << 24);
        i += 6;
        for (int k = 0; k < run; k++) {
            if (n >= npx_cap) { return -1; }
            px[n++] = p;
        }
    }
    return n;
}

/* --- Boot-Selbsttest mit synthetischen Paketen/Daten (kein Netz noetig) --- */
void dev_remote_selftest(void)
{
    /* Header: bauen -> parsen; verfaelschtes Magic + abgeschnitten -> Ablehnung. */
    uint8_t hb[DEV_HDR_LEN];
    dev_build_hdr(hb, sizeof(hb), DEV_FILE_CHUNK, 0, 0x1234);
    dev_hdr_t h;
    int hdr_ok = (dev_parse_hdr(hb, DEV_HDR_LEN, &h) == 0) && h.type == DEV_FILE_CHUNK && h.seq == 0x1234;
    uint8_t bad = hb[0]; hb[0] ^= 0xFF;
    int hdr_rej = (dev_parse_hdr(hb, DEV_HDR_LEN, &h) < 0) && (dev_parse_hdr(hb, 4, &h) < 0);
    hb[0] = bad;

    /* Datei-Reassembly: 5 Byte, chunk_size 2 -> 3 Chunks; OUT-OF-ORDER + Duplikat. */
    static uint8_t fbuf[16];
    dev_file_t f;
    dev_file_reset(&f, fbuf, sizeof(fbuf), 5, 2);         /* Chunks: [0,1] [2,3] [4] */
    static const uint8_t c2[] = { 5 };
    static const uint8_t c0[] = { 1, 2 };
    static const uint8_t c1[] = { 3, 4 };
    dev_file_chunk(&f, 2, c2, 1);
    dev_file_chunk(&f, 0, c0, 2);
    int dup = dev_file_chunk(&f, 0, c0, 2);              /* Duplikat -> 0 */
    dev_file_chunk(&f, 1, c1, 2);
    int file_ok = dev_file_complete(&f) && (dup == 0)
                  && fbuf[0] == 1 && fbuf[1] == 2 && fbuf[2] == 3 && fbuf[3] == 4 && fbuf[4] == 5;

    /* RLE-Roundtrip: 3x + 1x + 2x = 3 Laeufe (je 6 Byte -> 18). */
    static const uint32_t src[] = { 0x11111111u, 0x11111111u, 0x11111111u, 0x22222222u, 0x33333333u, 0x33333333u };
    static uint8_t rbuf[64];
    static uint32_t dbuf[16];
    int rl = dev_rle_encode(src, 6, rbuf, sizeof(rbuf));
    int nd = dev_rle_decode(rbuf, rl, dbuf, 16);
    int rle_ok = (rl == 18) && (nd == 6)
                 && dbuf[0] == 0x11111111u && dbuf[2] == 0x11111111u
                 && dbuf[3] == 0x22222222u && dbuf[5] == 0x33333333u;

    uart_begin();
    if (hdr_ok && hdr_rej && file_ok && rle_ok) {
        uart_puts("    [dev] proto-core: header=ok reject=ok file-reassembly(ooo+dup)=ok rle-roundtrip=ok\n");
    } else {
        uart_puts("    [dev] proto-core: FEHLER (header="); uart_puts(hdr_ok ? "ok" : "x");
        uart_puts(" reject="); uart_puts(hdr_rej ? "ok" : "x");
        uart_puts(" file="); uart_puts(file_ok ? "ok" : "x");
        uart_puts(" rle="); uart_puts(rle_ok ? "ok" : "x"); uart_puts(")\n");
    }
    uart_end();
}

#endif /* DEV_REMOTE */
