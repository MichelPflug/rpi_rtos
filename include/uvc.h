/*
 * include/uvc.h  --  USB-Video-Class-(UVC-)Klassen-Layer.
 *
 * REINE Klassen-Logik (Config-Deskriptor-Parse, VS_PROBE/COMMIT-Payload, Bulk-Frame-Assembly) --
 * spec-definiert, deterministisch, QEMU-verifizierbar mit synthetischen Daten (untrusted Geraete-
 * Deskriptoren + Kamera-Payloads -> bounds-checked). Der HW-Transfer-Glue (Control/Bulk ueber
 * dwc2/xHCI am echten Pi4) ist die On-Device-Naht A4.1b, die genau diese Logik antreibt und
 * vi_cam_grab() mit einem YUYV-Frame fuellt.
 *
 * Implementierung: drivers/usb/uvc.c (Inhalt ganz #ifdef VISION; ohne das Flag leeres Objekt,
 * wie fpctx.S/vi_parallel.c -> Kernel byte-identisch).
 */
#ifndef RPI_RTOS_UVC_H
#define RPI_RTOS_UVC_H

#include <stdint.h>

/* Gefundener Video-Stream (VideoStreaming-Interface + Bulk-IN-Endpoint). */
typedef struct {
    int found;          /* 1 = ein Bulk-IN-Endpoint in einem VS-Interface gefunden */
    int vs_interface;   /* bInterfaceNumber des VideoStreaming-Interface (0x0E/0x02) */
    int ep_addr;        /* Bulk-IN-Endpoint-Adresse (0x8x) */
    int ep_mps;         /* wMaxPacketSize */
} uvc_stream_t;

/* Config-Deskriptor (untrusted) parsen: VideoStreaming-Interface (Class 0x0E, Subclass 0x02)
 * + dessen Bulk-IN-Endpoint finden. Bounds-checked. 0 = ok, -1 = keins gefunden/malformiert. */
int uvc_parse_config(const uint8_t *cfg, int len, uvc_stream_t *out);

/* VS_PROBE_CONTROL-Payload (26 B, UVC 1.0) bauen: Format-/Frame-Index + dwFrameInterval setzen. */
void uvc_build_probe(uint8_t *buf, int len, int format_index, int frame_index, uint32_t frame_interval);

/* dwMaxVideoFrameSize (Offset 18, LE32) aus einem PROBE/COMMIT-Antwort-Payload lesen (0 wenn zu kurz). */
uint32_t uvc_probe_max_frame_size(const uint8_t *buf, int len);

/* Ein Bulk-Payload (UVC-Payload-Header + Nutzdaten) an den Frame-Puffer anhaengen. Setzt *done=1
 * beim EOF-Bit. Bounds-checked (untrusted Kamera-Daten). Rueckgabe: angehaengte Nutzbytes. */
int uvc_payload_append(const uint8_t *pkt, int pkt_len, uint8_t *frame, int frame_cap,
                       int *frame_pos, int *done);

/* Boot-Selbsttest des Klassen-Layers (synthetische Deskriptoren/Payloads) -> Serial-Marker
 * "[uvc] class-layer: ...". Nur im -Vision-Build gerufen (kmain). */
void uvc_selftest(void);

/* A4.1b HW-Backend (drivers/usb/dwc2.c, #ifdef VISION): ein YUYV-Frame von der enumerierten
 * Bulk-UVC-Kamera nach user_buf greifen (EL0-VA). Bytes / -1 (kein UVC-Geraet enumeriert).
 * vi_cam_grab (kernel/vi_parallel.c) ruft dies auf; EL0 dekodiert mit vi_yuyv_to_rgb. */
int dwc2_uvc_grab(uint64_t user_buf, unsigned long max);

#endif /* RPI_RTOS_UVC_H */
