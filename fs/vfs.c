/*
 * fs/vfs.c  --  VFS-Schicht: SD -> MBR -> mehrere FAT32-Mounts (hdd0/hdd1)
 *
 * Mountet die FAT32-Partitionen der MBR der Reihe nach als hdd0 (System),
 * hdd1 (User), ... und loest Pfade "<mount>:<datei>" auf.
 */
#include <stdint.h>
#include "uart.h"
#include "aarch64.h"
#include "spinlock.h"
#include "sd.h"
#include "blockdev.h"
#include "fat32.h"
#include "usbmsc.h"
#include "vfs.h"

/* Globaler FS-Lock: der fat32-/SD-Zustand (secbuf, FAT-Cache, sektorpuffer) ist global und
 * NICHT pro-Aufruf reentrant. Mit SMP koennen mehrere Kerne gleichzeitig Datei-Syscalls
 * ausfuehren -> jede VFS-Operation laeuft unter diesem Lock. IRQs werden dabei maskiert
 * (der Lock wird aus preemptierbarem Syscall-Kontext genommen -> sonst koennte ein
 * preemptierter Halter auf demselben Kern mit dem naechsten Task verklemmen). */
static spinlock_t s_fslock = SPINLOCK_INIT;

static uint64_t fs_lock(void)
{
    uint64_t f = READ_SYSREG(daif);
    irq_disable();
    spin_lock(&s_fslock);
    return f;
}
static void fs_unlock(uint64_t f)
{
    spin_unlock(&s_fslock);
    WRITE_SYSREG(daif, f);
}

/* --- SD als Block-Device --- */
static int sd_bd_read(blockdev_t *bd, uint64_t lba, void *buf)
{
    (void)bd;
    return sd_read_block(lba, buf);
}
static int sd_bd_write(blockdev_t *bd, uint64_t lba, const void *buf)
{
    (void)bd;
    return sd_write_block(lba, buf);
}
static blockdev_t sd_bdev = { sd_bd_read, sd_bd_write, 0 };

/* --- USB-Massenspeicher als Block-Device (je ein 512-Byte-Sektor) --- */
static int usb_bd_read(blockdev_t *bd, uint64_t lba, void *buf)
{
    (void)bd;
    return usbmsc_read((uint32_t)lba, 1, buf);
}
static int usb_bd_write(blockdev_t *bd, uint64_t lba, const void *buf)
{
    (void)bd;
    return usbmsc_write((uint32_t)lba, 1, buf);
}
static blockdev_t usb_bdev = { usb_bd_read, usb_bd_write, 0 };

/* --- Mount-Tabelle: hdd0/hdd1 = SD, hdd2 = USB-Stick --- */
#define MAX_MOUNTS 3
#define SD_MOUNTS  2
struct mount {
    const char *name;
    fat_fs_t    fs;
    int         used;
    int         read_only;
};
static struct mount mounts[MAX_MOUNTS];
static const char  *mount_names[MAX_MOUNTS] = { "hdd0", "hdd1", "hdd2" };

/* Cache-Line-aligned: dient als DMA-Ziel (USB-Pfad invalidiert ganze Cache-Lines). */
static uint8_t mbr[512] __attribute__((aligned(64)));

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static struct mount *find_mount(const char *name)
{
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used && streq(mounts[i].name, name)) {
            return &mounts[i];
        }
    }
    return 0;
}

