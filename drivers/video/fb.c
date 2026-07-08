/*
 * drivers/video/fb.c  --  VideoCore-Framebuffer (32 bpp) via Mailbox-Property
 *
 * Setzt physische+virtuelle Aufloesung, Tiefe (32) und Pixel-Reihenfolge, fordert
 * den Puffer an und liest Basis/Pitch. Die zurueckgegebene Basis ist eine
 * BUS-Adresse -> in eine physische (CPU-)Adresse umrechnen (untere 30 Bit).
 */
#include <stdint.h>
#include "mmio.h"
#include "mailbox.h"
#include "fb.h"

#define CACHE_LINE 64

static fb_t s_fb;
/* Cache-Line-aligned (64 B): Clean/Invalidate im Mailbox-Pfad treffen so nur
 * Puffer-eigene Cache-Lines, keine Nachbardaten. */
static volatile uint32_t mbuf[48] __attribute__((aligned(64)));

int fb_init(uint32_t w, uint32_t h)
{
    int i = 0;
    mbuf[i++] = 0;                 /* Gesamtgroesse (unten gesetzt) */
    mbuf[i++] = 0;                 /* Request */

    int physwh_i = i;
    mbuf[i++] = 0x00048003; mbuf[i++] = 8; mbuf[i++] = 0; mbuf[i++] = w; mbuf[i++] = h;  /* set phys WH */
    mbuf[i++] = 0x00048004; mbuf[i++] = 8; mbuf[i++] = 0; mbuf[i++] = w; mbuf[i++] = h;  /* set virt WH */
    mbuf[i++] = 0x00048005; mbuf[i++] = 4; mbuf[i++] = 0; mbuf[i++] = 32;                /* set depth */
    /* Pixel-Reihenfolge BGR (0), NICHT RGB (1): unsere Pixel liegen als uint32 0x00RRGGBB little-
     * endian im Speicher -> Bytes [BB,GG,RR,00]. Das Panel muss Byte0 als Blau lesen (BGR), sonst
     * erscheinen Rot und Blau vertauscht. */
    mbuf[i++] = 0x00048006; mbuf[i++] = 4; mbuf[i++] = 0; mbuf[i++] = 0;                 /* pixel order BGR */

    int alloc_i = i;
    mbuf[i++] = 0x00040001; mbuf[i++] = 8; mbuf[i++] = 0; mbuf[i++] = 4096; mbuf[i++] = 0; /* allocate (align 4096) -> base,size */

    int pitch_i = i;
    mbuf[i++] = 0x00040008; mbuf[i++] = 4; mbuf[i++] = 0; mbuf[i++] = 0;                 /* get pitch */

    mbuf[i++] = 0;                 /* End-Tag */
    mbuf[0] = (uint32_t)(i * 4);

    if (mailbox_property(mbuf) != 0) {
        return -1;
    }

    uint32_t base_bus = mbuf[alloc_i + 3];
    uint32_t fbsize   = mbuf[alloc_i + 4];
    uint32_t pitch    = mbuf[pitch_i + 3];
    if (base_bus == 0 || fbsize == 0) {
        return -1;
    }

    /* TATSAECHLICH konfigurierte Aufloesung aus der GPU-Antwort lesen: die Firmware
     * darf eine nicht unterstuetzte Aufloesung auf einen kleineren Modus klemmen und
     * allokiert dann nur fuer DIESE Groesse. Wuerden wir die ANGEFORDERTE WH behalten,
     * adressierten fb_pixel/fb_clear ueber das allokierte 'size' hinaus (OOB). */
    uint32_t aw = mbuf[physwh_i + 3];
    uint32_t ah = mbuf[physwh_i + 4];
    if (aw == 0 || ah == 0) { aw = w; ah = h; }
    if (pitch == 0) {
        pitch = aw * 4;
    }
    /* Bounds strikt an die echte Allokation klemmen: eine Zeile muss in den Pitch
     * passen und die Hoehe darf 'size' nie ueberschreiten. */
    if (aw * 4u > pitch) {
        aw = pitch / 4u;
    }
    uint32_t max_rows = pitch ? (fbsize / pitch) : 0;
    if (ah > max_rows) {
        ah = max_rows;
    }
    if (aw == 0 || ah == 0) {
        return -1;
    }

    uint64_t base_phys = (uint64_t)(base_bus & 0x3FFFFFFFu);   /* bus -> phys (unterer 1-GiB-Alias) */

    /* Der FB bleibt Normal-Write-Back-cacheable (schnelles Rendern + cached memmove
     * beim Scrollen). Die VideoCore/HVS liest ihn per eigenem DMA aus dem SDRAM und ist
     * NICHT kohaerent mit der ARM-Daten-Cache -> nach dem Schreiben muessen die Zeilen
     * per fb_flush_rows()/fb_flush() (dc cvac, PoC) in den RAM getrieben werden, damit
     * die GPU sie sieht. (NC-Mapping waere auch korrekt, in QEMU aber sehr langsam, weil
     * non-cacheable Speicher den softmmu-slow-path nimmt; dc cvac ist in QEMU ein No-op.) */

    s_fb.base   = (volatile uint8_t *)(uintptr_t)base_phys;
    s_fb.size   = fbsize;
    s_fb.pitch  = pitch;
    s_fb.width  = aw;
    s_fb.height = ah;
    return 0;
}

const fb_t *fb_get(void) { return &s_fb; }

/* Daten-Cache fuer die Zeilen [y, y+nrows) zum PoC bereinigen (dc cvac), damit die
 * GPU die frisch gerenderten Pixel im SDRAM sieht. In QEMU ein No-op (kohaerent). */
void fb_flush_rows(uint32_t y, uint32_t nrows)
{
    if (!s_fb.base || y >= s_fb.height) {
        return;
    }
    if (y + nrows > s_fb.height) {
        nrows = s_fb.height - y;
    }
    uintptr_t a   = (uintptr_t)s_fb.base + (uint64_t)y * s_fb.pitch;
    uintptr_t end = a + (uint64_t)nrows * s_fb.pitch;
    a &= ~(uintptr_t)(CACHE_LINE - 1);
    for (; a < end; a += CACHE_LINE) {
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

void fb_flush(void)
{
    fb_flush_rows(0, s_fb.height);
}

void fb_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (!s_fb.base || x >= s_fb.width || y >= s_fb.height) {
        return;
    }
    volatile uint32_t *p = (volatile uint32_t *)(s_fb.base + (uint64_t)y * s_fb.pitch + (uint64_t)x * 4);
    *p = rgb;
}

void fb_clear(uint32_t rgb)
{
    if (!s_fb.base) {
        return;
    }
    for (uint32_t y = 0; y < s_fb.height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(s_fb.base + (uint64_t)y * s_fb.pitch);
        for (uint32_t x = 0; x < s_fb.width; x++) {
            row[x] = rgb;
        }
    }
    fb_flush();                 /* gesamten Schirm zur GPU treiben */
}
