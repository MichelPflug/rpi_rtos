/*
 * fs/fat32.c  --  Minimaler, lesender FAT32-Treiber (mehrfach mountbar)
 *
 * Liest Bloecke ueber die blockdev-Abstraktion. Keine Unterverzeichnisse, keine
 * LFN-Aufloesung (8.3-Name wird genutzt), read-only. Der FAT-Sektor-Cache liegt
 * pro fat_fs_t; der Verzeichnis-/Daten-Scratch (secbuf) ist global -> nicht
 * reentrant, in M1 single-threaded genutzt.
 */
#include <stdint.h>
#include "uart.h"
#include "fat32.h"
#include "aarch64.h"   /* CNTPCT/CNTFRQ fuer Zeitstempel (kein RTC am Pi 4) */

/* Cache-Line-aligned: dienen als DMA-Ziel/-Quelle (USB-Pfad cleant/invalidiert ganze
 * Cache-Lines; ein nur 16-B-aligned Puffer wuerde Nachbardaten treffen). */
static uint8_t secbuf[512]  __attribute__((aligned(64)));   /* globaler Scratch (Verzeichnis/Datei) */
static uint8_t wsec[512]    __attribute__((aligned(64)));   /* Daten-Sektor-Assembly beim Schreiben */
static uint8_t fatwsec[512] __attribute__((aligned(64)));   /* FAT-Sektor Read-Modify-Write */
static uint8_t fsisec[512]  __attribute__((aligned(64)));   /* FSInfo-Sektor (frei von secbuf/fatwsec) */

#ifdef RTOS_SELFTEST
/* Guardian-Instrumentierung: zaehlt, wie viele Kerne GLEICHZEITIG im geteilten secbuf-
 * Verzeichnis-Scan (dir_iterate) sind. Da JEDER korrekte Pfad secbuf nur unter dem VFS-`fs_lock`
 * anfasst, MUSS das immer 1 sein. Ein Pfad, der `fs_lock` umgeht (z.B. ein unverriegeltes
 * vfs_list -> das T1.10 behebt), fuehrt zu occ>1 -> Verletzungs-Latch. Das Fenster wird nur
 * waehrend des Nebenlaeufigkeits-Tests kuenstlich geweitet (fat32_secbuf_widen), damit die
 * vielen dir_iterate-Aufrufe im Normalboot nicht ausgebremst werden. Nur im Selbsttest. */
volatile int          fat32_secbuf_widen;   /* Test setzt =1 waehrend des Laufs */
static volatile uint32_t s_secbuf_occ;       /* aktuell im secbuf-Scan befindliche Kerne */
static volatile uint32_t s_secbuf_occ_max;   /* je beobachtetes Maximum */
static volatile uint32_t s_secbuf_occ_viol;  /* Latch: je >1 beobachtet? (0=nie) */

static void secbuf_occ_sample(uint32_t o)
{
    if (o > s_secbuf_occ_max) { s_secbuf_occ_max = o; }
    if (o > 1u)               { s_secbuf_occ_viol = 1u; }
}
static void secbuf_occ_enter(void)
{
    secbuf_occ_sample(++s_secbuf_occ);
    if (fat32_secbuf_widen) {
        for (volatile int w = 0; w < 4000; w++) {
            secbuf_occ_sample(s_secbuf_occ);
        }
    }
}
static void secbuf_occ_leave(void)
{
    --s_secbuf_occ;
}
uint32_t fat32_secbuf_occ_max(void)       { return s_secbuf_occ_max; }
uint32_t fat32_secbuf_occ_violation(void) { return s_secbuf_occ_viol; }
#endif /* RTOS_SELFTEST */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Kein RTC am Pi 4: FAT-Zeitstempel = feste Basis-Epoche + Uptime (CNTPCT/CNTFRQ) -> MONOTONE,
 * unterscheidbare Zeiten ab einem festen Datum (statt der stets-1980 Null-Zeitstempel). */
#define FAT_BASE_SECS 1466942400u   /* 2026-06-26 12:00:00 in Sekunden seit 1980-01-01 (FAT-Epoche) */

static int fat_is_leap(int y) { return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0); }

/* Aktuelle Zeit als FAT-Datum (Jahr-1980<<9 | Monat<<5 | Tag) + FAT-Zeit (Std<<11 | Min<<5 | Sek/2). */
static void fat_now(uint16_t *fdate, uint16_t *ftime)
{
    uint64_t frq  = READ_SYSREG(cntfrq_el0);
    uint64_t up_s = frq ? (READ_SYSREG(cntpct_el0) / frq) : 0;
    uint32_t secs = FAT_BASE_SECS + (uint32_t)up_s;
    uint32_t days = secs / 86400u;
    uint32_t rem  = secs % 86400u;
    int hh = (int)(rem / 3600u), mi = (int)((rem % 3600u) / 60u), ss = (int)(rem % 60u);
    int year = 1980;
    for (;;) {
        uint32_t diy = fat_is_leap(year) ? 366u : 365u;
        if (days < diy) { break; }
        days -= diy;
        year++;
    }
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int month = 12;
    for (int m = 0; m < 12; m++) {
        uint32_t dim = (uint32_t)mdays[m] + ((m == 1 && fat_is_leap(year)) ? 1u : 0u);
        if (days < dim) { month = m + 1; break; }
        days -= dim;
    }
    int day = (int)days + 1;
    *fdate = (uint16_t)((((year - 1980) & 0x7F) << 9) | ((month & 0x0F) << 5) | (day & 0x1F));
    *ftime = (uint16_t)(((hh & 0x1F) << 11) | ((mi & 0x3F) << 5) | ((ss / 2) & 0x1F));
}

/* Formatiert FAT-Datum+Zeit als "YYYY-MM-DD HH:MM" (16 Zeichen + Null) in out (>= 17 Byte). */
static void fmt_datetime(uint16_t d, uint16_t t, char *out)
{
    int year = 1980 + ((d >> 9) & 0x7F), mon = (d >> 5) & 0x0F, day = d & 0x1F;
    int hh = (t >> 11) & 0x1F, mi = (t >> 5) & 0x3F;
    int o = 0;
    out[o++] = (char)('0' + (year / 1000) % 10); out[o++] = (char)('0' + (year / 100) % 10);
    out[o++] = (char)('0' + (year / 10) % 10);   out[o++] = (char)('0' + year % 10);
    out[o++] = '-'; out[o++] = (char)('0' + mon / 10); out[o++] = (char)('0' + mon % 10);
    out[o++] = '-'; out[o++] = (char)('0' + day / 10); out[o++] = (char)('0' + day % 10);
    out[o++] = ' '; out[o++] = (char)('0' + hh / 10);  out[o++] = (char)('0' + hh % 10);
    out[o++] = ':'; out[o++] = (char)('0' + mi / 10);  out[o++] = (char)('0' + mi % 10);
    out[o] = '\0';
}

static int rd_block(fat_fs_t *fs, uint64_t lba, void *buf)
{
    return fs->bdev->read_block(fs->bdev, lba, buf);
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* "HELLO.TXT" -> 11 Byte "HELLO   TXT" (Space-gepaddet, Grossbuchstaben). */
static void parse_name(const char *name, uint8_t *raw)
{
    for (int i = 0; i < 11; i++) {
        raw[i] = ' ';
    }
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8) {
        raw[o++] = (uint8_t)up(name[i++]);
    }
    while (name[i] && name[i] != '.') {
        i++;
    }
    if (name[i] == '.') {
        i++;
        int e = 8;
        while (name[i] && e < 11) {
            raw[e++] = (uint8_t)up(name[i++]);
        }
    }
}

static void fmt_name(const uint8_t *raw, char *out)
{
    int o = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        out[o++] = (char)raw[i];
    }
    if (raw[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            out[o++] = (char)raw[i];
        }
    }
    out[o] = '\0';
}