int vfs_init(void)
{
    if (sd_init() != 0) {
        return -1;
    }
    if (sd_read_block(0, mbr) != 0) {
        uart_puts("    [vfs] MBR-Lesefehler\n");
        return -1;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        uart_puts("    [vfs] keine gueltige MBR-Signatur\n");
        return -1;
    }

    /* Nur primaere FAT32-Eintraege (0x0B/0x0C); erweiterte Partitionen
     * (0x05/0x0F) und EBR werden bewusst nicht unterstuetzt. Der i-te FAT32-
     * Eintrag wird hdd<i> -- UNABHAENGIG vom Mount-Erfolg, damit die Rollen
     * (hdd0=System, hdd1=User) stabil bleiben, falls eine Partition korrupt ist. */
    int fat_idx = 0;
    int mounted = 0;
    for (int i = 0; i < 4 && fat_idx < SD_MOUNTS; i++) {     /* SD -> hdd0, hdd1 */
        const uint8_t *pe = &mbr[446 + i * 16];
        uint8_t  type  = pe[4];
        uint32_t start = rd32(&pe[8]);
        uint32_t count = rd32(&pe[12]);
        if (count == 0 || (type != 0x0B && type != 0x0C)) {
            continue;
        }
        int slot = fat_idx++;
        mounts[slot].name      = mount_names[slot];
        mounts[slot].read_only = (slot == 0);   /* hdd0 = System (read-only) */
        if (fat32_mount(&mounts[slot].fs, &sd_bdev, start) == 0) {
            mounts[slot].used = 1;
            mounted++;
            uart_puts("    [vfs] ");
            uart_puts(mount_names[slot]);
            uart_puts(" gemountet (FAT32 @ LBA ");
            uart_putdec(start);
            uart_puts(")\n");
        } else {
            mounts[slot].used = 0;
            uart_puts("    [vfs] ");
            uart_puts(mount_names[slot]);
            uart_puts(": Mount fehlgeschlagen\n");
        }
    }
    if (mounted == 0) {
        uart_puts("    [vfs] keine FAT32-Partition gemountet\n");
        return -1;
    }
    return 0;
}

/* Den (bereits enumerierten) USB-Massenspeicher als hdd2 (read+write) einhaengen:
 * USB-MBR lesen, erste FAT32-Partition mounten. 0 = ok. */
int vfs_mount_usb(void)
{
    static uint8_t umbr[512] __attribute__((aligned(64)));   /* DMA-Ziel -> cache-line-aligned */
    if (usb_bdev.read_block(&usb_bdev, 0, umbr) != 0) {
        uart_puts("    [vfs] USB-MBR-Lesefehler\n");
        return -1;
    }
    if (umbr[510] != 0x55 || umbr[511] != 0xAA) {
        uart_puts("    [vfs] USB: keine gueltige MBR-Signatur\n");
        return -1;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t *pe = &umbr[446 + i * 16];
        uint8_t  type  = pe[4];
        uint32_t start = rd32(&pe[8]);
        uint32_t count = rd32(&pe[12]);
        if (count == 0 || (type != 0x0B && type != 0x0C)) {
            continue;
        }
        struct mount *m = &mounts[2];           /* hdd2 */
        m->name      = mount_names[2];
        m->read_only = 0;                        /* USB-Stick ist schreibbar */
        if (fat32_mount(&m->fs, &usb_bdev, start) == 0) {
            m->used = 1;
            uart_puts("    [vfs] hdd2 (USB-Stick) gemountet (FAT32 @ LBA ");
            uart_putdec(start);
            uart_puts(")\n");
            return 0;
        }
        uart_puts("    [vfs] hdd2 (USB): Mount fehlgeschlagen\n");
        return -1;
    }
    uart_puts("    [vfs] USB: keine FAT32-Partition gefunden\n");
    return -1;
}

void vfs_list(const char *mount_name)
{
    /* T1.10: unter fs_lock -- fat32_list scannt den GLOBALEN secbuf (dir_iterate); ohne Lock
     * korrumpiert ein gleichzeitiger FS-Zugriff auf einem anderen Kern den Sektorpuffer.
     * (Konsolenausgabe laeuft hier mit unter dem Lock; boot-only Diagnose, kein RT-Hotpath.) */
    uint64_t f = fs_lock();
    struct mount *m = find_mount(mount_name);
    if (m) {
        fat32_list(&m->fs);
    } else {
        uart_puts("      (Mount nicht gefunden)\n");
    }
    fs_unlock(f);
}

/* Gepflegter FSInfo-Free-Count des Mounts (0xFFFFFFFF = unbekannt / nicht gefunden). */
uint32_t vfs_free_count(const char *mount_name)
{
    uint64_t f = fs_lock();
    struct mount *m = find_mount(mount_name);
    uint32_t r = m ? fat32_free_count(&m->fs) : 0xFFFFFFFFu;
    fs_unlock(f);
    return r;
}

void vfs_list_all(void)
{
    /* T1.10: das GESAMTE Multi-Mount-Listing atomar unter fs_lock -- jedes fat32_list scannt
     * den geteilten secbuf (siehe vfs_list). Laeuft im Boot [5] einmalig auf Kern 0. */
    uint64_t f = fs_lock();
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].used) {
            uart_puts("    ");
            uart_puts(mounts[i].name);
            uart_puts(":\n");
            fat32_list(&mounts[i].fs);
        }
    }
    fs_unlock(f);
}

