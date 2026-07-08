#!/usr/bin/env python3
# Erzeugt ein 128-MiB-SD-Image mit MBR und ZWEI FAT32-Partitionen:
#   hdd0 (System)  -> SYSTEM.TXT [+ INIT.ELF, falls als arg uebergeben]
#   hdd1 (User)    -> WELCOME.TXT, USER.TXT
#
#   python _build/tools/gen_sdimg.py sd.img [user/hello.elf]
import struct, sys

SECTOR       = 512
IMG_SECTORS  = 262144          # 128 MiB
P1_START     = 2048            # hdd0
P1_SECTORS   = 129024
P2_START     = 131072          # hdd1
P2_SECTORS   = 131072
SEC_PER_CLUS = 1
RSVD         = 32
NUMFATS      = 2

def fatsz32(total, rsvd, numfats, spc):
    tmp1 = total - rsvd
    tmp2 = (256 * spc + numfats) // 2
    return (tmp1 + tmp2 - 1) // tmp2

LFN_SLOTS = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]

def lfn_checksum(short11):
    s = 0
    for c in short11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s

def lfn_entries(longname, short11):
    # Erzeugt die LFN-Eintraege (physisch hoechster Ordinal zuerst) fuer einen
    # langen Namen, verankert per Pruefsumme am 8.3-Aliasnamen short11.
    cks = lfn_checksum(short11)
    chars = [ord(c) for c in longname] + [0x0000]      # Terminator
    while len(chars) % 13 != 0:
        chars.append(0xFFFF)                           # Padding
    n = len(chars) // 13
    entries = []
    for i in range(n):
        ordn = i + 1
        e = bytearray(32)
        e[0] = ordn | (0x40 if ordn == n else 0)       # letzter logisch -> 0x40
        e[11] = 0x0F                                   # LFN-Attribut
        chunk = chars[i * 13:(i + 1) * 13]
        for j in range(13):
            struct.pack_into('<H', e, LFN_SLOTS[j], chunk[j])
        e[13] = cks
        entries.append(e)
    entries.reverse()
    return entries