static uint64_t cluster_lba(fat_fs_t *fs, uint32_t clus)
{
    return fs->data_lba + (uint64_t)(clus - 2) * fs->sec_per_clus;
}

static uint32_t fat_next(fat_fs_t *fs, uint32_t clus)
{
    uint64_t lba = fs->fat_lba + (clus * 4) / 512;
    uint32_t off = (clus * 4) % 512;
    if (lba != fs->fatbuf_lba) {
        if (rd_block(fs, lba, fs->fatbuf)) {
            fs->io_err = 1;            /* Fehler markieren (nicht als EOC verschlucken) */
            return 0x0FFFFFFF;
        }
        fs->fatbuf_lba = lba;
    }
    return rd32(&fs->fatbuf[off]) & 0x0FFFFFFF;
}

/* Gueltig = im Datenbereich der Partition. max_cluster begrenzt die Traversierung
 * auf die tatsaechliche Groesse -> eine boesartige FAT kann nicht ueber die Partition
 * hinaus auf beliebige Geraetesektoren zeigen. */
static int cluster_valid(fat_fs_t *fs, uint32_t c)
{
    return (c >= 2) && (c < fs->max_cluster);
}

/* Ein Schritt entlang einer Verzeichnis-Cluster-Kette MIT Floyd-Zykluserkennung
 * (Hase/Igel). *clus (Igel) wandert einen Schritt, *hare (Hase) zwei; treffen sie
 * sich, enthaelt die Kette einen Zyklus -> -1. So wird eine zyklische/korrupte FAT
 * (z.B. auf untrusted hdd2) nach wenigen Schritten erkannt statt endlos durchlaufen
 * -- wichtig, weil Datei-Syscalls mit maskierten IRQs laufen (Endlosschleife = Hang)
 * und jeder Cluster-Read auf USB eine teure BOT/SCSI-Transaktion ist. Eine reine
 * Iterationsschranke (== Cluster-Anzahl) terminiert zwar, waere auf USB aber Minuten.
 * Sobald der Hase das Kettenende erreicht (kein Zyklus moeglich), wird er stillgelegt. */
static int dir_chain_step(fat_fs_t *fs, uint32_t *clus, uint32_t *hare, int *hare_live)
{
    *clus = fat_next(fs, *clus);
    if (*hare_live) {
        uint32_t h = fat_next(fs, *hare);
        if (cluster_valid(fs, h)) {
            h = fat_next(fs, h);
        }
        if (!cluster_valid(fs, h)) {
            *hare_live = 0;                 /* Hase am Kettenende -> kein Zyklus */
        } else {
            *hare = h;
            if (*hare == *clus) {
                return -1;                  /* Igel vom Hasen eingeholt -> Zyklus */
            }
        }
    }
    return 0;
}

int fat32_mount(fat_fs_t *fs, blockdev_t *bdev, uint64_t part_lba)
{
    fs->bdev       = bdev;
    fs->mounted    = 0;
    fs->io_err     = 0;
    fs->fatbuf_lba = (uint64_t)-1;

    if (rd_block(fs, part_lba, secbuf)) {
        return -1;
    }
    uint16_t byts_per_sec = rd16(&secbuf[11]);
    uint8_t  sec_per_clus = secbuf[13];
    uint16_t rsvd         = rd16(&secbuf[14]);
    uint8_t  num_fats     = secbuf[16];
    uint32_t fatsz        = rd32(&secbuf[36]);
    uint32_t root_clus    = rd32(&secbuf[44]);
    uint16_t fsinfo_sec   = rd16(&secbuf[48]);

    if (byts_per_sec != 512 || sec_per_clus == 0 || num_fats == 0 ||
        fatsz == 0 || root_clus < 2) {
        return -1;
    }

    fs->fat_lba      = part_lba + rsvd;
    fs->data_lba     = part_lba + rsvd + (uint64_t)num_fats * fatsz;
    fs->root_cluster = root_clus;
    fs->sec_per_clus = sec_per_clus;
    fs->num_fats     = num_fats;
    fs->fat_size     = fatsz;

    /* Geometrie 64-bittig rechnen: eine boesartige/defekte BPB (grosse fatsz/num_fats)
     * darf 'used' nicht ueberlaufen lassen, sonst passierte die Konsistenzpruefung
     * faelschlich und max_cluster wuerde absurd gross. */
    uint32_t tot_sec = rd32(&secbuf[32]);
    uint64_t used    = (uint64_t)rsvd + (uint64_t)num_fats * fatsz;
    if ((uint64_t)tot_sec <= used) {
        return -1;                              /* inkonsistente BPB */
    }
    uint64_t maxc    = (((uint64_t)tot_sec - used) / sec_per_clus) + 2;
    uint64_t fat_cap = (uint64_t)fatsz * 512 / 4;   /* FAT-Eintraege */
    if (maxc > fat_cap) {
        maxc = fat_cap;                         /* nie ueber die FAT-Kapazitaet */
    }
    if (maxc > 0x0FFFFFF7u) {
        maxc = 0x0FFFFFF7u;                     /* FAT32-Cluster-Obergrenze */
    }
    fs->max_cluster = (uint32_t)maxc;

    /* FSInfo (BPB[48] = Sektornummer relativ zum Partitionsstart): Signaturen pruefen, den
     * next-free-Hint uebernehmen und den freien-Cluster-Zaehler AUS DER FAT berechnen (der
     * FSInfo-Wert kann veraltet/unbekannt sein). Fortan bei alloc/free gepflegt. */
    fs->fsinfo_lba = 0;
    fs->free_count = 0xFFFFFFFFu;
    fs->next_free  = 2;
    uint32_t fsi_free = 0xFFFFFFFFu;
    if (fsinfo_sec != 0 && fsinfo_sec != 0xFFFF) {
        uint64_t filba = part_lba + fsinfo_sec;
        if (rd_block(fs, filba, fsisec) == 0 &&
            rd32(&fsisec[0]) == 0x41615252u && rd32(&fsisec[484]) == 0x61417272u) {
            fs->fsinfo_lba = filba;
            uint32_t nf = rd32(&fsisec[492]);
            if (nf >= 2 && nf < fs->max_cluster) { fs->next_free = nf; }
            fsi_free = rd32(&fsisec[488]);
        }
    }
    /* free_count aus FSInfo uebernehmen, WENN plausibel (<= Cluster-Anzahl) -- der FS wird nur
     * von diesem Treiber geschrieben, der FSInfo bei alloc/free pflegt. Nur wenn unbekannt/
     * unplausibel die FAT EINMAL scannen (teuer bei grossen Partitionen). */
    if (fsi_free != 0xFFFFFFFFu && fsi_free <= (fs->max_cluster - 2)) {
        fs->free_count = fsi_free;
    } else {
        uint32_t freec = 0;
        for (uint32_t c = 2; c < fs->max_cluster; c++) {
            if (fat_next(fs, c) == 0) { freec++; }
            if (fs->io_err) { fs->io_err = 0; freec = 0xFFFFFFFFu; break; }
        }
        fs->free_count = freec;
    }

    fs->mounted      = 1;
    return 0;
}

/* LFN-Pruefsumme des 8.3-Namens (Standard-FAT-Rotationssumme). */
static uint8_t lfn_checksum(const uint8_t *name11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
    }
    return sum;
}

/* Callback je 8.3-Eintrag: raw (8.3), longname ("" wenn kein gueltiger LFN),
 * attr, Start-Cluster, Groesse, physischer Ort (lba+off). cb!=0 bricht ab. */
typedef int (*dir_cb_t)(const uint8_t *raw, const char *longname, uint8_t attr,
                        uint32_t clus, uint32_t size, uint64_t lba, uint32_t off,
                        void *ctx);

/* Iteriert die Verzeichniseintraege ab dir_cluster (Root oder Unterverzeichnis),
 * reassembliert vorangehende LFN-Eintraege (attr 0x0F) zu einem langen Namen und
 * reicht ihn (per Pruefsumme gegen den 8.3-Eintrag validiert) an cb. Floyd-geschuetzt. */
