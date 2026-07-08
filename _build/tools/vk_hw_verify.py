#!/usr/bin/env python3
# tools/vk_hw_verify.py -- Vulkan-auf-echter-Pi4-HW-Verifikation ueber das Dev-Remote-Interface (UDP).
#
# Voraussetzung: der Pi4 laeuft mit einem -DevImage-Kernel (Auto-Login -> Shell + GUI-Cap + Dev-Agent,
# statische IP 192.168.10.244:5599). Serial ist auf der realen HW tot -> dieses Skript nutzt den
# dev-remote-Konsolen-Tee (Marker) + den Framebuffer-Abzug (gerendertes Bild) als Verifikationskanal.
#
# Ablauf:
#   1. (--deploy) frische user/vkcube.elf + vktest.elf + vk_vert.spv + vk_frag.spv nach hdd1 spielen.
#   2. `run hdd1:VKTEST.ELF` -> die vollstaendige Vulkan-Testsuite auf dem Silizium ausfuehren und
#      alle [vktest]/[vkcube]-Marker ueber UDP einsammeln (=ok / FEHLER zaehlen).
#   3. `run hdd1:VKCUBE.ELF` -> den animierten Vulkan-Wuerfel rendern und den HDMI-Framebuffer als
#      BMP abgreifen (visueller Beweis, dass Vulkan end-to-end auf der HW rendert).
#
# Nutzung:
#   python _build/tools/vk_hw_verify.py --host 192.168.10.244 --deploy --shot cube.bmp
#
# Pure-stdlib; teilt den Protokoll-Kern mit tools/dev_remote.py (dort Dev-Klasse + robustes sendfile).
import sys, os, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dev_remote import Dev, KEY, OUTPUT, SCREEN_REQ, SCREEN_DATA, PING, PONG, rle_decode, write_bmp
import struct


def capture_output(d, seconds, idle_stop=6.0):
    """OUTPUT-Text ueber N Sekunden einsammeln (frueher Abbruch bei laengerem Leerlauf)."""
    d.sock.settimeout(0.3)
    end = time.time() + seconds
    buf = []
    last = time.time()
    while time.time() < end:
        r = d.recv()
        if r and r[0] == OUTPUT:
            buf.append(r[3].decode("latin-1", "replace"))
            last = time.time()
        elif buf and time.time() - last > idle_stop:
            break
    d.sock.settimeout(2.0)
    return "".join(buf)


def ping(d):
    d.send(PING)
    r = d.recv()
    return bool(r and r[0] == PONG)


ARTIFACTS = [
    ("_build/vkcube.elf", 1, "VKCUBE.ELF"),
    ("_build/vktest.elf", 1, "VKTEST.ELF"),
    ("user/vk_vert.spv", 1, "VERT.SPV"),
    ("user/vk_frag.spv", 1, "FRAG.SPV"),
]


def main(argv):
    host = os.environ.get("RPI_DEV_HOST")
    deploy = False
    shot = "cube.bmp"
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--host":
            host = argv[i + 1]; i += 2
        elif a == "--deploy":
            deploy = True; i += 1
        elif a == "--shot":
            shot = argv[i + 1]; i += 2
        else:
            i += 1
    if not host:
        print("Kein Host: --host <ip> oder $RPI_DEV_HOST setzen."); return 2

    d = Dev(host)
    print("[1] Erreichbarkeit:", "PONG (Pi4 + Dev-Agent aktiv)" if ping(d) else "KEINE ANTWORT")

    if deploy:
        print("[2] frische Vulkan-Artefakte nach hdd1 spielen:")
        for local, part, name in ARTIFACTS:
            ok = d.sendfile(local, part, name)
            if not ok:
                print("    FEHLER beim Transfer von", local); return 1
        d.key("kill\n"); time.sleep(1.0)   # evtl. laufenden VKCUBE freigeben (Slot/Framebuffer)

    # Wuerfel ZUERST im sauberen Zustand rendern+abgreifen (VKTEST spawnt am Ende einen eigenen,
    # ueberlebenden VKCUBE, der Slots/Framebuffer belegt -> die Kapturschritte nicht vermischen).
    print("[3] `run hdd1:VKCUBE.ELF` -- Wuerfel rendern + Framebuffer abgreifen:")
    d.key("run hdd1:VKCUBE.ELF\n")
    time.sleep(9.0)
    rendered = False
    px = None; w = h = 0
    for attempt in range(4):          # SCREEN_REQ mehrfach: OUTPUT-Flut kann eine Antwort verdraengen
        d.send(SCREEN_REQ)
        w = h = nchunks = 0
        chunks = {}
        d.sock.settimeout(0.4)
        deadline = time.time() + 5.0
        while time.time() < deadline:
            r = d.recv()
            if not r or r[0] != SCREEN_DATA:
                continue          # OUTPUT/andere Pakete ignorieren, weiter auf SCREEN_DATA warten
            body = r[3]; idx = r[2]
            if idx == 0:
                w, h, nchunks = struct.unpack_from("<HHH", body, 0); chunks[0] = body[6:]
            else:
                chunks[idx] = body
            if nchunks and len(chunks) >= nchunks:
                break
        if nchunks and len(chunks) >= nchunks:
            px = rle_decode(b"".join(chunks[i] for i in range(nchunks) if i in chunks))
            break
        time.sleep(1.0)
    if px is not None:
        write_bmp(shot, w, h, px)
        distinct = len(set(px))
        rendered = distinct >= 4   # Wuerfel = mehrere schattierte Flaechen + Clear-Hintergrund
        print("    Framebuffer %dx%d -> %s (%d distinct Farben)" % (w, h, shot, distinct))
    else:
        print("    kein Framebuffer empfangen (Wuerfel separat via dev_remote.py screen pruefbar)")
    d.key("kill\n"); time.sleep(0.8)      # VKCUBE beenden -> Slot fuer VKTEST + dessen VKCUBE frei
    capture_output(d, 2.0)                # OUTPUT-Rueckstand leeren

    print("[4] `run hdd1:VKTEST.ELF` -- vollstaendige Vulkan-Suite auf dem Silizium:")
    d.key("run hdd1:VKTEST.ELF\n")
    text = capture_output(d, 60.0)
    vk = [l for l in text.splitlines() if "[vktest]" in l or "[vkcube]" in l]
    fehler = [l for l in vk if "FEHLER" in l]
    for l in vk:
        print("   ", l)
    print("    -> %d Vulkan-Marker, %d mit FEHLER" % (len(vk), len(fehler)))
    d.key("kill\n")                       # den von VKTEST gespawnten VKCUBE beenden (Aufraeumen)

    suite_ok = len(vk) > 0 and len(fehler) == 0
    print("=" * 60)
    print("VKTEST-Suite auf HW: %s (%d Marker)" % ("ALLE OK" if suite_ok else "FEHLER", len(vk)))
    print("VKCUBE-Render auf HW: %s" % ("gerendert" if rendered else "NICHT bestaetigt"))
    ok = suite_ok and rendered
    print("VULKAN-AUF-HW: %s" % ("BESTAETIGT" if ok else "UNVOLLSTAENDIG"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