def build_fat32(img, part_start, part_sectors, label11, files, subdirs=None, spc=SEC_PER_CLUS):
    # subdirs: Liste von (dirname11, [(fname11, data), ...]) -- ein Unterverzeichnis
    # je Eintrag (jeweils 1 Cluster gross). Liegt unter der Wurzel.
    # spc: Sektoren pro Cluster (Test-Fixture: hdd1 nutzt >1, um Multi-Sektor-Cluster zu pruefen).
    subdirs    = subdirs or []
    pstart     = part_start * SECTOR
    fatsz      = fatsz32(part_sectors, RSVD, NUMFATS, spc)
    data_start = RSVD + NUMFATS * fatsz
    clus_bytes = spc * SECTOR

    # BPB
    bpb = bytearray(SECTOR)
    bpb[0:3]   = bytes([0xEB, 0x58, 0x90])
    bpb[3:11]  = b'MSDOS5.0'
    struct.pack_into('<H', bpb, 11, SECTOR)
    bpb[13]    = spc
    struct.pack_into('<H', bpb, 14, RSVD)
    bpb[16]    = NUMFATS
    bpb[21]    = 0xF8
    struct.pack_into('<H', bpb, 24, 32)
    struct.pack_into('<H', bpb, 26, 64)
    struct.pack_into('<I', bpb, 28, part_start)
    struct.pack_into('<I', bpb, 32, part_sectors)
    struct.pack_into('<I', bpb, 36, fatsz)
    struct.pack_into('<I', bpb, 44, 2)            # Root-Cluster
    struct.pack_into('<H', bpb, 48, 1)            # FSInfo
    struct.pack_into('<H', bpb, 50, 6)            # Backup-Boot
    bpb[64] = 0x80
    bpb[66] = 0x29
    struct.pack_into('<I', bpb, 67, 0x12345678)
    bpb[71:82] = label11
    bpb[82:90] = b'FAT32   '
    bpb[510] = 0x55
    bpb[511] = 0xAA
    img[pstart:pstart + SECTOR] = bpb
    img[pstart + 6 * SECTOR:pstart + 7 * SECTOR] = bpb

    # FSInfo
    fsi = bytearray(SECTOR)
    struct.pack_into('<I', fsi, 0,   0x41615252)
    struct.pack_into('<I', fsi, 484, 0x61417272)
    struct.pack_into('<I', fsi, 488, 0xFFFFFFFF)
    struct.pack_into('<I', fsi, 492, 0xFFFFFFFF)
    struct.pack_into('<I', fsi, 508, 0xAA550000)
    img[pstart + SECTOR:pstart + 2 * SECTOR] = fsi

    # FAT + Root + Dateien
    fat = bytearray(fatsz * SECTOR)
    def set_fat(c, v):
        struct.pack_into('<I', fat, c * 4, v & 0x0FFFFFFF)
    set_fat(0, 0x0FFFFFF8)
    set_fat(1, 0x0FFFFFFF)
    set_fat(2, 0x0FFFFFFF)                          # Root-Dir 1 Cluster

    def dirent(name11, attr, clus, size):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        struct.pack_into('<H', e, 20, (clus >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, clus & 0xFFFF)
        struct.pack_into('<I', e, 28, size)
        return e

    def clus_off(c):
        return pstart + (data_start + (c - 2) * spc) * SECTOR

    state = {'next': 3}                              # naechster freier Cluster (Root=2)

    def write_chain(data):                           # Datei-Cluster-Kette schreiben
        nclus = max(1, (len(data) + clus_bytes - 1) // clus_bytes)
        start = state['next']
        for k in range(nclus):
            c = start + k
            set_fat(c, 0x0FFFFFFF if k == nclus - 1 else (c + 1))
            chunk = data[k * clus_bytes:(k + 1) * clus_bytes]
            img[clus_off(c):clus_off(c) + len(chunk)] = chunk
        state['next'] += nclus
        return start

    rootdir = bytearray()
    rootdir.extend(dirent(label11, 0x08, 0, 0))     # Volume-Label
    for fentry in files:                            # Root-Dateien (LFN-Tupel wie Subdirs)
        fname11, fdata = fentry[0], fentry[1]
        longname = fentry[2] if len(fentry) > 2 else None
        bad_ord  = fentry[3] if len(fentry) > 3 else None
        start = write_chain(fdata)
        if bad_ord:
            e = bytearray(32)
            e[0]  = 0x40 | bad_ord
            e[11] = 0x0F
            e[13] = lfn_checksum(fname11)
            for j in range(13):
                struct.pack_into('<H', e, LFN_SLOTS[j], 0x0041)   # 'A'
            rootdir.extend(e)
        elif longname:                              # LFN-Eintraege vor dem 8.3-Eintrag
            for le in lfn_entries(longname, fname11):
                rootdir.extend(le)
        rootdir.extend(dirent(fname11, 0x20, start, len(fdata)))

    for dirname11, dfiles in subdirs:               # Unterverzeichnis(se) anlegen
        dir_clus = state['next']
        state['next'] += 1
        set_fat(dir_clus, 0x0FFFFFFF)               # 1-Cluster-Verzeichnis
        sub = bytearray()
        sub.extend(dirent(b'.          ', 0x10, dir_clus, 0))  # "." -> sich selbst
        sub.extend(dirent(b'..         ', 0x10, 0, 0))         # ".." -> Root (clus 0)
        for fentry in dfiles:
            fname11, fdata = fentry[0], fentry[1]
            longname = fentry[2] if len(fentry) > 2 else None
            bad_ord  = fentry[3] if len(fentry) > 3 else None
            fstart = write_chain(fdata)
            if bad_ord:
                # EIN malformierter LFN-Eintrag: nur dieser Ordinal mit 0x40-Bit, passende
                # Pruefsumme, 13x 'A', KEIN Terminator -> luecken-/inkonsistente Sequenz.
                # Testet, dass der Kernel auf 8.3 zurueckfaellt statt Stack-Bytes zu leaken.
                e = bytearray(32)
                e[0]  = 0x40 | bad_ord
                e[11] = 0x0F
                e[13] = lfn_checksum(fname11)
                for j in range(13):
                    struct.pack_into('<H', e, LFN_SLOTS[j], 0x0041)   # 'A'
                sub.extend(e)
            elif longname:                              # LFN-Eintraege vor dem 8.3-Eintrag
                for le in lfn_entries(longname, fname11):
                    sub.extend(le)
            sub.extend(dirent(fname11, 0x20, fstart, len(fdata)))
        # Unterverzeichnisse werden als EIN Cluster angelegt (set_fat(dir_clus, EOC) oben) -> ein
        # Ueberlauf wuerde still in den naechsten (Datei-)Cluster schreiben. Lieber laut abbrechen.
        if len(sub) > clus_bytes:
            raise SystemExit("Unterverzeichnis %r (%d B) passt nicht in einen Cluster (%d B)."
                             % (dirname11, len(sub), clus_bytes))
        img[clus_off(dir_clus):clus_off(dir_clus) + len(sub)] = sub
        rootdir.extend(dirent(dirname11, 0x10, dir_clus, 0))

    fat_abs = pstart + RSVD * SECTOR
    img[fat_abs:fat_abs + len(fat)] = fat
    img[fat_abs + fatsz * SECTOR:fat_abs + 2 * fatsz * SECTOR] = fat
    # Das Root-Verzeichnis ist ebenfalls 1 Cluster (set_fat(2, EOC)); ein 17. Eintrag (bei spc=1)
    # wuerde sonst still den ersten Datei-Cluster (Cluster 3) ueberschreiben. Fail-loud statt Korruption.
    if len(rootdir) > clus_bytes:
        raise SystemExit("Root-Verzeichnis (%d B) passt nicht in einen Cluster (%d B) -- weniger "
                         "Dateien oder groessere sec_per_clus waehlen." % (len(rootdir), clus_bytes))
    img[clus_off(2):clus_off(2) + len(rootdir)] = rootdir

    # FSInfo Free-Count + Next-Free NACH allen Allokationen eintragen -> der Treiber muss beim
    # Mount die FAT nicht scannen (spart Bootzeit). Belegt sind Cluster 2..state['next']-1.
    total_clusters = (part_sectors - data_start) // spc
    free_count = total_clusters - (state['next'] - 2)
    struct.pack_into('<I', img, pstart + SECTOR + 488, free_count)
    struct.pack_into('<I', img, pstart + SECTOR + 492, state['next'])

# --- MBR mit zwei Partitionen ---
def part_entry(boot, ptype, start, count):
    e = bytearray(16)
    e[0] = boot
    e[1:4] = bytes([0xFE, 0xFF, 0xFF])
    e[4] = ptype
    e[5:8] = bytes([0xFE, 0xFF, 0xFF])
    struct.pack_into('<I', e, 8, start)
    struct.pack_into('<I', e, 12, count)
    return e


def main(argv):
    img = bytearray(IMG_SECTORS * SECTOR)

    mbr = bytearray(SECTOR)
    mbr[446:462] = part_entry(0x80, 0x0C, P1_START, P1_SECTORS)   # hdd0
    mbr[462:478] = part_entry(0x00, 0x0C, P2_START, P2_SECTORS)   # hdd1
    mbr[510] = 0x55
    mbr[511] = 0xAA
    img[0:SECTOR] = mbr

    # --- hdd0 (System) ---
    files0 = [(b'SYSTEM  TXT',
               b'rpi_rtos System-Partition hdd0 (read-only gedacht).\r\n')]
    hello = None
    if len(argv) > 2:                             # arg2 = User-App (hello)
        with open(argv[2], 'rb') as uf:
            hello = uf.read()
        files0.append((b'INIT    ELF', hello))
    if len(argv) > 3:                             # arg3 = Shell
        with open(argv[3], 'rb') as sf:
            files0.append((b'SHELL   ELF', sf.read()))
    if len(argv) > 8:                             # arg8 = GUI.ELF (T2.6 GUI-Sitzung nach dem Login)
        with open(argv[8], 'rb') as gf:
            files0.append((b'GUI     ELF', gf.read()))
    if len(argv) > 10:                            # arg10 = FPTEST.ELF (T3.1 FP-Kontext-Guardian)
        with open(argv[10], 'rb') as pf:
            files0.append((b'FPTEST  ELF', pf.read()))
    if len(argv) > 11:                            # arg11 = VKTEST.ELF (T3.2+ Rasterizer/Vulkan-Selbsttests)
        with open(argv[11], 'rb') as pf:
            files0.append((b'VKTEST  ELF', pf.read()))
    if len(argv) > 15:                            # arg15 = VKGUI.ELF (Vulkan-in-WinForms-Demo, hdd0)
        with open(argv[15], 'rb') as pf:
            files0.append((b'VKGUI   ELF', pf.read()))
    if len(argv) > 16:                            # arg16 = AIVISION.ELF (Vision-Track, docs/architecture/19)
        with open(argv[16], 'rb') as pf:
            files0.append((b'AIVISIONELF', pf.read()))
    build_fat32(img, P1_START, P1_SECTORS, b'RPI_SYS    ', files0)

    # --- hdd1 (User) ---  (hello zusaetzlich als HELLO.ELF, damit die Shell es 'run'nen kann)
    files1 = [(b'WELCOME TXT', b'Willkommen auf der User-Partition hdd1!\r\n'),
              (b'USER    TXT', b'Beispiel-Userdatei auf hdd1.\r\n'),
              # WWW-Fixture fuer den VFS-gebundenen HTTP-Server (Content-Type aus Endung):
              (b'INDEX   HTM', b'<html><body><h1>rpi_rtos httpd</h1>'
                               b'<p>marker=rpi_rtos-www-index-8c1d</p></body></html>\r\n'),
              (b'STYLE   CSS', b'body{font-family:monospace;color:#0a0}\r\n'),
              # LFN mit zwei aufeinanderfolgenden Punkten -> Regressionstest, dass der
              # HTTP-Resolver "release..notes.txt" NICHT faelschlich als Traversal abweist:
              (b'RELNOT~1TXT', b'marker=rpi_rtos-relnotes-2dots\r\n', 'release..notes.txt')]
    if hello is not None:
        files1.append((b'HELLO   ELF', hello))
    if len(argv) > 4:                            # arg4 = pchild (wait/kill-Testkind)
        with open(argv[4], 'rb') as pf:
            files1.append((b'PCHILD  ELF', pf.read()))
    if len(argv) > 5:                            # arg5 = pwaiter (kill-in-wait-Test)
        with open(argv[5], 'rb') as pf:
            files1.append((b'PWAITER ELF', pf.read()))
    if len(argv) > 6:                            # arg6 = ploop (unsterbliches Enkelkind)
        with open(argv[6], 'rb') as pf:
            files1.append((b'PLOOP   ELF', pf.read()))
    if len(argv) > 7:                            # arg7 = guitest (T2.1 GUI-Bruecke-EL0-Test)
        with open(argv[7], 'rb') as pf:
            files1.append((b'GUITEST ELF', pf.read()))
    if len(argv) > 9:                            # arg9 = GUIFONT.TTF (T2.8 Laufzeit-TTF, ASCII-Subset)
        with open(argv[9], 'rb') as pf:
            files1.append((b'GUIFONT TTF', pf.read()))
    if len(argv) > 10:                           # arg10 = FPTEST.ELF AUCH auf hdd1: der T3.1-Reuse-
        with open(argv[10], 'rb') as pf:         # Guardian spawnt die dritte Instanz via SYS_SPAWN,
            files1.append((b'FPTEST  ELF', pf.read()))   # und EL0 darf nur hdd1/hdd2 (Policy)
    if len(argv) > 12:                           # arg12 = VKCUBE.ELF (T3.6, via SYS_SPAWN -> hdd1)
        with open(argv[12], 'rb') as pf:
            files1.append((b'VKCUBE  ELF', pf.read()))
    if len(argv) > 13:                           # arg13 = VERT.SPV (T3.6, echte SPIR-V-Datei)
        with open(argv[13], 'rb') as pf:
            files1.append((b'VERT    SPV', pf.read()))
    if len(argv) > 14:                           # arg14 = FRAG.SPV
        with open(argv[14], 'rb') as pf:
            files1.append((b'FRAG    SPV', pf.read()))
    if len(argv) > 17:                           # arg17 = VISIMG.BMP (M0 Vision-Track, hdd1)
        with open(argv[17], 'rb') as pf:
            files1.append((b'VISIMG  BMP', pf.read()))
    if len(argv) > 18:                           # arg18 = VISNET.NET (M0 Vision-Track, hdd1)
        with open(argv[18], 'rb') as pf:
            files1.append((b'VISNET  NET', pf.read()))

    # Unterverzeichnis DOCS/ als Test-Fixture fuer ls/cat in Unterordner. Die dritte
    # Datei traegt einen LANGEN Namen (LFN) mit 8.3-Alias LANGEN~1.TXT.
    docs = [(b'README  TXT', b'README im Unterverzeichnis hdd1:DOCS (FAT32-Subdir).\r\n'),
            (b'NOTES   TXT', b'Notizen in hdd1:DOCS/NOTES.TXT.\r\n'),
            (b'LANGEN~1TXT', b'Datei mit langem Namen (LFN) in hdd1:DOCS.\r\n', 'LangeNotiz.txt'),
            # Malformierter LFN-Eintrag (Sicherheits-Regressionstest, Stack-Leak/8.3-Fallback):
            (b'BADLFN  TXT', b'8.3-Fallback bei kaputtem LFN.\r\n', None, 20)]
    # Fuellereintraege, damit der folgende LFN-Lauf eine 512-B-SEKTORGRENZE kreuzt (Test fuer das
    # LFN-Loeschen ueber Sektorgrenzen): ".",".." + 7 bestehende Eintraege = 9 (Offset 288); +6 Filler
    # -> Offset 480; dann CrossBoundary.txt (2 LFN + 8.3) bei 480/512/544 -> straddelt 512.
    for _i in range(1, 7):
        docs.append((b'FILL%d   TXT' % _i, b'Filler %d\r\n' % _i))
    docs.append((b'CROSSB~1TXT', b'LFN-Lauf ueber die Sektorgrenze.\r\n', 'CrossBoundary.txt'))
    docs.append((b'MARKER  TXT', b'marker-nach-cross-lfn\r\n'))
    # hdd1 nutzt sec_per_clus=4 (Multi-Sektor-Cluster) -- prueft die Cluster<->Sektor-Mathematik
    # des Treibers (Lesen/Schreiben/Verzeichnisse ueber mehrere Sektoren pro Cluster).
    build_fat32(img, P2_START, P2_SECTORS, b'RPI_USR    ', files1,
                subdirs=[(b'DOCS       ', docs)], spc=4)

    with open(argv[1] if len(argv) > 1 else 'sd.img', 'wb') as f:
        f.write(img)

    print("OK: 2 Partitionen (hdd0 @%d, hdd1 @%d)" % (P1_START, P2_START))


if __name__ == '__main__':
    main(sys.argv)