static int dir_iterate_inner(fat_fs_t *fs, uint32_t dir_cluster, dir_cb_t cb, void *ctx)
{
    /* Offsets der 13 UTF-16-Zeichen eines LFN-Eintrags. */
    static const int LSLOT[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    uint32_t clus = dir_cluster, hare = dir_cluster;
    int      hare_live = 1;
    char     lfn[261];
    int      lfn_have = 0, lfn_n = 0, lfn_expect = 0, lfn_term = -1;
    uint8_t  lfn_sum = 0;

    while (cluster_valid(fs, clus)) {
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            uint64_t lba = cluster_lba(fs, clus) + s;
            if (rd_block(fs, lba, secbuf)) {
                return -1;
            }
            for (int e = 0; e < 512; e += 32) {
                uint8_t *de = &secbuf[e];
                if (de[0] == 0x00) {
                    return 0;
                }
                if (de[0] == 0xE5) {
                    lfn_have = 0;
                    continue;
                }
                uint8_t attr = de[11];
                if ((attr & 0x0F) == 0x0F) {            /* LFN-Eintrag akkumulieren */
                    int ord = de[0] & 0x1F;
                    if (de[0] & 0x40) {                 /* Start: letzter logisch = erster physisch */
                        if (ord < 1 || ord > 20) { lfn_have = 0; continue; }
                        /* Puffer nullen: gegen Stack-Leak an EL0 UND gegen veraltete
                         * Bytes bei luecken-/fehlerhaften Sequenzen (untrusted hdd2). */
                        for (int i = 0; i < 261; i++) lfn[i] = 0;
                        lfn_have = 1; lfn_n = ord; lfn_expect = ord;
                        lfn_term = -1; lfn_sum = de[13];
                    }
                    /* Strikte Ordinal-Integritaet: muss mit 0x40 starten und in
                     * absteigender, lueckenloser Folge (n, n-1, .. 1) bei gleicher
                     * Pruefsumme kommen -- sonst verwerfen (8.3-Fallback). */
                    if (!lfn_have || ord != lfn_expect || de[13] != lfn_sum) {
                        lfn_have = 0;
                        continue;
                    }
                    lfn_expect--;
                    int base = (ord - 1) * 13;
                    for (int j = 0; j < 13; j++) {
                        uint16_t ch = (uint16_t)(de[LSLOT[j]] | (de[LSLOT[j] + 1] << 8));
                        int pos = base + j;
                        if (ch == 0x0000 || ch == 0xFFFF) {
                            if (lfn_term < 0 || pos < lfn_term) lfn_term = pos;
                        } else if (pos < 260) {
                            lfn[pos] = (ch < 0x80) ? (char)ch : '?';
                        }
                    }
                    continue;
                }
                if (attr & 0x08) {                      /* Volume-Label */
                    lfn_have = 0;
                    continue;
                }
                /* 8.3-Eintrag: Langnamen finalisieren -- nur wenn die LFN-Sequenz
                 * vollstaendig war (alle Ordinale n..1) UND die Pruefsumme zum 8.3
                 * passt. Sonst 8.3-Fallback (longname bleibt ""). */
                const char *longname = "";
                if (lfn_have && lfn_expect == 0) {
                    int term = (lfn_term >= 0) ? lfn_term : lfn_n * 13;
                    if (lfn_sum == lfn_checksum(de) && term <= 260) {
                        lfn[term] = '\0';
                        longname = lfn;
                    }
                }
                lfn_have = 0;
                uint32_t fc = ((uint32_t)rd16(&de[20]) << 16) | rd16(&de[26]);
                uint32_t sz = rd32(&de[28]);
                if (cb(de, longname, attr, fc, sz, lba, (uint32_t)e, ctx)) {
                    return 0;
                }
            }
        }
        if (dir_chain_step(fs, &clus, &hare, &hare_live)) {  /* Zyklus -> Abbruch */
            fs->io_err = 1;
            return -1;
        }
    }
    return 0;
}

/* Alle Verzeichnis-Scans laufen ueber diesen einen Choke-Point (secbuf-Nutzer). Im Selbsttest
 * misst er die gleichzeitige Belegung des geteilten secbuf; produktiv reine Delegation. */
static int dir_iterate(fat_fs_t *fs, uint32_t dir_cluster, dir_cb_t cb, void *ctx)
{
#ifdef RTOS_SELFTEST
    secbuf_occ_enter();
    int r = dir_iterate_inner(fs, dir_cluster, cb, ctx);
    secbuf_occ_leave();
    return r;
#else
    return dir_iterate_inner(fs, dir_cluster, cb, ctx);
#endif
}

/* ===================== Pfad-Aufloesung (Unterverzeichnisse) ===================== */

/* Kopiert eine Pfadkomponente (start..start+len) als nullterminierten String. */
static void comp_copy(const char *start, int len, char *out, int max)
{
    int n = (len < max - 1) ? len : max - 1;
    for (int i = 0; i < n; i++) {
        out[i] = start[i];
    }
    out[n] = '\0';
}

/* Fundort eines Verzeichniseintrags (8.3) inkl. physischer Position fuer Updates. */
struct dirent_loc {
    uint64_t lba;       /* Sektor des Eintrags */
    uint32_t off;       /* Offset im Sektor */
    uint32_t clus;      /* Start-Cluster (0 = leer / Root-Ref bei "..") */
    uint32_t size;
    uint8_t  attr;
    int      found;
};

