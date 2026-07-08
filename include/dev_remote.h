/*
 * include/dev_remote.h  --  Dev-Remote-Interface (UDP), Protokoll-Kern (D1).
 *
 * ENTWICKLER-Schnittstelle: Host-Python fernsteuert den Pi4 (Ausgabe/Bild empfangen, Eingaben
 * senden, Dateien nach hdd0/hdd1 inkl. kernel8.img, Neustart). Per Definition eine Backdoor ->
 * der GESAMTE Code (drivers/net/dev_remote.c, kmain-Aufruf, Syscalls) steht unter #ifdef DEV_REMOTE
 * und ist NIEMALS im RC-/Release-Produktions-Image (grep-clean-bestaetigt). Plan: docs/architecture/20.
 *
 * D1 = die REINEN, in QEMU verifizierbaren Funktionen (Header-Parse, Datei-Reassembly, RLE). Der
 * UDP-Agent + die System-Integration (Konsole/FB/Eingabe/FS/Reboot) sind D2 (realer Pfad am Pi4).
 */
#ifndef RPI_RTOS_DEV_REMOTE_H
#define RPI_RTOS_DEV_REMOTE_H

#include <stdint.h>

#define DEV_MAGIC    0x4D455244u   /* 'D','R','E','M' little-endian */
#define DEV_PORT     5599
#define DEV_HDR_LEN  8

enum {
    DEV_PING = 0x00, DEV_KEY = 0x01, DEV_MOUSE = 0x02,
    DEV_FILE_BEGIN = 0x10, DEV_FILE_CHUNK = 0x11, DEV_FILE_END = 0x12,
    DEV_SCREEN_REQ = 0x20, DEV_RESTART = 0x30,
    DEV_PONG = 0x80, DEV_OUTPUT = 0x81, DEV_SCREEN_DATA = 0x82, DEV_ACK = 0x8F, DEV_RESULT = 0x92
};

typedef struct { uint8_t type, flags; uint16_t seq; } dev_hdr_t;

/* Paket-Kopf parsen (untrusted Netz-Daten -> bounds-checked + Magic geprueft). 0 = ok, -1 = ungueltig. */
int dev_parse_hdr(const uint8_t *pkt, int len, dev_hdr_t *out);
/* Paket-Kopf bauen (buf >= DEV_HDR_LEN). Rueckgabe: geschriebene Bytes / -1. */
int dev_build_hdr(uint8_t *buf, int cap, uint8_t type, uint8_t flags, uint16_t seq);

/* --- Datei-Reassembly: Chunks nach Index in einen Puffer einsortieren (untrusted). --- */
#define DEV_FILE_MAX_CHUNKS 4096
typedef struct {
    uint8_t  *buf;
    uint32_t  cap;
    uint32_t  total_size;
    uint32_t  chunk_size;
    uint32_t  total_chunks;
    uint32_t  recv_count;
    uint8_t   got[DEV_FILE_MAX_CHUNKS / 8];   /* Bitmap empfangener Chunks */
} dev_file_t;

void dev_file_reset(dev_file_t *f, uint8_t *buf, uint32_t cap, uint32_t total_size, uint32_t chunk_size);
/* Chunk 'index' einsortieren (idempotent, bounds-checked). Rueckgabe: 1 = neu, 0 = Duplikat/ungueltig. */
int  dev_file_chunk(dev_file_t *f, uint32_t index, const uint8_t *data, uint32_t len);
int  dev_file_complete(const dev_file_t *f);

/* --- RLE (Lauf-Kompression) fuer 0xAARRGGBB-Framebuffer-Pixel: [u16 count][u32 pixel]* --- */
int dev_rle_encode(const uint32_t *px, int npx, uint8_t *out, int out_cap);    /* -> Byte-Laenge / -1 */
int dev_rle_decode(const uint8_t *in, int in_len, uint32_t *px, int npx_cap);  /* -> Pixel-Zahl / -1 */

/* Boot-Selbsttest des Protokoll-Kerns (synthetisch) -> Serial-Marker "[dev] proto-core: ...". */
void dev_remote_selftest(void);

#endif /* RPI_RTOS_DEV_REMOTE_H */
