/*
 * drivers/usb/uvc.c  --  UVC-Klassen-Layer
 *
 * Ganzer Inhalt #ifdef VISION -> ohne das Flag leeres Objekt (wie fpctx.S/vi_parallel.c), der
 * Kernel bleibt byte-identisch. Reine, spec-definierte Klassen-Logik (siehe include/uvc.h):
 * Config-Deskriptor-Parse, VS_PROBE-Payload, Bulk-Frame-Assembly -- alles bounds-checked (Geraete-
 * Deskriptoren + Kamera-Payloads sind UNTRUSTED). Der HW-Transfer-Glue (Control/Bulk ueber dwc2/
 * xHCI + Enumeration am echten Pi4) ist die On-Device-Naht A4.1b und treibt diese Logik an.
 */
#ifdef VISION

#include <stdint.h>
#include "uvc.h"
#include "uart.h"

int uvc_parse_config(const uint8_t *cfg, int len, uvc_stream_t *out)
{
    out->found = 0; out->vs_interface = -1; out->ep_addr = 0; out->ep_mps = 0;
    if (!cfg || len < 2) { return -1; }
    int i = 0, cur_vs = 0;
    while (i + 2 <= len) {
        int blen  = cfg[i];
        int btype = cfg[i + 1];
        if (blen < 2 || i + blen > len) { break; }        /* malformiert -> Stopp (kein OOB) */
        if (btype == 0x04) {                               /* INTERFACE-Deskriptor */
            if (i + 7 <= len) {
                int iclass = cfg[i + 5], isub = cfg[i + 6];
                cur_vs = (iclass == 0x0E && isub == 0x02); /* CC_VIDEO / VideoStreaming */
                if (cur_vs && out->vs_interface < 0) { out->vs_interface = cfg[i + 2]; }
            }
        } else if (btype == 0x05 && cur_vs) {              /* ENDPOINT im VS-Interface */
            if (i + 6 <= len) {
                int addr = cfg[i + 2], attr = cfg[i + 3];
                int mps  = cfg[i + 4] | (cfg[i + 5] << 8);
                if ((attr & 0x03) == 0x02 && (addr & 0x80)) {   /* Bulk IN */
                    out->ep_addr = addr; out->ep_mps = mps; out->found = 1;
                }
            }
        }
        i += blen;
    }
    return out->found ? 0 : -1;
}

void uvc_build_probe(uint8_t *buf, int len, int format_index, int frame_index, uint32_t frame_interval)
{
    for (int k = 0; k < len; k++) { buf[k] = 0; }
    if (len < 26) { return; }
    buf[0] = 0x01; buf[1] = 0x00;                          /* bmHint: dwFrameInterval fixiert */
    buf[2] = (uint8_t)format_index;                        /* bFormatIndex */
    buf[3] = (uint8_t)frame_index;                         /* bFrameIndex */
    buf[4] = (uint8_t)(frame_interval & 0xFF);             /* dwFrameInterval (LE32, 100-ns-Einheiten) */
    buf[5] = (uint8_t)((frame_interval >> 8) & 0xFF);
    buf[6] = (uint8_t)((frame_interval >> 16) & 0xFF);
    buf[7] = (uint8_t)((frame_interval >> 24) & 0xFF);
}

uint32_t uvc_probe_max_frame_size(const uint8_t *buf, int len)
{
    if (!buf || len < 22) { return 0; }                    /* dwMaxVideoFrameSize @ Offset 18 */
    return (uint32_t)buf[18] | ((uint32_t)buf[19] << 8)
         | ((uint32_t)buf[20] << 16) | ((uint32_t)buf[21] << 24);
}

int uvc_payload_append(const uint8_t *pkt, int pkt_len, uint8_t *frame, int frame_cap,
                       int *frame_pos, int *done)
{
    *done = 0;
    if (!pkt || pkt_len < 2) { return 0; }
    int hlen   = pkt[0];
    int bmInfo = pkt[1];
    if (hlen < 2 || hlen > pkt_len) { return 0; }          /* malformierter Header -> verwerfen */
    int data_len = pkt_len - hlen;
    int n = 0;
    for (int k = 0; k < data_len && *frame_pos < frame_cap; k++) {
        frame[(*frame_pos)++] = pkt[hlen + k];
        n++;
    }
    if (bmInfo & 0x02) { *done = 1; }                      /* EOF-Bit im bmHeaderInfo */
    return n;
}

/* --- Boot-Selbsttest mit synthetischen UVC-Daten (kein echtes Geraet noetig) --- */
void uvc_selftest(void)
{
    /* Config: Config-Deskriptor(9) + VS-Interface(0x0E/0x02, if=3, 1 EP) + Bulk-IN-EP 0x81, mps=512. */
    static const uint8_t cfg[] = {
        9, 0x02, 25, 0, 1, 1, 0, 0x80, 50,                 /* Config-Deskriptor (wTotalLength=25) */
        9, 0x04, 3, 0, 1, 0x0E, 0x02, 0x00, 0,             /* Interface: VideoStreaming */
        7, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00              /* Endpoint: Bulk IN, wMaxPacketSize=512 */
    };
    uvc_stream_t s;
    int parse_ok = (uvc_parse_config(cfg, (int)sizeof(cfg), &s) == 0)
                   && s.found && s.ep_addr == 0x81 && s.ep_mps == 512 && s.vs_interface == 3;

    uint8_t probe[26];
    uvc_build_probe(probe, (int)sizeof(probe), 1, 1, 333333u);   /* ~30 fps */
    uint32_t itv = (uint32_t)probe[4] | ((uint32_t)probe[5] << 8)
                 | ((uint32_t)probe[6] << 16) | ((uint32_t)probe[7] << 24);
    int probe_ok = (probe[2] == 1) && (probe[3] == 1) && (itv == 333333u);

    /* Frame aus 2 Bulk-Payloads: pkt1 (Header 2B, kein EOF, Daten [1,2,3]), pkt2 (EOF, Daten [4,5]). */
    static const uint8_t pkt1[] = { 2, 0x00, 1, 2, 3 };
    static const uint8_t pkt2[] = { 2, 0x02, 4, 5 };
    uint8_t frame[16];
    int pos = 0, done = 0;
    uvc_payload_append(pkt1, (int)sizeof(pkt1), frame, (int)sizeof(frame), &pos, &done);
    int mid_ok = (pos == 3) && (done == 0);
    uvc_payload_append(pkt2, (int)sizeof(pkt2), frame, (int)sizeof(frame), &pos, &done);
    int asm_ok = mid_ok && (pos == 5) && (done == 1)
                 && frame[0] == 1 && frame[1] == 2 && frame[2] == 3 && frame[3] == 4 && frame[4] == 5;

    uart_begin();
    if (parse_ok && probe_ok && asm_ok) {
        uart_puts("    [uvc] class-layer: parse(vs+bulk-in)=ok probe=ok assemble(2pkt+eof)=ok\n");
    } else {
        uart_puts("    [uvc] class-layer: FEHLER (parse=");
        uart_puts(parse_ok ? "ok" : "x");
        uart_puts(" probe=");
        uart_puts(probe_ok ? "ok" : "x");
        uart_puts(" assemble=");
        uart_puts(asm_ok ? "ok" : "x");
        uart_puts(")\n");
    }
    uart_end();
}

#endif /* VISION */