/* Zerlegt "<mount>:<datei>"; ohne ':' -> hdd0 (System). Liefert Mount + Dateiname. */
static struct mount *vfs_resolve(const char *path, const char **out_file)
{
    const char *colon = path;
    while (*colon && *colon != ':') {
        colon++;
    }
    if (*colon == ':') {
        char nm[8];
        int  k = 0;
        for (const char *p = path; p < colon && k < 7; p++) {
            nm[k++] = *p;
        }
        nm[k] = '\0';
        *out_file = colon + 1;
        return find_mount(nm);
    }
    *out_file = path;
    return find_mount("hdd0");
}

int vfs_read_file(const char *path, void *buf, uint32_t max_len)
{
    uint64_t f = fs_lock();
    const char   *file;
    struct mount *m = vfs_resolve(path, &file);
    int ret;
    if (!m) {
        uart_puts("    [vfs] Mount nicht gefunden: ");
        uart_puts(path);
        uart_puts("\n");
        ret = VFS_ENOENT;                /* Mount/Datei nicht vorhanden (nicht: Lesefehler) */
    } else {
        ret = fat32_read_file(&m->fs, file, buf, max_len);   /* reicht 0.., -1, VFS_ENOENT durch */
    }
    fs_unlock(f);
    return ret;
}

static int vfs_write_common(const char *path, const void *buf, uint32_t len, int priv)
{
    uint64_t f = fs_lock();
    const char   *file;
    struct mount *m = vfs_resolve(path, &file);
    int ret;
    if (!m) {
        uart_puts("    [vfs] Mount nicht gefunden: ");
        uart_puts(path);
        uart_puts("\n");
        ret = -1;
    } else if (m->read_only && !priv) {
        uart_puts("    [vfs] ");
        uart_puts(m->name);
        uart_puts(" ist read-only\n");
        ret = -1;
    } else {
        ret = fat32_write_file(&m->fs, file, buf, len);
    }
    fs_unlock(f);
    return ret;
}

int vfs_write_file(const char *path, const void *buf, uint32_t len)
{
    return vfs_write_common(path, buf, len, 0);
}

/* Privilegierter Schreibpfad: umgeht die read-only-Policy von hdd0. NUR
 * kernel-intern (z.B. Benutzerverwaltung) -- NICHT ueber Syscalls erreichbar. */
int vfs_write_file_priv(const char *path, const void *buf, uint32_t len)
{
    return vfs_write_common(path, buf, len, 1);
}

/* Listet ein Verzeichnis als Text in buf. path = "hdd1" (Wurzel) oder
 * "hdd1:DOCS" / "hdd1:A/B" (Unterverzeichnis -> nach dem ':'). */
int vfs_listdir(const char *path, char *buf, uint32_t max)
{
    char        nm[8];
    const char *sub = "";
    int         k = 0;
    const char *p = path;
    while (*p && *p != ':' && k < 7) {
        nm[k++] = *p++;
    }
    nm[k] = '\0';
    if (*p == ':') {
        sub = p + 1;                            /* Unterpfad nach dem Doppelpunkt */
    }
    uint64_t f = fs_lock();
    struct mount *m = find_mount(nm);
    int ret = m ? fat32_listdir(&m->fs, sub, buf, max) : -1;
    fs_unlock(f);
    return ret;
}

/* Loescht eine Datei "<mount>:<datei>". Auf read-only-Mounts (hdd0) abgelehnt. */
int vfs_delete(const char *path)
{
    uint64_t f = fs_lock();
    const char   *file;
    struct mount *m = vfs_resolve(path, &file);
    int ret = (m && !m->read_only) ? fat32_delete(&m->fs, file) : -1;
    fs_unlock(f);
    return ret;
}

/* Legt ein Verzeichnis "<mount>:<pfad>" an / entfernt ein leeres. read-only abgelehnt. */
int vfs_mkdir(const char *path)
{
    uint64_t f = fs_lock();
    const char   *sub;
    struct mount *m = vfs_resolve(path, &sub);
    int ret = (m && !m->read_only) ? fat32_mkdir(&m->fs, sub) : -1;
    fs_unlock(f);
    return ret;
}

int vfs_rmdir(const char *path)
{
    uint64_t f = fs_lock();
    const char   *sub;
    struct mount *m = vfs_resolve(path, &sub);
    int ret = (m && !m->read_only) ? fat32_rmdir(&m->fs, sub) : -1;
    fs_unlock(f);
    return ret;
}
