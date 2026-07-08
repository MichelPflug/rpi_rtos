/*
 * include/fat32.h  --  Minimaler FAT32-Leser (mehrfach mountbar, Handle-basiert)
 */
#ifndef RPI_RTOS_FAT32_H
#define RPI_RTOS_FAT32_H

#include <stdint.h>
#include "blockdev.h"

/* Eine gemountete FAT32-Instanz. Mehrere Instanzen (z.B. hdd0/hdd1) parallel. */
typedef struct {
    blockdev_t *bdev;
    uint64_t    fat_lba;
    uint64_t    data_lba;
    uint32_t    root_cluster;
    uint8_t     sec_per_clus;
    uint8_t     num_fats;
    uint32_t    fat_size;         /* Sektoren pro FAT */
    uint32_t    max_cluster;      /* obere Cluster-Schranke (exklusiv) */
    int         mounted;
    int         io_err;           /* gesetzt bei Blocklesefehler waehrend Operation */
    uint64_t    fatbuf_lba;        /* Cache: aktuell geladener FAT-Sektor */
    uint8_t     fatbuf[512];
    uint64_t    fsinfo_lba;        /* FSInfo-Sektor (absolut) oder 0 = keiner */
    uint32_t    free_count;        /* freie Cluster laut FSInfo (0xFFFFFFFF = unbekannt) */
    uint32_t    next_free;         /* Suchstart-Hint fuer den naechsten freien Cluster */
} fat_fs_t;

/* Test-/Diagnosezugriff: liefert den (gepflegten) FSInfo-Free-Count, 0xFFFFFFFF wenn unbekannt. */
uint32_t fat32_free_count(const fat_fs_t *fs);

/* Mountet die FAT32-Partition bei absoluter LBA part_lba auf bdev. 0/-1. */
int  fat32_mount(fat_fs_t *fs, blockdev_t *bdev, uint64_t part_lba);

/* Listet das Wurzelverzeichnis ueber die UART. */
void fat32_list(fat_fs_t *fs);

/* Listet ein Verzeichnis als Text ("NAME.EXT  SIZE\n" bzw. "NAME  <DIR>\n" je Eintrag)
 * in buf. path = "" -> Wurzelverzeichnis, sonst Unterverzeichnis ("DOCS", "A/B").
 * Liefert die geschriebene Laenge (ohne Nullterminator) oder -1. */
int  fat32_listdir(fat_fs_t *fs, const char *path, char *buf, uint32_t max);

/* Loescht eine Datei. name ist ein Pfad ("DATEI.TXT" oder "DIR/DATEI.TXT", 8.3-
 * Komponenten); Verzeichnisse werden abgelehnt (rmdir separat). 0/-1. */
int  fat32_delete(fat_fs_t *fs, const char *name);

/* Legt ein Unterverzeichnis an (mit "."/".."); das Elternverzeichnis muss existieren,
 * der Name darf noch nicht vergeben sein. 0/-1. */
int  fat32_mkdir(fat_fs_t *fs, const char *path);

/* Entfernt ein LEERES Unterverzeichnis (nur "."/".." enthalten). 0/-1. */
int  fat32_rmdir(fat_fs_t *fs, const char *path);

/* Liest eine Datei. name ist ein Pfad ("DATEI.TXT" oder "DIR/SUB/DATEI.TXT"). Kopiert
 * min(dateigroesse, max_len) Bytes in buf und liefert die WAHRE DATEIGROESSE zurueck (oder -1
 * bei Fehler/korrupt). return > max_len signalisiert dem Aufrufer TRUNKIERUNG (Datei groesser als
 * der Puffer) -- statt still eine gekuerzte Datei zu liefern. Fuer Dateien <= max_len ist
 * return == kopierte Bytes (abwaertskompatibel). Nicht reentrant (geteilter Scratch). */
int  fat32_read_file(fat_fs_t *fs, const char *name, void *buf, uint32_t max_len);

/* Schreibt len Bytes als Datei (Pfad wie oben; legt an oder ueberschreibt; das
 * Zielverzeichnis muss existieren). Anzahl Bytes oder -1 (voll / I/O / Dir voll). */
int  fat32_write_file(fat_fs_t *fs, const char *name, const void *buf, uint32_t len);

#ifdef RTOS_SELFTEST
/* T1.10-Guardian: Belegungs-Instrumentierung des geteilten secbuf-Verzeichnis-Scans
 * (dir_iterate). `fat32_secbuf_widen`=1 weitet das Beobachtungsfenster waehrend des
 * Nebenlaeufigkeits-Tests; occ_max muss 1 sein, violation 0 (jede secbuf-Nutzung serialisiert). */
extern volatile int fat32_secbuf_widen;
uint32_t fat32_secbuf_occ_max(void);
uint32_t fat32_secbuf_occ_violation(void);
#endif

#endif /* RPI_RTOS_FAT32_H */
