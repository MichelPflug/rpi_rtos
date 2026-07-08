#!/usr/bin/env python3
# tools/gen_hwsd.py -- Bootfaehiges SD-Image fuer den echten Raspberry Pi 4 (T1.17-HW-Harness).
#
# Anders als gen_sdimg.py (reines QEMU-Test-Fixture) erzeugt dieses Skript ein Image, das die
# Pi-Firmware auch auf ECHTER HW booten kann:
#   Partition 1 (Typ 0x0C, bootable)  = Boot + hdd0 (System):
#       config.txt, kernel8.img, SYSTEM.TXT, INIT.ELF, SHELL.ELF  [+ Firmware falls --firmware]
#   Partition 2 (Typ 0x0C)            = hdd1 (User): WELCOME.TXT, USER.TXT
#
# Die FAT32/MBR-Mechanik wird aus gen_sdimg.py wiederverwendet (eine Quelle).
#
#   python _build/tools/gen_hwsd.py hwsd.img --kernel kernel8.img \
#          [--init user/hello.elf] [--shell user/shell.elf] \
#          [--firmware <dir mit start4.elf/fixup4.dat/bcm2711-rpi-4-b.dtb/armstub8-gic.bin>] \
#          [--total-mib 256] [--check]
#
# --check verifiziert das erzeugte Image rein am Host (Read-Back): MBR-Partitionstabelle,
# beide FAT32-BPBs und die Root-Verzeichnisse -> Pflichtdateien vorhanden + Groessen plausibel.
# So ist der SD-Fixture-Bau OHNE echte HW pruefbar; der Firmware-/Boot-Handoff selbst bleibt
# HW-only (QEMU liest weder config.txt noch die Firmware -- es laedt kernel8.img via -kernel).
import os, sys, struct, argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_sdimg import build_fat32, part_entry, SECTOR   # noqa: E402  (Pfad-Setup zuerst)

SEC_PER_MIB = (1024 * 1024) // SECTOR   # 2048

# config.txt fuer den RC-Boot (T1.17/T1.18). Werte an den PL011-UART auf GPIO14/15 gepinnt:
# enable_uart + init_uart_clock=48 MHz + disable-bt (sonst Mini-UART); enable_gic -> die
# Firmware waehlt automatisch armstub8-gic.bin. arm_64bit + kernel = unser Image.
CONFIG_TXT = (
    "# rpi_rtos RC-Boot (erzeugt von tools/gen_hwsd.py, T1.17)\r\n"
    "arm_64bit=1\r\n"
    "kernel=kernel8.img\r\n"
    "enable_uart=1\r\n"
    "init_uart_clock=48000000\r\n"
    "dtoverlay=disable-bt\r\n"
    "enable_gic=1\r\n"
    "# armstub8-gic.bin wird bei enable_gic=1 automatisch verwendet\r\n"
).encode("ascii")