/* Case-insensitiver Stringvergleich (FAT-Namen sind case-insensitiv). */
static int name_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (up(*a) != up(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

struct lookup_ctx { const char *want; struct dirent_loc *out; };

static int lookup_cb(const uint8_t *raw, const char *longname, uint8_t attr,
                     uint32_t clus, uint32_t size, uint64_t lba, uint32_t off, void *ctx)
{
    struct lookup_ctx *L = (struct lookup_ctx *)ctx;
    char short83[16];
    fmt_name(raw, short83);
    if (((longname && longname[0]) && name_ieq(L->want, longname)) ||
        name_ieq(L->want, short83)) {
        L->out->lba = lba; L->out->off = off; L->out->clus = clus;
        L->out->size = size; L->out->attr = attr; L->out->found = 1;
        return 1;                            /* Treffer -> Iteration beenden */
    }
    return 0;
}

/* Sucht Datei/Verzeichnis per Name (langer ODER 8.3-Name, case-insensitiv) ab
 * dir_cluster. 0 = ok (out->found gesetzt/ungesetzt), -1 = I/O-Fehler/Zyklus. */
static int dir_lookup(fat_fs_t *fs, uint32_t dir_cluster, const char *name,
                      struct dirent_loc *out)
{
    out->found = 0;
    struct lookup_ctx L = { name, out };
    if (dir_iterate(fs, dir_cluster, lookup_cb, &L) != 0 || fs->io_err) {
        return -1;
    }
    return 0;
}

/* Laeuft path von der Wurzel durch alle Komponenten AUSSER der letzten (jede muss
 * ein Unterverzeichnis sein); liefert das Eltern-Cluster + die letzte Komponente als
 * String (langer ODER 8.3-Name). 0 = ok, -1 = Zwischenkomp. fehlt/kein Verz./leer. */
static int walk_to_parent(fat_fs_t *fs, const char *path,
                          uint32_t *parent_clus, char *leaf_out, int leaf_max)
{
    uint32_t dir = fs->root_cluster;
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    for (;;) {
        const char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        int len = (int)(p - start);
        const char *q = p;
        while (*q == '/') {
            q++;
        }
        if (*q == '\0') {                     /* letzte Komponente = Leaf */
            if (len == 0) {
                return -1;                    /* "dir/" ohne Dateinamen */
            }
            comp_copy(start, len, leaf_out, leaf_max);
            *parent_clus = dir;
            return 0;
        }
        if (len == 0) {                       /* doppelter '/' -> ueberspringen */
            p = q;
            continue;
        }
        char comp[64];
        comp_copy(start, len, comp, sizeof(comp));
        struct dirent_loc loc;
        if (dir_lookup(fs, dir, comp, &loc) != 0 || !loc.found || !(loc.attr & 0x10)) {
            return -1;                        /* fehlt oder kein Verzeichnis */
        }
        dir = loc.clus ? loc.clus : fs->root_cluster;   /* ".." auf Root: clus=0 */
        p = q;
    }
}

/* Loest path KOMPLETT zu einem Verzeichnis-Cluster auf (alle Komponenten = Verz.).
 * Leerer Pfad -> Root. 0 = ok, -1 = Komponente fehlt/kein Verzeichnis. */
static int resolve_dir(fat_fs_t *fs, const char *path, uint32_t *out)
{
    uint32_t dir = fs->root_cluster;
    const char *p = path;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *s = p;
        while (*p && *p != '/') {
            p++;
        }
        char comp[64];
        comp_copy(s, (int)(p - s), comp, sizeof(comp));
        struct dirent_loc loc;
        if (dir_lookup(fs, dir, comp, &loc) != 0 || !loc.found || !(loc.attr & 0x10)) {
            return -1;
        }
        dir = loc.clus ? loc.clus : fs->root_cluster;
    }
    *out = dir;
    return 0;
}

static int list_cb(const uint8_t *raw, const char *longname, uint8_t attr,
                   uint32_t clus, uint32_t size, uint64_t lba, uint32_t off, void *ctx)
{
    (void)clus; (void)lba; (void)off; (void)ctx;
    char name[16];
    fmt_name(raw, name);
    uart_puts("      - ");
    uart_puts((longname && longname[0]) ? longname : name);
    char dt[20];
    fmt_datetime(rd16(&raw[24]), rd16(&raw[22]), dt);   /* letztes Schreibdatum+zeit */
    if (attr & 0x10) {
        uart_puts("  <DIR>  ");
        uart_puts(dt);
        uart_puts("\n");
        return 0;
    }
    uart_puts("  (");
    uart_putdec(size);
    uart_puts(" Byte)  ");
    uart_puts(dt);
    uart_puts("\n");
    return 0;
}

void fat32_list(fat_fs_t *fs)
{
    if (!fs->mounted) {
        uart_puts("      (nicht gemountet)\n");
        return;
    }
    dir_iterate(fs, fs->root_cluster, list_cb, 0);
}

struct lbuf_ctx { char *buf; uint32_t max; uint32_t len; };

static void lbuf_puts(struct lbuf_ctx *c, const char *s)
{
    while (*s && c->len + 1 < c->max) {
        c->buf[c->len++] = *s++;
    }
}

static int listbuf_cb(const uint8_t *raw, const char *longname, uint8_t attr,
                      uint32_t clus, uint32_t size, uint64_t lba, uint32_t off, void *ctx)
{
    (void)clus; (void)lba; (void)off;
    if (raw[0] == '.') {
        return 0;                                   /* "." / ".." nicht listen */
    }
    struct lbuf_ctx *c = (struct lbuf_ctx *)ctx;
    char name[16];
    fmt_name(raw, name);
    lbuf_puts(c, (longname && longname[0]) ? longname : name);
    char dt[20];
    fmt_datetime(rd16(&raw[24]), rd16(&raw[22]), dt);   /* letztes Schreibdatum(24)+zeit(22) */
    if (attr & 0x10) {                              /* Unterverzeichnis */
        lbuf_puts(c, "  <DIR>  ");
        lbuf_puts(c, dt);
        lbuf_puts(c, "\n");
        return 0;
    }
    char num[14];                                   /* "  " + Dezimalgroesse */
    int n = 0;
    num[n++] = ' '; num[n++] = ' ';
    if (size == 0) {
        num[n++] = '0';
    } else {
        char t[10]; int ti = 0;
        while (size && ti < 10) { t[ti++] = (char)('0' + size % 10); size /= 10; }
        while (ti > 0) { num[n++] = t[--ti]; }
    }
    num[n] = '\0';
    lbuf_puts(c, num);
    lbuf_puts(c, "  ");
    lbuf_puts(c, dt);
    lbuf_puts(c, "\n");
    return 0;
}

int fat32_listdir(fat_fs_t *fs, const char *path, char *buf, uint32_t max)
{
    if (!fs->mounted || max == 0) {
        return -1;
    }
    fs->io_err = 0;
    uint32_t dir;
    if (resolve_dir(fs, path ? path : "", &dir) != 0) {
        return -1;                                  /* Verzeichnis nicht gefunden */
    }
    struct lbuf_ctx c = { buf, max, 0 };
    if (dir_iterate(fs, dir, listbuf_cb, &c) != 0 || fs->io_err) {
        return -1;
    }
    buf[c.len] = '\0';
    return (int)c.len;
}

int fat32_read_file(fat_fs_t *fs, const char *name, void *buf, uint32_t max_len)
{
    if (!fs->mounted) {
        return -1;
    }
    fs->io_err = 0;
    char leaf[64];
    uint32_t parent;
    if (walk_to_parent(fs, name, &parent, leaf, sizeof(leaf)) != 0) {
        return -1;                   /* Pfad/Verzeichnis nicht aufloesbar */
    }
    struct dirent_loc loc;
    if (dir_lookup(fs, parent, leaf, &loc) != 0 || fs->io_err) {
        return -1;                   /* Lesefehler waehrend der Verzeichnissuche */
    }
    if (!loc.found) {
        return -2;                   /* GENUIN nicht vorhanden (== VFS_ENOENT). Bewusst != -1:
                                      * -1 signalisiert einen Lese-/Korruptionsfehler bei einer
                                      * vorhandenen Datei. Der Aufrufer (z.B. user_init) muss
                                      * "Erstboot" (Datei fehlt) von "DB korrupt" unterscheiden. */
    }
    if (loc.attr & 0x10) {
        return -1;                   /* Name existiert, ist aber ein Verzeichnis -> Fehler */
    }

    uint32_t to_read = loc.size < max_len ? loc.size : max_len;
    uint32_t copied  = 0;
    uint32_t clus    = loc.clus;
    uint8_t *out     = (uint8_t *)buf;

    while (cluster_valid(fs, clus) && copied < to_read) {
        for (uint32_t s = 0; s < fs->sec_per_clus && copied < to_read; s++) {
            if (rd_block(fs, cluster_lba(fs, clus) + s, secbuf)) {
                return -1;
            }
            uint32_t n = 512;
            if (n > to_read - copied) {
                n = to_read - copied;
            }
            for (uint32_t i = 0; i < n; i++) {
                out[copied + i] = secbuf[i];
            }
            copied += n;
        }
        clus = fat_next(fs, clus);
    }
    if (fs->io_err) {
        return -1;                   /* Lesefehler waehrend des Datei-Lesens */
    }
    if (copied < to_read) {
        return -1;                   /* Cluster-Kette endete VOR der deklarierten Groesse -> korrupt */
    }
    /* Rueckgabe = WAHRE Dateigroesse, NICHT nur die kopierten Bytes. In den Puffer wurden
     * min(size, max_len) Bytes gelegt; ist die Datei groesser als max_len, erkennt der Aufrufer
     * die Trunkierung an return > max_len (statt still eine gekuerzte Datei zu bekommen).
     * Fuer Dateien <= max_len ist return == kopierte Bytes wie zuvor (abwaertskompatibel). */
    return (loc.size > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int)loc.size;
}

/* ===================== Schreiben (read/write FAT32) ===================== */

static int wr_block(fat_fs_t *fs, uint64_t lba, const void *buf)
{
    if (!fs->bdev->write_block) {
        return -1;                   /* read-only Geraet */
    }
    return fs->bdev->write_block(fs->bdev, lba, buf);
}

/* Schreibt den gepflegten free_count/next_free in den FSInfo-Sektor zurueck (Signaturen bleiben
 * erhalten, da wir den Sektor RMW behandeln). Bester Aufwand -- ein Hint, kein harter Fehler. */
static void fsinfo_flush(fat_fs_t *fs)
{
    if (!fs->fsinfo_lba || !fs->bdev->write_block) {
        return;
    }
    if (rd_block(fs, fs->fsinfo_lba, fsisec)) {
        return;
    }
    fsisec[488] = (uint8_t)fs->free_count;        fsisec[489] = (uint8_t)(fs->free_count >> 8);
    fsisec[490] = (uint8_t)(fs->free_count >> 16); fsisec[491] = (uint8_t)(fs->free_count >> 24);
    fsisec[492] = (uint8_t)fs->next_free;          fsisec[493] = (uint8_t)(fs->next_free >> 8);
    fsisec[494] = (uint8_t)(fs->next_free >> 16);  fsisec[495] = (uint8_t)(fs->next_free >> 24);
    (void)wr_block(fs, fs->fsinfo_lba, fsisec);
}

uint32_t fat32_free_count(const fat_fs_t *fs)
{
    return (fs && fs->mounted) ? fs->free_count : 0xFFFFFFFFu;
}

/* Schreibt einen FAT-Eintrag (28-bit val) in ALLE FAT-Kopien (RMW). */
static int fat_set_next(fat_fs_t *fs, uint32_t clus, uint32_t val)
{
    uint32_t fat_off = clus * 4;
    uint32_t sec     = fat_off / 512;
    uint32_t off     = fat_off % 512;
    for (uint32_t k = 0; k < fs->num_fats; k++) {
        uint64_t lba = fs->fat_lba + (uint64_t)k * fs->fat_size + sec;
        if (rd_block(fs, lba, fatwsec)) {
            return -1;
        }
        uint32_t old = rd32(&fatwsec[off]);
        uint32_t nv  = (old & 0xF0000000u) | (val & 0x0FFFFFFFu);
        fatwsec[off + 0] = (uint8_t)(nv);
        fatwsec[off + 1] = (uint8_t)(nv >> 8);
        fatwsec[off + 2] = (uint8_t)(nv >> 16);
        fatwsec[off + 3] = (uint8_t)(nv >> 24);
        if (wr_block(fs, lba, fatwsec)) {
            return -1;
        }
    }
    fs->fatbuf_lba = (uint64_t)-1;   /* Read-Cache invalidieren */
    return 0;
}

/* Sucht einen freien Cluster (ab dem next-free-Hint, mit Wrap), markiert ihn als EOC und gibt ihn
 * zurueck (0 = voll). Pflegt free_count/next_free + schreibt FSInfo zurueck. */
static uint32_t fat_alloc_cluster(fat_fs_t *fs)
{
    if (fs->max_cluster <= 2) {
        return 0;
    }
    uint32_t span = fs->max_cluster - 2;
    uint32_t base = (fs->next_free >= 2 && fs->next_free < fs->max_cluster) ? (fs->next_free - 2) : 0;
    for (uint32_t i = 0; i < span; i++) {
        uint32_t c = 2 + (base + i) % span;      /* ab dem Hint suchen, dann umlaufen */
        uint32_t e = fat_next(fs, c);
        if (fs->io_err) {
            return 0;
        }
        if (e == 0) {                            /* frei */
            if (fat_set_next(fs, c, 0x0FFFFFFF)) {
                return 0;
            }
            if (fs->free_count != 0xFFFFFFFFu && fs->free_count > 0) { fs->free_count--; }
            fs->next_free = 2 + ((c - 2 + 1) % span);
            fsinfo_flush(fs);
            return c;
        }
    }
    return 0;
}

static int fat_free_chain(fat_fs_t *fs, uint32_t clus)
{
    uint32_t freed = 0;
    while (cluster_valid(fs, clus)) {
        uint32_t next = fat_next(fs, clus);
        if (fs->io_err) {
            return -1;
        }
        if (next == 0) {
            break;   /* Cluster schon frei -> Zyklus-Rueckkehr/korrupte Kette (untrusted hdd2):
                      * abbrechen, statt ihn doppelt zu zaehlen (free_count-Hint bliebe sonst +1).
                      * Da jeder besuchte Cluster genullt wird, liest ein Rueckverweis hier 0. */
        }
        if (fat_set_next(fs, clus, 0)) {
            return -1;
        }
        freed++;
        if (clus >= 2 && clus < fs->next_free) { fs->next_free = clus; }   /* frei -> guter Hint */
        clus = next;
    }
    if (freed) {
        if (fs->free_count != 0xFFFFFFFFu) { fs->free_count += freed; }
        fsinfo_flush(fs);
    }
    return 0;
}

/* Sucht den Verzeichniseintrag fuer raw oder einen freien Slot. Liefert dessen
 * Sektor-LBA + Offset und den evtl. existierenden Start-Cluster. 0/-1. */
static int dir_find_or_alloc(fat_fs_t *fs, uint32_t dir_cluster, const uint8_t *raw,
                             uint64_t *out_lba, uint32_t *out_off, uint32_t *existing)
{
    uint32_t clus      = dir_cluster;
    uint32_t hare      = dir_cluster;
    uint32_t last_clus = dir_cluster;
    int      hare_live = 1;
    int64_t  free_lba  = -1;
    uint32_t free_off  = 0;
    *existing = 0;

    while (cluster_valid(fs, clus)) {
        last_clus = clus;
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            uint64_t lba = cluster_lba(fs, clus) + s;
            if (rd_block(fs, lba, secbuf)) {
                return -1;
            }
            for (int e = 0; e < 512; e += 32) {
                uint8_t *de = &secbuf[e];
                if (de[0] == 0x00) {
                    if (free_lba < 0) { free_lba = (int64_t)lba; free_off = (uint32_t)e; }
                    *out_lba = (uint64_t)free_lba;
                    *out_off = free_off;
                    return 0;        /* nicht gefunden -> freier Slot */
                }
                if (de[0] == 0xE5) {
                    if (free_lba < 0) { free_lba = (int64_t)lba; free_off = (uint32_t)e; }
                    continue;
                }
                uint8_t attr = de[11];
                if ((attr & 0x0F) == 0x0F || (attr & 0x08) || (attr & 0x10)) {
                    continue;        /* LFN / Volume-Label / Unterverzeichnis */
                }
                int match = 1;
                for (int i = 0; i < 11; i++) {
                    if (de[i] != raw[i]) { match = 0; break; }
                }
                if (match) {
                    *out_lba   = lba;
                    *out_off   = (uint32_t)e;
                    *existing  = ((uint32_t)rd16(&de[20]) << 16) | rd16(&de[26]);
                    return 0;        /* existierender Eintrag */
                }
            }
        }
        if (dir_chain_step(fs, &clus, &hare, &hare_live) || fs->io_err) {
            fs->io_err = 1;             /* Zyklus oder I/O-Fehler -> Abbruch */
            return -1;
        }
    }
    if (free_lba >= 0) {
        *out_lba = (uint64_t)free_lba;
        *out_off = free_off;
        return 0;
    }
    /* Verzeichnis voll -> um EINEN Cluster wachsen lassen: neuen Cluster allozieren,
     * an die Kette haengen, komplett nullen (alle Slots = 0x00/frei), ersten Slot
     * zurueckgeben. So koennen Dateien/Unterverzeichnisse beliebig hinzukommen. */
    uint32_t newc = fat_alloc_cluster(fs);
    if (newc == 0) {
        return -1;                    /* Disk voll */
    }
    if (fat_set_next(fs, last_clus, newc)) {
        fat_free_chain(fs, newc);
        return -1;
    }
    for (uint32_t i = 0; i < 512; i++) {
        wsec[i] = 0;
    }
    for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
        if (wr_block(fs, cluster_lba(fs, newc) + s, wsec)) {
            return -1;
        }
    }
    *out_lba = cluster_lba(fs, newc);
    *out_off = 0;
    return 0;
}

