/*
 * include/vfs.h  --  VFS-Schicht mit Mount-Tabelle (hdd0 = System, hdd1 = User)
 *
 * Pfadschema: "<mount>:<datei>", z.B. "hdd0:INIT.ELF" oder "hdd1:WELCOME.TXT".
 * Ohne Praefix wird hdd0 (System) angenommen.
 */
#ifndef RPI_RTOS_VFS_H
#define RPI_RTOS_VFS_H

#include <stdint.h>

/* Initialisiert SD, parst die MBR und mountet die FAT32-Partitionen als
 * hdd0, hdd1, ... 0 = mindestens eine gemountet, -1 = Fehler. */
int  vfs_init(void);

/* Haengt den enumerierten USB-Massenspeicher als hdd2 (read+write) ein. 0 = ok. */
int  vfs_mount_usb(void);

/* Listet das Wurzelverzeichnis eines Mounts bzw. aller Mounts. */
void vfs_list(const char *mount_name);
void vfs_list_all(void);

/* vfs_read_file-Rueckgabecodes fuer den Fehlerfall (Erfolg: 0..wahre Groesse):
 *   VFS_ENOENT (-2) = Datei GENUIN nicht vorhanden (bzw. Mount fehlt).
 *   -1              = vorhanden, aber Lese-/Korruptionsfehler (I/O, kurze Cluster-Kette,
 *                     Verzeichnis statt Datei). Aufrufer, die "fehlt" von "kaputt"
 *                     unterscheiden muessen (z.B. user_init -> fail-closed), pruefen darauf. */
#define VFS_ENOENT (-2)

/* Liest/schreibt eine Datei ueber einen Pfad "<mount>:<datei>". Schreiben auf
 * einen read-only-Mount (hdd0) wird abgelehnt. vfs_read_file liefert die WAHRE Dateigroesse
 * (return > max_len => Trunkierung; siehe fat32_read_file). */
int  vfs_read_file(const char *path, void *buf, uint32_t max_len);
int  vfs_write_file(const char *path, const void *buf, uint32_t len);

/* Gepflegter FSInfo-Free-Count des Mounts (0xFFFFFFFF = unbekannt). */
uint32_t vfs_free_count(const char *mount_name);

/* Privilegierter Schreibpfad (umgeht read-only von hdd0); nur kernel-intern
 * fuer Systemdaten wie die Benutzer-DB. Nicht ueber Syscalls erreichbar. */
int  vfs_write_file_priv(const char *path, const void *buf, uint32_t len);

/* Listet ein Verzeichnis als Text in buf (Laenge/-1). path = "hdd1" (Wurzel) oder
 * "hdd1:DOCS" / "hdd1:A/B" (Unterverzeichnis). */
int  vfs_listdir(const char *path, char *buf, uint32_t max);
int  vfs_delete(const char *path);

/* Legt ein Verzeichnis "<mount>:<pfad>" an / entfernt ein leeres (read-only abgelehnt). */
int  vfs_mkdir(const char *path);
int  vfs_rmdir(const char *path);

#endif /* RPI_RTOS_VFS_H */