# Bekannte Pi4-Firmware-Bootdateien (Top-Level). Wert = 8.3-Alias; bei Namen, die nicht in 8.3
# passen, traegt build_fat32 zusaetzlich LFN-Eintraege mit dem langen Namen ein.
# TBD-verify gegen raspberrypi/firmware (boot/): exakter Dateisatz + armstub-Name/Einstiegs-EL.
FW_FILES = [
    ("start4.elf",          b"START4  ELF", None),
    ("fixup4.dat",          b"FIXUP4  DAT", None),
    ("start4cd.elf",        b"START4CDELF", None),
    ("fixup4cd.dat",        b"FIXUP4CDDAT", None),
    ("bcm2711-rpi-4-b.dtb", b"BCM271~1DTB", "bcm2711-rpi-4-b.dtb"),
    ("armstub8-gic.bin",    b"ARMST~1 BIN", "armstub8-gic.bin"),
]


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def build(out_path, kernel, init_elf, shell_elf, firmware_dir, total_mib,
          vktest=None, vkcube=None, vert=None, frag=None, aivision=None, visimg=None, visnet=None):
    total_sectors = total_mib * SEC_PER_MIB
    if total_sectors & (total_sectors - 1):
        # QEMUs SD-Modell erwartet eine 2er-Potenz-Groesse; echte Karten sind es ohnehin.
        raise SystemExit("total-mib muss eine 2er-Potenz ergeben (z.B. 128/256/512)")

    p1_start = 2048
    p1_sectors = total_sectors // 2 - p1_start        # Boot+hdd0 (~ halbe Karte)
    p2_start = p1_start + p1_sectors
    p2_sectors = total_sectors - p2_start             # Rest = hdd1

    # FAT32 verlangt >= 65525 Cluster je Partition. Bei spc=1 heisst das Partitionen >~ 34 MiB;
    # eine zu kleine --total-mib erzeugte sonst still ein FAT16-grosses Volume (Firmware/Treiber
    # verweigern es). Lieber laut abbrechen mit Hinweis auf eine groessere Karte.
    for name, sectors in (("P1/Boot", p1_sectors), ("P2/hdd1", p2_sectors)):
        approx_clusters = sectors - 32 - 2 * ((sectors * 4) // SECTOR + 1)
        if approx_clusters < 65525:
            raise SystemExit("--total-mib=%d zu klein: %s haette nur ~%d Cluster (FAT32 braucht "
                             ">=65525). Groessere Karte waehlen (z.B. --total-mib 256)."
                             % (total_mib, name, approx_clusters))

    img = bytearray(total_sectors * SECTOR)

    # --- MBR: P1 bootable (0x80), beide Typ 0x0C (FAT32 LBA) ---
    mbr = bytearray(SECTOR)
    mbr[446:462] = part_entry(0x80, 0x0C, p1_start, p1_sectors)
    mbr[462:478] = part_entry(0x00, 0x0C, p2_start, p2_sectors)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    img[0:SECTOR] = mbr

    # --- Partition 1: Boot + hdd0 ---
    files0 = [
        (b"CONFIG  TXT", CONFIG_TXT),
        (b"KERNEL8 IMG", _read(kernel)),
        (b"SYSTEM  TXT", b"rpi_rtos System-Partition hdd0 (read-only gedacht).\r\n"),
    ]
    if init_elf:
        files0.append((b"INIT    ELF", _read(init_elf)))
    if shell_elf:
        files0.append((b"SHELL   ELF", _read(shell_elf)))
    # Flavor-Artefakte auf hdd0 (kmain spawnt sie von dort): -Vk -> VKTEST, -Vision -> AIVISION.
    if vktest:
        files0.append((b"VKTEST  ELF", _read(vktest)))
    if aivision:
        files0.append((b"AIVISIONELF", _read(aivision)))

    fw_added = []
    if firmware_dir:
        for fname, alias, longname in FW_FILES:
            fp = os.path.join(firmware_dir, fname)
            if os.path.isfile(fp):
                data = _read(fp)
                files0.append((alias, data) if longname is None else (alias, data, longname))
                fw_added.append(fname)
        if not fw_added:
            print("WARN: --firmware angegeben, aber keine bekannte Bootdatei gefunden in " + firmware_dir)
    else:
        # Kein Firmware-Baum: valides Image + Manifest, WELCHE Dateien fuer echten HW-Boot fehlen.
        # (In QEMU trotzdem bootbar -- dort liefert -kernel das Image, config.txt/Firmware ignoriert.)
        manifest = ("Fuer echten Pi4-Boot zusaetzlich aus raspberrypi/firmware (boot/) einspielen:\r\n"
                    + "".join("  - %s\r\n" % f for (f, _a, _l) in FW_FILES)
                    + "  - overlays/disable-bt.dtbo\r\n").encode("ascii")
        files0.append((b"FIRMWARETXT", manifest))

    build_fat32(img, p1_start, p1_sectors, b"RPI_BOOT   ", files0, spc=1)

    # --- Partition 2: hdd1 (User) ---
    files1 = [
        (b"WELCOME TXT", b"Willkommen auf der User-Partition hdd1 (echte HW).\r\n"),
        (b"USER    TXT", b"Beispiel-Userdatei auf hdd1.\r\n"),
    ]
    # Flavor-Artefakte auf hdd1 (per SYS_SPAWN/SYS_READ_FILE geladen): -Vk -> VKCUBE + Shader,
    # -Vision -> Test-Bild + Modell.
    if vkcube:
        files1.append((b"VKCUBE  ELF", _read(vkcube)))
    if vert:
        files1.append((b"VERT    SPV", _read(vert)))
    if frag:
        files1.append((b"FRAG    SPV", _read(frag)))
    if visimg:
        files1.append((b"VISIMG  BMP", _read(visimg)))
    if visnet:
        files1.append((b"VISNET  NET", _read(visnet)))
    build_fat32(img, p2_start, p2_sectors, b"RPI_USR    ", files1, spc=1)

    with open(out_path, "wb") as f:
        f.write(img)

    print("OK: %s (%d MiB, P1@%d [%d Sekt.] boot+hdd0, P2@%d [%d Sekt.] hdd1%s)" % (
        out_path, total_mib, p1_start, p1_sectors, p2_start, p2_sectors,
        (", Firmware: " + ", ".join(fw_added)) if fw_added else ", OHNE Firmware"))
    return p1_start, p1_sectors, p2_start, p2_sectors


# --------------------------------------------------------------------------------------------
# Read-Back-Verifikation (rein am Host, ohne QEMU/HW): MBR + beide FAT32-Root-Verzeichnisse.
# --------------------------------------------------------------------------------------------
def _u16(b, o):
    return b[o] | (b[o + 1] << 8)


def _u32(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def _fat_geom(img, part_start):
    """(base, bps, spc, fat_off, data_start, root_clus) einer FAT32-Partition; ValueError wenn kein FAT32."""
    base = part_start * SECTOR
    bpb = img[base:base + SECTOR]
    if not (bpb[510] == 0x55 and bpb[511] == 0xAA):
        raise ValueError("BPB-Signatur 0x55AA fehlt @ Partition %d" % part_start)
    if bytes(bpb[82:87]) != b"FAT32":
        raise ValueError("kein FAT32-Kennzeichen @ Partition %d" % part_start)
    bps = _u16(bpb, 11)
    spc = bpb[13]
    rsvd = _u16(bpb, 14)
    numfats = bpb[16]
    fatsz = _u32(bpb, 36)
    root_clus = _u32(bpb, 44)
    data_start = rsvd + numfats * fatsz
    fat_off = base + rsvd * bps
    return base, bps, spc, fat_off, data_start, root_clus


def fat_root_entries(img, part_start):
    """Liefert {8.3-Name(str): (size, start_cluster)} aller Root-Dateien einer FAT32-Partition."""
    base, bps, spc, fat_off, data_start, root_clus = _fat_geom(img, part_start)
    ents = {}
    c = root_clus
    guard = 0
    while 2 <= c < 0x0FFFFFF8 and guard < 1 << 20:
        guard += 1
        off = base + (data_start + (c - 2) * spc) * bps
        for e in range(0, spc * bps, 32):
            ent = img[off + e:off + e + 32]
            if ent[0] == 0x00:
                return ents                # Verzeichnisende
            if ent[0] == 0xE5:
                continue                   # geloescht
            attr = ent[11]
            if attr == 0x0F or (attr & 0x08):
                continue                   # LFN / Volume-Label
            name = bytes(ent[0:11]).decode("latin-1")
            clus = (_u16(ent, 20) << 16) | _u16(ent, 26)
            ents[name] = (_u32(ent, 28), clus)
        c = _u32(img, fat_off + c * 4) & 0x0FFFFFFF
    return ents


def fat_read_file(img, part_start, start_clus, size):
    """Liest die Cluster-Kette ab start_clus (size Byte) einer FAT32-Partition zurueck."""
    base, bps, spc, fat_off, data_start, _ = _fat_geom(img, part_start)
    out = bytearray()
    c = start_clus
    guard = 0
    while 2 <= c < 0x0FFFFFF8 and len(out) < size and guard < 1 << 20:
        guard += 1
        off = base + (data_start + (c - 2) * spc) * bps
        out += img[off:off + spc * bps]
        c = _u32(img, fat_off + c * 4) & 0x0FFFFFFF
    return bytes(out[:size])


def check(img_path, kernel, init_elf, shell_elf, firmware_dir,
          vktest=None, vkcube=None, vert=None, frag=None, aivision=None, visimg=None, visnet=None):
    with open(img_path, "rb") as f:
        img = bytearray(f.read())
    ok = True

    def assert_(name, cond):
        nonlocal ok
        print(("  [PASS] " if cond else "  [FAIL] ") + name)
        if not cond:
            ok = False

    # MBR
    assert_("MBR-Signatur 0x55AA", img[510] == 0x55 and img[511] == 0xAA)
    p1 = img[446:462]
    p2 = img[462:478]
    assert_("P1 bootable (0x80) + Typ 0x0C", p1[0] == 0x80 and p1[4] == 0x0C)
    assert_("P2 Typ 0x0C", p2[4] == 0x0C)
    p1_start = _u32(p1, 8)
    p2_start = _u32(p2, 8)
    assert_("P1 startet @ LBA 2048", p1_start == 2048)
    assert_("P2 liegt hinter P1", p2_start > p1_start)

    # Partition 1 (Boot + hdd0)
    try:
        n0 = fat_root_entries(img, p1_start)
    except ValueError as ex:
        assert_("P1 FAT32 lesbar: " + str(ex), False)
        n0 = {}

    def size_of(names, key):     # -1 wenn nicht vorhanden
        return names.get(key, (-1, 0))[0]

    # config.txt: nicht nur Praesenz, sondern INHALT (die boot-kritischen Direktiven).
    if "CONFIG  TXT" in n0:
        sz, cl = n0["CONFIG  TXT"]
        cfg = fat_read_file(img, p1_start, cl, sz)
        assert_("P1: config.txt korrekt (arm_64bit + kernel=kernel8.img + enable_uart)",
                b"arm_64bit=1" in cfg and b"kernel=kernel8.img" in cfg and b"enable_uart=1" in cfg)
    else:
        assert_("P1: config.txt vorhanden", False)
    kernel_size = os.path.getsize(kernel)
    assert_("P1: kernel8.img == Original-Groesse", size_of(n0, "KERNEL8 IMG") == kernel_size)
    assert_("P1: SYSTEM.TXT vorhanden (Groesse>0)", size_of(n0, "SYSTEM  TXT") > 0)
    # INIT/SHELL nur pruefen, wenn build() sie ueberhaupt einspielen sollte (beide optional).
    if init_elf:
        assert_("P1: INIT.ELF vorhanden (Groesse>0)", size_of(n0, "INIT    ELF") > 0)
    if shell_elf:
        assert_("P1: SHELL.ELF vorhanden (Groesse>0)", size_of(n0, "SHELL   ELF") > 0)
    # Firmware: jede TATSAECHLICH vorhandene Bootdatei muss auch im Image gelandet sein -- sonst
    # ist die --firmware-Partition (der einzige Grund fuer echten HW-Boot) still unvollstaendig.
    if firmware_dir:
        for fname, alias, _longname in FW_FILES:
            if os.path.isfile(os.path.join(firmware_dir, fname)):
                a = alias.decode("latin-1")
                assert_("P1: Firmware '%s' eingespielt (Groesse>0)" % fname, size_of(n0, a) > 0)

    # Partition 2 (hdd1)
    try:
        n1 = fat_root_entries(img, p2_start)
    except ValueError as ex:
        assert_("P2 FAT32 lesbar: " + str(ex), False)
        n1 = {}
    assert_("P2: WELCOME.TXT vorhanden (Groesse>0)", size_of(n1, "WELCOME TXT") > 0)

    # Flavor-Artefakte (P1 = hdd0, P2 = hdd1) -- nur pruefen, was build() einspielen sollte.
    if vktest:
        assert_("P1: VKTEST.ELF vorhanden (Groesse>0)", size_of(n0, "VKTEST  ELF") > 0)
    if aivision:
        assert_("P1: AIVISION.ELF vorhanden (Groesse>0)", size_of(n0, "AIVISIONELF") > 0)
    if vkcube:
        assert_("P2: VKCUBE.ELF vorhanden (Groesse>0)", size_of(n1, "VKCUBE  ELF") > 0)
    if vert:
        assert_("P2: VERT.SPV vorhanden (Groesse>0)", size_of(n1, "VERT    SPV") > 0)
    if frag:
        assert_("P2: FRAG.SPV vorhanden (Groesse>0)", size_of(n1, "FRAG    SPV") > 0)
    if visimg:
        assert_("P2: VISIMG.BMP vorhanden (Groesse>0)", size_of(n1, "VISIMG  BMP") > 0)
    if visnet:
        assert_("P2: VISNET.NET vorhanden (Groesse>0)", size_of(n1, "VISNET  NET") > 0)

    print("Read-Back: " + ("ALLE PASS" if ok else "FEHLER"))
    return ok


def main(argv):
    ap = argparse.ArgumentParser(description="Bootfaehiges Pi4-SD-Image (T1.17)")
    ap.add_argument("out", help="Ausgabe-Image (z.B. hwsd.img)")
    ap.add_argument("--kernel", default="kernel8.img")
    ap.add_argument("--init", default=None, help="INIT.ELF (User-App)")
    ap.add_argument("--shell", default=None, help="SHELL.ELF (Login-Shell)")
    ap.add_argument("--firmware", default=None, help="Verzeichnis mit Pi4-Boot-Firmware")
    ap.add_argument("--total-mib", type=int, default=256, help="Gesamtgroesse (2er-Potenz)")
    ap.add_argument("--check", action="store_true", help="nach dem Bau Read-Back verifizieren")
    # Flavor-Artefakte (T1.17-Erweiterung): -Vk (VKTEST/VKCUBE/Shader) bzw. -Vision (AIVISION/Fixtures).
    ap.add_argument("--vktest", default=None, help="VKTEST.ELF -> hdd0 (-Vk)")
    ap.add_argument("--vkcube", default=None, help="VKCUBE.ELF -> hdd1 (-Vk)")
    ap.add_argument("--vert", default=None, help="VERT.SPV -> hdd1 (-Vk)")
    ap.add_argument("--frag", default=None, help="FRAG.SPV -> hdd1 (-Vk)")
    ap.add_argument("--aivision", default=None, help="AIVISION.ELF -> hdd0 (-Vision)")
    ap.add_argument("--visimg", default=None, help="VISIMG.BMP -> hdd1 (-Vision)")
    ap.add_argument("--visnet", default=None, help="VISNET.NET -> hdd1 (-Vision)")
    a = ap.parse_args(argv[1:])

    flavor = dict(vktest=a.vktest, vkcube=a.vkcube, vert=a.vert, frag=a.frag,
                  aivision=a.aivision, visimg=a.visimg, visnet=a.visnet)
    build(a.out, a.kernel, a.init, a.shell, a.firmware, a.total_mib, **flavor)
    if a.check:
        if not check(a.out, a.kernel, a.init, a.shell, a.firmware, **flavor):
            raise SystemExit(1)


if __name__ == "__main__":
    main(sys.argv)