/* ===================== Eintraege schreiben (8.3 + LFN) ===================== */

static int sl(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Schreibt einen 32-Byte-8.3-Verzeichniseintrag in den Puffer de. */
static void set_dirent(uint8_t *de, const uint8_t *name11, uint8_t attr,
                       uint32_t clus, uint32_t size)
{
    for (int i = 0; i < 32; i++) de[i] = 0;
    for (int i = 0; i < 11; i++) de[i] = name11[i];
    de[11] = attr;
    /* Zeitstempel eines NEUEN Eintrags: Erstellungs-, Zugriffs- UND Schreibzeit auf JETZT. Beim
     * Ueberschreiben (fat32_write_file, ex.found) wird nur die Schreib-/Zugriffszeit aufgefrischt,
     * die Erstellungszeit bleibt erhalten (die Datei wird ja nicht neu angelegt). */
    uint16_t d = 0, t = 0;
    fat_now(&d, &t);
    de[13] = 0;                                             /* Erstellungszeit-Hundertstel */
    de[14] = (uint8_t)t;  de[15] = (uint8_t)(t >> 8);       /* Erstellungszeit */
    de[16] = (uint8_t)d;  de[17] = (uint8_t)(d >> 8);       /* Erstellungsdatum */
    de[18] = (uint8_t)d;  de[19] = (uint8_t)(d >> 8);       /* letzter Zugriff (nur Datum) */
    de[20] = (uint8_t)(clus >> 16); de[21] = (uint8_t)(clus >> 24);
    de[22] = (uint8_t)t;  de[23] = (uint8_t)(t >> 8);       /* letzte Schreibzeit */
    de[24] = (uint8_t)d;  de[25] = (uint8_t)(d >> 8);       /* letztes Schreibdatum */
    de[26] = (uint8_t)(clus);       de[27] = (uint8_t)(clus >> 8);
    de[28] = (uint8_t)(size);       de[29] = (uint8_t)(size >> 8);
    de[30] = (uint8_t)(size >> 16); de[31] = (uint8_t)(size >> 24);
}

/* Braucht 'name' einen LFN? Ja, wenn der 8.3-Roundtrip (Gross/Trunkierung) den Namen
 * veraendert (z.B. Kleinbuchstaben, > 8.3, Sonderzeichen). */
static int needs_lfn(const char *name)
{
    uint8_t raw[11];
    parse_name(name, raw);
    char back[16];
    fmt_name(raw, back);
    int i = 0;
    while (name[i] && back[i] && name[i] == back[i]) i++;
    return !(name[i] == '\0' && back[i] == '\0');
}

/* Baut einen eindeutigen 8.3-Kurzalias "BASE~N.EXT" fuer einen langen Namen. 0/-1. */
static int make_short_alias(fat_fs_t *fs, uint32_t dir, const char *longname,
                            uint8_t out11[11])
{
    char base[8]; int bl = 0;
    char ext[4];  int el = 0;
    int dot = -1;
    for (int i = 0; longname[i]; i++) { if (longname[i] == '.') dot = i; }
    for (int i = 0; longname[i] && (dot < 0 || i < dot) && bl < 6; i++) {
        char c = up(longname[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) base[bl++] = c;
    }
    if (bl == 0) base[bl++] = '_';
    if (dot >= 0) {
        for (int i = dot + 1; longname[i] && el < 3; i++) {
            char c = up(longname[i]);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ext[el++] = c;
        }
    }
    for (int n = 1; n <= 9; n++) {
        uint8_t raw[11];
        for (int i = 0; i < 11; i++) raw[i] = ' ';
        int keep = (bl < 6) ? bl : 6;
        for (int i = 0; i < keep; i++) raw[i] = (uint8_t)base[i];
        raw[keep]     = '~';
        raw[keep + 1] = (uint8_t)('0' + n);
        for (int i = 0; i < el; i++) raw[8 + i] = (uint8_t)ext[i];
        char s[16];
        fmt_name(raw, s);
        struct dirent_loc loc;
        if (dir_lookup(fs, dir, s, &loc) != 0) return -1;     /* I/O-Fehler */
        if (!loc.found) {
            for (int i = 0; i < 11; i++) out11[i] = raw[i];
            return 0;
        }
    }
    return -1;                                                /* ~1..~9 vergeben */
}

/* Fuellt einen LFN-Eintrag (ord = Ordinal, is_last = letzter logisch -> 0x40-Bit). */
static void set_lfn_entry(uint8_t *de, int ord, int is_last, uint8_t cksum,
                          const char *name, int namelen)
{
    static const int LSLOT[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int i = 0; i < 32; i++) de[i] = 0;
    de[0]  = (uint8_t)(ord | (is_last ? 0x40 : 0));
    de[11] = 0x0F;
    de[13] = cksum;
    int base = (ord - 1) * 13;
    for (int j = 0; j < 13; j++) {
        int idx = base + j;
        uint16_t ch;
        if (idx < namelen)      ch = (uint8_t)name[idx];
        else if (idx == namelen) ch = 0x0000;     /* Terminator */
        else                     ch = 0xFFFF;      /* Padding */
        de[LSLOT[j]]     = (uint8_t)(ch & 0xFF);
        de[LSLOT[j] + 1] = (uint8_t)(ch >> 8);
    }
}

/* Sucht 'count' aufeinanderfolgende freie Slots (0x00/0xE5) INNERHALB eines Sektors;
 * waechst notfalls um einen genullten Cluster. Liefert lba+off des ERSTEN Slots.
 * count <= 16 (passt in einen Sektor). 0/-1. */
static int dir_alloc_run(fat_fs_t *fs, uint32_t dir_cluster, int count,
                         uint64_t *out_lba, uint32_t *out_off)
{
    if (count < 1 || count > 16) return -1;
    uint32_t clus = dir_cluster, hare = dir_cluster, last = dir_cluster;
    int hare_live = 1;
    while (cluster_valid(fs, clus)) {
        last = clus;
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            uint64_t lba = cluster_lba(fs, clus) + s;
            if (rd_block(fs, lba, secbuf)) return -1;
            int run = 0, run_off = 0;
            for (int e = 0; e < 512; e += 32) {
                if (secbuf[e] == 0x00 || secbuf[e] == 0xE5) {
                    if (run == 0) run_off = e;
                    if (++run >= count) {
                        *out_lba = lba; *out_off = (uint32_t)run_off;
                        return 0;
                    }
                } else {
                    run = 0;
                }
            }
            /* Der Run passte NICHT in diesen Sektor (nur ein Rest < count frei). Wird der Eintrag
             * gleich in einem FOLGE-Sektor platziert, entstuende hier eine 0x00-Luecke -- und
             * dir_iterate/dir_is_empty/dir_find_or_alloc deuten das ERSTE 0x00 als Verzeichnisende
             * und wuerden den spaeteren Eintrag NIE sehen. Darum die 0x00-Endmarker dieses Sektors
             * zu 0xE5 (frei, aber "es folgt evtl. mehr") umschreiben -> Leser ueberspringen sie. */
            int changed = 0;
            for (int e = 0; e < 512; e += 32) {
                if (secbuf[e] == 0x00) { secbuf[e] = 0xE5; changed = 1; }
            }
            if (changed && wr_block(fs, lba, secbuf)) return -1;
        }
        if (dir_chain_step(fs, &clus, &hare, &hare_live) || fs->io_err) {
            fs->io_err = 1;
            return -1;
        }
    }
    /* Wachsen: neuer genullter Cluster -> 16 freie Slots am Stueck. */
    uint32_t newc = fat_alloc_cluster(fs);
    if (newc == 0) return -1;
    if (fat_set_next(fs, last, newc)) { fat_free_chain(fs, newc); return -1; }
    for (uint32_t i = 0; i < 512; i++) wsec[i] = 0;
    for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
        if (wr_block(fs, cluster_lba(fs, newc) + s, wsec)) return -1;
    }
    *out_lba = cluster_lba(fs, newc);
    *out_off = 0;
    return 0;
}

