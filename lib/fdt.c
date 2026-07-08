/*
 * lib/fdt.c  --  Minimaler Flattened-Device-Tree-Leser (nur /memory)
 *
 * Genug FDT (DTB v17), um die /memory-reg-Regionen zu extrahieren -- die einzige
 * verlaessliche Quelle der WAHREN Gesamt-RAM-Groesse auf 4/8-GB-Pi-4.
 * Alle Zugriffe sind gegen die im Header angegebene totalsize begrenzt (Firmware-
 * Eingabe, defensiv behandelt). Big-Endian on-wire -> explizite Byte-Assemblierung.
 */
#include <stdint.h>
#include "fdt.h"

/* FDT-Struktur-Tokens (big-endian u32 im struct-Block). */
#define FDT_BEGIN_NODE 0x1u
#define FDT_END_NODE   0x2u
#define FDT_PROP       0x3u
#define FDT_NOP        0x4u
#define FDT_END        0x9u

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* FDT-Header (alle Felder big-endian u32): 0=magic 8=off_dt_struct 12=off_dt_strings
 * 16=off_mem_rsvmap 24=totalsize? -- Layout: magic,totalsize,off_dt_struct,off_dt_strings,
 * off_mem_rsvmap,version,last_comp_version,... */
struct fdt_header_off { uint32_t v[10]; };

/* Praefix-Vergleich (a beginnt mit want): fuer Node-Namen ("memory" ~ "memory@0").
 * Streng in [.., end) begrenzt. */
static int name_is(const uint8_t *a, const uint8_t *end, const char *want)
{
    while (*want) {
        if (a >= end || (char)*a != *want) { return 0; }
        a++; want++;
    }
    return 1;   /* Praefix-Match reicht */
}

/* EXAKTER Name-Vergleich inkl. Nullterminator, streng in [.., end) begrenzt -- der
 * Terminator-Lesezugriff selbst wird gegen 'end' geprueft (sonst 1-Byte-OOB bei einem
 * manipulierten/abgeschnittenen DTB, dessen Name genau an totalsize endet). */
static int name_eq(const uint8_t *a, const uint8_t *end, const char *want)
{
    while (*want) {
        if (a >= end || (char)*a != *want) { return 0; }
        a++; want++;
    }
    return (a < end && *a == '\0');
}

int fdt_get_memory(uint64_t dtb, fdt_mem_region_t *out, int max)
{
    if (!dtb || !out || max <= 0) {
        return 0;
    }
    const uint8_t *base = (const uint8_t *)(uintptr_t)dtb;
    if (be32(base) != FDT_MAGIC) {
        return 0;                                  /* kein/ungueltiger DTB (z.B. QEMU) */
    }
    uint32_t totalsize   = be32(base + 4);
    uint32_t off_struct  = be32(base + 8);
    uint32_t off_strings = be32(base + 12);
    /* Plausibilitaet: Offsets + eine Mindestgroesse innerhalb totalsize. */
    if (totalsize < 64 || totalsize > (64u * 1024u * 1024u) ||
        off_struct >= totalsize || off_strings >= totalsize) {
        return 0;
    }
    const uint8_t *sp  = base + off_struct;
    const uint8_t *end = base + totalsize;
    const uint8_t *str = base + off_strings;

    /* Root-#address-cells/#size-cells (Pi-4: 2/2 = 64-bit); Default nach FDT-Spec 2/1,
     * wir setzen konservativ 2/2 und ueberschreiben aus dem Root-Node. */
    uint32_t addr_cells = 2, size_cells = 2;
    int depth = 0;
    int in_memory = 0;                             /* im /memory-Node auf Tiefe 1? */
    int count = 0;

    while (sp + 4 <= end) {
        uint32_t tok = be32(sp);
        sp += 4;
        if (tok == FDT_NOP) {
            continue;
        }
        if (tok == FDT_END) {
            break;
        }
        if (tok == FDT_BEGIN_NODE) {
            const uint8_t *nm = sp;
            /* Namen (nullterminiert) ueberspringen -> auf 4 aufrunden. */
            while (sp < end && *sp) { sp++; }
            if (sp >= end) { break; }
            sp++;                                  /* Nullbyte */
            sp = base + ((uint32_t)(sp - base) + 3u & ~3u);
            depth++;
            /* Root ist Tiefe 1; /memory@... ist ein direktes Kind -> Tiefe 2. */
            in_memory = (depth == 2 && name_is(nm, end, "memory"));
            continue;
        }
        if (tok == FDT_END_NODE) {
            if (depth > 0) { depth--; }
            in_memory = 0;
            continue;
        }
        if (tok == FDT_PROP) {
            if (sp + 8 > end) { break; }
            uint32_t len     = be32(sp);
            uint32_t nameoff = be32(sp + 4);
            sp += 8;
            const uint8_t *data = sp;
            if (data + len > end || len > totalsize) { break; }
            const uint8_t *pname = str + nameoff;
            /* Zellgroessen NUR aus dem Root (Tiefe 1) uebernehmen -- sonst wuerden die
             * #*-cells eines frueheren Kindes (z.B. /soc mit 1/1) die fuer /memory
             * gueltigen Root-Werte (2/2) ueberschreiben. */
            if (depth == 1 && len == 4 && name_eq(pname, end, "#address-cells")) {
                addr_cells = be32(data);
            } else if (depth == 1 && len == 4 && name_eq(pname, end, "#size-cells")) {
                size_cells = be32(data);
            } else if (in_memory && name_eq(pname, end, "reg")) {
                /* reg = (addr, size)-Paare, je addr_cells + size_cells u32. */
                if (addr_cells == 0 || addr_cells > 2 || size_cells == 0 || size_cells > 2) {
                    return 0;                       /* unerwartetes Zellformat -> Grobkarte */
                }
                uint32_t pair = (addr_cells + size_cells) * 4u;
                const uint8_t *q = data;
                while (q + pair <= data + len && count < max) {
                    uint64_t a = 0, s = 0;
                    for (uint32_t k = 0; k < addr_cells; k++) { a = (a << 32) | be32(q); q += 4; }
                    for (uint32_t k = 0; k < size_cells; k++) { s = (s << 32) | be32(q); q += 4; }
                    if (s != 0) {
                        out[count].base = a;
                        out[count].size = s;
                        count++;
                    }
                }
            }
            /* Prop-Daten (len) ueberspringen -> auf 4 aufrunden. */
            sp = base + ((uint32_t)(sp - base) + len + 3u & ~3u);
            continue;
        }
        break;                                     /* unbekanntes Token -> abbrechen */
    }
    return count;
}
