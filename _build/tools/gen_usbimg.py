#!/usr/bin/env python3
# Erzeugt ein 8-MiB-USB-Stick-Image mit MBR und EINER FAT32-Partition (hdd2):
#   USBINFO.TXT  + optional USBHELLO.ELF (arg2 = user/hello.elf)
#
#   python _build/tools/gen_usbimg.py usb.img [user/hello.elf]
import struct, sys

SECTOR      = 512
IMG_SECTORS = 16384            # 8 MiB
P1_START    = 2048
P1_SECTORS  = IMG_SECTORS - P1_START
SEC_PER_CLUS = 1
RSVD        = 32
NUMFATS     = 2


def fatsz32(total, rsvd, numfats, spc):
    tmp1 = total - rsvd
    tmp2 = (256 * spc + numfats) // 2
    return (tmp1 + tmp2 - 1) // tmp2


def build_fat32(img, part_start, part_sectors, label11, files, cyclic=False):
    pstart     = part_start * SECTOR
    fatsz      = fatsz32(part_sectors, RSVD, NUMFATS, SEC_PER_CLUS)
    data_start = RSVD + NUMFATS * fatsz
    clus_bytes = SEC_PER_CLUS * SECTOR

    bpb = bytearray(SECTOR)
    bpb[0:3]   = bytes([0xEB, 0x58, 0x90])
    bpb[3:11]  = b'MSDOS5.0'
    struct.pack_into('<H', bpb, 11, SECTOR)
    bpb[13]    = SEC_PER_CLUS
    struct.pack_into('<H', bpb, 14, RSVD)
    bpb[16]    = NUMFATS
    bpb[21]    = 0xF8
    struct.pack_into('<H', bpb, 24, 32)
    struct.pack_into('<H', bpb, 26, 64)
    struct.pack_into('<I', bpb, 28, part_start)
    struct.pack_into('<I', bpb, 32, part_sectors)
    struct.pack_into('<I', bpb, 36, fatsz)
    struct.pack_into('<I', bpb, 44, 2)
    struct.pack_into('<H', bpb, 48, 1)
    struct.pack_into('<H', bpb, 50, 6)
    bpb[64] = 0x80
    bpb[66] = 0x29
    struct.pack_into('<I', bpb, 67, 0x1234ABCD)
    bpb[71:82] = label11
    bpb[82:90] = b'FAT32   '
    bpb[510] = 0x55
    bpb[511] = 0xAA
    img[pstart:pstart + SECTOR] = bpb
    img[pstart + 6 * SECTOR:pstart + 7 * SECTOR] = bpb

    fsi = bytearray(SECTOR)
    struct.pack_into('<I', fsi, 0,   0x41615252)
    struct.pack_into('<I', fsi, 484, 0x61417272)
    struct.pack_into('<I', fsi, 488, 0xFFFFFFFF)
    struct.pack_into('<I', fsi, 492, 0xFFFFFFFF)
    struct.pack_into('<I', fsi, 508, 0xAA550000)
    img[pstart + SECTOR:pstart + 2 * SECTOR] = fsi

    fat = bytearray(fatsz * SECTOR)

    def set_fat(c, v):
        struct.pack_into('<I', fat, c * 4, v & 0x0FFFFFFF)
    set_fat(0, 0x0FFFFFF8)
    set_fat(1, 0x0FFFFFFF)
    set_fat(2, 0x0FFFFFFF)

    def dirent(name11, attr, clus, size):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        struct.pack_into('<H', e, 20, (clus >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, clus & 0xFFFF)
        struct.pack_into('<I', e, 28, size)
        return e

    def clus_off(c):
        return pstart + (data_start + (c - 2) * SEC_PER_CLUS) * SECTOR

    rootdir = bytearray()
    rootdir.extend(dirent(label11, 0x08, 0, 0))
    next_clus = 3
    for name11, data in files:
        nclus = max(1, (len(data) + clus_bytes - 1) // clus_bytes)
        start = next_clus
        for k in range(nclus):
            c = start + k
            set_fat(c, 0x0FFFFFFF if k == nclus - 1 else (c + 1))
            chunk = data[k * clus_bytes:(k + 1) * clus_bytes]
            off = clus_off(c)
            img[off:off + len(chunk)] = chunk
        rootdir.extend(dirent(name11, 0x20, start, len(data)))
        next_clus += nclus

    if cyclic:
        # Boesartiges/korruptes Medium: zyklische Root-Dir-Cluster-Kette 2->3->2->...
        # Beide Cluster werden mit 0xE5 (geloeschte Eintraege) gefuellt, also KEIN
        # 0x00-Endmarker -> ohne Zyklusschutz liefe root_iterate/fat32_delete endlos
        # (kompletter RTOS-Hang, DoS). Dient als Regressionstest fuer den Guard.
        set_fat(2, 3)
        set_fat(3, 2)

    fat_abs = pstart + RSVD * SECTOR
    img[fat_abs:fat_abs + len(fat)] = fat
    img[fat_abs + fatsz * SECTOR:fat_abs + 2 * fatsz * SECTOR] = fat
    img[clus_off(2):clus_off(2) + len(rootdir)] = rootdir
    if cyclic:
        for c in (2, 3):                       # Cluster 2+3 komplett mit 0xE5 fuellen
            off = clus_off(c)
            img[off:off + clus_bytes] = b'\xE5' * clus_bytes


img = bytearray(IMG_SECTORS * SECTOR)


def part_entry(boot, ptype, start, count):
    e = bytearray(16)
    e[0] = boot
    e[1:4] = bytes([0xFE, 0xFF, 0xFF])
    e[4] = ptype
    e[5:8] = bytes([0xFE, 0xFF, 0xFF])
    struct.pack_into('<I', e, 8, start)
    struct.pack_into('<I', e, 12, count)
    return e


mbr = bytearray(SECTOR)
mbr[446:462] = part_entry(0x00, 0x0C, P1_START, P1_SECTORS)
mbr[510] = 0x55
mbr[511] = 0xAA
img[0:SECTOR] = mbr

# --cyclic: praepariertes Medium mit zyklischer Root-Dir-Kette (DoS-Regressionstest).
cyclic = '--cyclic' in sys.argv
pos = [a for a in sys.argv[1:] if not a.startswith('--')]

files = [(b'USBINFO TXT', b'Datei vom USB-Stick (hdd2), gelesen ueber DWC2 + BOT/SCSI.\r\n')]
if len(pos) > 1:
    with open(pos[1], 'rb') as f:
        files.append((b'USBHELLOELF', f.read()))

build_fat32(img, P1_START, P1_SECTORS, b'RPI_USB    ', files, cyclic=cyclic)

with open(pos[0] if pos else 'usb.img', 'wb') as f:
    f.write(img)
print("OK: USB-Image (8 MiB, 1 FAT32-Partition @%d)%s"
      % (P1_START, " [ZYKLISCH/korrupt]" if cyclic else ""))