/* Legt einen NEUEN Eintrag fuer 'leaf' (langer oder 8.3-Name) im Verzeichnis dir an:
 * passt der Name in 8.3 -> ein Slot; sonst LFN-Eintraege + eindeutiger Kurzalias.
 * Liefert die Position des 8.3-Eintrags (loc_lba/loc_off). 0/-1. */
static int dir_put_entry(fat_fs_t *fs, uint32_t dir, const char *leaf, uint8_t attr,
                         uint32_t clus, uint32_t size, uint64_t *loc_lba, uint32_t *loc_off)
{
    uint8_t short11[11];
    if (!needs_lfn(leaf)) {
        parse_name(leaf, short11);
        uint32_t existing;
        if (dir_find_or_alloc(fs, dir, short11, loc_lba, loc_off, &existing) != 0) return -1;
        if (rd_block(fs, *loc_lba, secbuf)) return -1;
        set_dirent(&secbuf[*loc_off], short11, attr, clus, size);
        return wr_block(fs, *loc_lba, secbuf);
    }
    int namelen = sl(leaf);
    if (namelen > 194) return -1;                 /* n+1<=16 Slots -> ein Sektor (13*15=195) */
    if (make_short_alias(fs, dir, leaf, short11) != 0) return -1;
    int n = (namelen + 13) / 13;                  /* Anzahl LFN-Eintraege */
    uint8_t cks = lfn_checksum(short11);
    if (dir_alloc_run(fs, dir, n + 1, loc_lba, loc_off) != 0) return -1;
    if (rd_block(fs, *loc_lba, secbuf)) return -1;
    for (int k = 0; k < n; k++) {                 /* physisch: hoechster Ordinal zuerst */
        int ord = n - k;
        set_lfn_entry(&secbuf[*loc_off + k * 32], ord, (ord == n), cks, leaf, namelen);
    }
    set_dirent(&secbuf[*loc_off + n * 32], short11, attr, clus, size);
    if (wr_block(fs, *loc_lba, secbuf)) return -1;
    *loc_off += (uint32_t)n * 32;                 /* Position des 8.3-Eintrags */
    return 0;
}

/* Markiert den 8.3-Eintrag bei (lba,off) als geloescht (0xE5) UND die unmittelbar davorstehenden
 * LFN-Eintraege mit passender Pruefsumme. Der LFN-Lauf wird ueber SEKTORGRENZEN hinweg verfolgt
 * (ein fremder Writer kann einen Lauf ueber die 512-B-Grenze gelegt haben) -- innerhalb desselben
 * Clusters (prev = lba-1). Verhindert verwaiste LFN-Eintraege. 0/-1.
 * Grenze: ein Lauf, der die CLUSTER-Grenze kreuzt, wird nicht weiterverfolgt (sehr selten; die
 * verbleibenden LFN-Eintraege sind harmlos -- werden beim Lesen als 8.3-lose Reste uebersprungen). */
static int mark_deleted_with_lfn(fat_fs_t *fs, uint64_t lba, uint32_t off)
{
    if (rd_block(fs, lba, secbuf)) return -1;
    uint8_t cks = lfn_checksum(&secbuf[off]);
    secbuf[off] = 0xE5;
    int e = (int)off - 32;
    for (;;) {
        for (; e >= 0; e -= 32) {
            uint8_t *de = &secbuf[e];
            if ((de[11] & 0x0F) == 0x0F && de[0] != 0xE5 && de[13] == cks) {
                de[0] = 0xE5;                        /* zum Lauf gehoerender LFN-Eintrag */
            } else {
                return wr_block(fs, lba, secbuf);    /* Lauf zu Ende (Nicht-LFN) */
            }
        }
        /* Sektoranfang erreicht, aber alle Eintraege gehoerten zum Lauf -> koennte im VORHERIGEN
         * Sektor weitergehen. Aktuellen Sektor sichern, dann (nur cluster-intern) zurueckgehen. */
        if (wr_block(fs, lba, secbuf)) return -1;
        if (((lba - fs->data_lba) % fs->sec_per_clus) == 0) {
            return 0;                                /* erster Sektor des Clusters -> Ende */
        }
        lba -= 1;
        if (rd_block(fs, lba, secbuf)) return -1;
        e = 512 - 32;                                /* letzten Eintrag des Vorsektors zuerst */
    }
}

int fat32_write_file(fat_fs_t *fs, const char *name, const void *buf, uint32_t len)
{
    if (!fs->mounted) {
        return -1;
    }
    fs->io_err = 0;

    char leaf[64];
    uint32_t parent;
    if (walk_to_parent(fs, name, &parent, leaf, sizeof(leaf)) != 0) {
        return -1;                   /* Pfad/Verzeichnis nicht aufloesbar */
    }
    /* Existierenden Eintrag suchen (langer ODER 8.3-Name) -> Overwrite vs. Neuanlage. */
    struct dirent_loc ex;
    if (dir_lookup(fs, parent, leaf, &ex) != 0 || fs->io_err) {
        return -1;
    }
    if (ex.found && (ex.attr & 0x10)) {
        return -1;                   /* Name ist ein Verzeichnis */
    }
    uint32_t existing = ex.found ? ex.clus : 0;

    /* Transaktionale Reihenfolge: ZUERST die neue Kette allozieren + Daten schreiben,
     * DANN den Verzeichniseintrag setzen, und ERST DANACH die alte Kette freigeben.
     * So bleibt bei disk-full/I/O-Fehler die alte Datei intakt (kein Leak/Cross-Link). */
    uint32_t clus_bytes = (uint32_t)fs->sec_per_clus * 512;
    uint32_t nclus      = len ? (len + clus_bytes - 1) / clus_bytes : 0;
    uint32_t first = 0, prev = 0, copied = 0;
    const uint8_t *src = (const uint8_t *)buf;

    for (uint32_t k = 0; k < nclus; k++) {
        uint32_t c = fat_alloc_cluster(fs);
        if (c == 0) {
            if (first) fat_free_chain(fs, first);
            return -1;                            /* voll -> neue Teilkette frei */
        }
        if (k == 0) {
            first = c;
        } else if (fat_set_next(fs, prev, c)) {
            fat_free_chain(fs, first);
            return -1;
        }
        for (uint32_t s = 0; s < fs->sec_per_clus && copied < len; s++) {
            uint32_t n = len - copied;
            if (n > 512) n = 512;
            for (uint32_t i = 0; i < 512; i++) {
                wsec[i] = (i < n) ? src[copied + i] : 0;
            }
            if (wr_block(fs, cluster_lba(fs, c) + s, wsec)) {
                fat_free_chain(fs, first);
                return -1;
            }
            copied += n;
        }
        prev = c;
    }

    if (ex.found) {
        /* Overwrite: nur Start-Cluster + Groesse des bestehenden 8.3-Eintrags
         * aktualisieren (Name/LFN bleiben erhalten). */
        if (rd_block(fs, ex.lba, secbuf)) {
            if (first) fat_free_chain(fs, first);
            return -1;
        }
        uint8_t *de = &secbuf[ex.off];
        de[20] = (uint8_t)(first >> 16); de[21] = (uint8_t)(first >> 24);
        de[26] = (uint8_t)(first);       de[27] = (uint8_t)(first >> 8);
        de[28] = (uint8_t)(len);         de[29] = (uint8_t)(len >> 8);
        de[30] = (uint8_t)(len >> 16);   de[31] = (uint8_t)(len >> 24);
        /* Modifikationszeit (last-write) auf JETZT aktualisieren -- das ist der Sinn der mtime.
         * Erstellungszeit (de[14..17]) bleibt erhalten (ein Overwrite erzeugt die Datei nicht neu). */
        uint16_t wd = 0, wt = 0;
        fat_now(&wd, &wt);
        de[22] = (uint8_t)wt; de[23] = (uint8_t)(wt >> 8);   /* letzte Schreibzeit */
        de[24] = (uint8_t)wd; de[25] = (uint8_t)(wd >> 8);   /* letztes Schreibdatum */
        de[18] = (uint8_t)wd; de[19] = (uint8_t)(wd >> 8);   /* letzter Zugriff (Datum) */
        if (wr_block(fs, ex.lba, secbuf)) {
            if (first) fat_free_chain(fs, first);
            return -1;
        }
    } else {
        /* Neuanlage: 8.3 oder LFN-Eintrag(e) anlegen. */
        uint64_t loc_lba;
        uint32_t loc_off;
        if (dir_put_entry(fs, parent, leaf, 0x20, first, len, &loc_lba, &loc_off) != 0) {
            if (first) fat_free_chain(fs, first);
            return -1;
        }
    }

    /* Jetzt zeigt der Eintrag auf die neue Kette -> alte gefahrlos freigeben. */
    if (existing >= 2) {
        if (fat_free_chain(fs, existing) != 0 || fs->io_err) {
            return -1;
        }
    }
    if (fs->io_err) {
        return -1;
    }
    return (int)len;
}

int fat32_delete(fat_fs_t *fs, const char *name)
{
    if (!fs->mounted) {
        return -1;
    }
    fs->io_err = 0;
    char leaf[64];
    uint32_t parent;
    if (walk_to_parent(fs, name, &parent, leaf, sizeof(leaf)) != 0) {
        return -1;                                   /* Pfad nicht aufloesbar */
    }
    struct dirent_loc loc;
    if (dir_lookup(fs, parent, leaf, &loc) != 0 || fs->io_err) {
        return -1;
    }
    if (!loc.found || (loc.attr & 0x10)) {
        return -1;                                   /* nicht gefunden / Verzeichnis (rmdir) */
    }

    /* Eintrag (8.3 + evtl. LFN-Eintraege) als geloescht markieren, dann Kette frei. */
    if (mark_deleted_with_lfn(fs, loc.lba, loc.off) != 0) {
        return -1;
    }
    if (loc.clus >= 2) {                             /* Cluster-Kette freigeben */
        /* Rueckgabewert UND io_err auswerten (analog fat32_write_file): bei I/O-Fehler
         * waehrend der Freigabe sonst verwaiste Cluster, faelschlich als Erfolg. */
        if (fat_free_chain(fs, loc.clus) != 0 || fs->io_err) {
            return -1;
        }
    }
    return 0;
}

/* ===================== Verzeichnisse anlegen/entfernen ===================== */

/* 1 = leer (nur "."/"..", keine lebenden Eintraege), 0 = nicht leer, -1 = Fehler. */
static int dir_is_empty(fat_fs_t *fs, uint32_t dir_clus)
{
    uint32_t clus = dir_clus, hare = dir_clus;
    int hare_live = 1;
    while (cluster_valid(fs, clus)) {
        for (uint32_t s = 0; s < fs->sec_per_clus; s++) {
            if (rd_block(fs, cluster_lba(fs, clus) + s, secbuf)) {
                return -1;
            }
            for (int e = 0; e < 512; e += 32) {
                uint8_t *de = &secbuf[e];
                if (de[0] == 0x00) {
                    return 1;                    /* Verzeichnisende -> leer */
                }
                if (de[0] == 0xE5) {
                    continue;
                }
                uint8_t attr = de[11];
                if ((attr & 0x0F) == 0x0F || (attr & 0x08)) {
                    continue;                    /* LFN / Volume-Label */
                }
                /* NUR die echten Punkt-Eintraege "." / ".." ueberspringen -- nicht
                 * jeden Namen mit fuehrendem 0x2E (ein praepariertes hdd2 koennte sonst
                 * einen lebenden ".XYZ"-Eintrag verstecken und rmdir damit taeuschen). */
                if (de[0] == '.' &&
                    (de[1] == ' ' || (de[1] == '.' && de[2] == ' '))) {
                    continue;                    /* "." / ".." */
                }
                return 0;                        /* lebender Eintrag -> nicht leer */
            }
        }
        if (dir_chain_step(fs, &clus, &hare, &hare_live) || fs->io_err) {
            return -1;
        }
    }
    return 1;
}

int fat32_mkdir(fat_fs_t *fs, const char *path)
{
    if (!fs->mounted) {
        return -1;
    }
    fs->io_err = 0;
    char leaf[64];
    uint32_t parent;
    if (walk_to_parent(fs, path, &parent, leaf, sizeof(leaf)) != 0) {
        return -1;
    }
    if (leaf[0] == '.') {
        return -1;                               /* "." / ".." nicht anlegbar */
    }
    struct dirent_loc loc;
    if (dir_lookup(fs, parent, leaf, &loc) != 0 || fs->io_err) {
        return -1;
    }
    if (loc.found) {
        return -1;                               /* Name (Datei/Verz.) existiert */
    }

    /* Cluster fuer das neue Verzeichnis allozieren + "."/".." schreiben. ZUERST den
     * Verzeichnisinhalt, DANN den Eintrag im Elternverzeichnis -> bei Fehler bleibt
     * hoechstens ein verwaister Cluster, kein Dangling-Dir-Eintrag. */
    uint32_t dir_clus = fat_alloc_cluster(fs);
    if (dir_clus == 0) {
        return -1;
    }
    uint8_t dot[11]  = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    uint8_t ddot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    for (uint32_t i = 0; i < 512; i++) wsec[i] = 0;
    set_dirent(&wsec[0],  dot,  0x10, dir_clus, 0);
    set_dirent(&wsec[32], ddot, 0x10, (parent == fs->root_cluster) ? 0 : parent, 0);
    if (wr_block(fs, cluster_lba(fs, dir_clus), wsec)) {
        fat_free_chain(fs, dir_clus);
        return -1;
    }
    for (uint32_t i = 0; i < 512; i++) wsec[i] = 0;  /* restliche Sektoren nullen */
    for (uint32_t s = 1; s < fs->sec_per_clus; s++) {
        if (wr_block(fs, cluster_lba(fs, dir_clus) + s, wsec)) {
            fat_free_chain(fs, dir_clus);
            return -1;
        }
    }

    /* Eintrag im Elternverzeichnis anlegen (8.3 oder LFN; dir_put_entry darf wachsen). */
    uint64_t loc_lba;
    uint32_t loc_off;
    if (dir_put_entry(fs, parent, leaf, 0x10, dir_clus, 0, &loc_lba, &loc_off) != 0) {
        fat_free_chain(fs, dir_clus);
        return -1;
    }
    if (fs->io_err) {
        return -1;
    }
    return 0;
}

int fat32_rmdir(fat_fs_t *fs, const char *path)
{
    if (!fs->mounted) {
        return -1;
    }
    fs->io_err = 0;
    char leaf[64];
    uint32_t parent;
    if (walk_to_parent(fs, path, &parent, leaf, sizeof(leaf)) != 0) {
        return -1;
    }
    if (leaf[0] == '.') {
        return -1;                               /* "." / ".." nicht entfernbar */
    }
    struct dirent_loc loc;
    if (dir_lookup(fs, parent, leaf, &loc) != 0 || fs->io_err) {
        return -1;
    }
    if (!loc.found || !(loc.attr & 0x10) || loc.clus < 2) {
        return -1;                               /* nicht gefunden / keine Verz. */
    }
    int empty = dir_is_empty(fs, loc.clus);
    if (empty != 1) {
        return -1;                               /* nicht leer oder Fehler */
    }
    /* Eintrag (8.3 + evtl. LFN) im Elternverzeichnis loeschen, dann Kette freigeben. */
    if (mark_deleted_with_lfn(fs, loc.lba, loc.off) != 0) {
        return -1;
    }
    if (fat_free_chain(fs, loc.clus) != 0 || fs->io_err) {
        return -1;
    }
    return 0;
}
