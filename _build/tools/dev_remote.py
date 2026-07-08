#!/usr/bin/env python3
# tools/dev_remote.py -- Host-Client fuer das Dev-Remote-Interface (UDP) des rpi_rtos (docs/architecture/20).
#
# Steuert einen laufenden Pi4 (Dev-Image, gebaut mit `.\build.ps1 -DevRemote`) fern:
#   output                       laufenden Konsolen-/Shell-Text mitlesen (Strg-C beendet)
#   screen <out.bmp>             einen Framebuffer-Abzug anfordern (RLE) -> BMP speichern
#   key "<text>"                 Tastatur-Bytes senden (z.B. "run hdd1:AIVISION.ELF\n")
#   mouse <dx> <dy> <buttons>    Maus-Delta + Buttons senden
#   sendfile <lokal> <hdd0|hdd1>:<PFAD>   Datei uebertragen (Seq/Ack + Retransmit)
#   deploy-kernel <kernel8.img>  = sendfile auf die Boot-Partition (hdd0:KERNEL8.IMG) + restart
#   restart                      Neustart ausloesen
#   selftest                     Protokoll-Kodierung STANDALONE pruefen (kein Geraet noetig)
#
# Pure-stdlib (socket/struct/zlib), analog tools/gen_*.py. Sicherheit: dieses Interface existiert
# NUR in -DevRemote-Dev-Images; das RC-/Release-Image enthaelt kein Byte davon.
import socket, struct, sys, time, os

DEV_MAGIC = 0x4D455244            # 'D','R','E','M' little-endian
DEV_PORT  = 5599
HDR       = struct.Struct("<IBBH")   # magic, type, flags, seq

# Nachrichtentypen (Spiegel von include/dev_remote.h)
PING, KEY, MOUSE            = 0x00, 0x01, 0x02
FILE_BEGIN, FILE_CHUNK, FILE_END = 0x10, 0x11, 0x12
SCREEN_REQ, RESTART        = 0x20, 0x30
PONG, OUTPUT, SCREEN_DATA, ACK, RESULT = 0x80, 0x81, 0x82, 0x8F, 0x92

CHUNK = 1024                       # Datei-Chunk-Nutzlast


def build_hdr(mtype, flags, seq):
    return HDR.pack(DEV_MAGIC, mtype, flags, seq)


def parse_hdr(pkt):
    if len(pkt) < HDR.size:
        return None
    magic, mtype, flags, seq = HDR.unpack_from(pkt, 0)
    if magic != DEV_MAGIC:
        return None
    return (mtype, flags, seq, pkt[HDR.size:])


def rle_decode(data):
    """[u16 count][u32 pixel]* -> Liste von 0xAARRGGBB-Pixeln."""
    out = []
    i = 0
    while i + 6 <= len(data):
        run = data[i] | (data[i + 1] << 8)
        px = struct.unpack_from("<I", data, i + 2)[0]
        i += 6
        out.extend([px] * run)
    return out


def rle_encode(px):
    out = bytearray()
    i = 0
    n = len(px)
    while i < n:
        p = px[i]
        run = 1
        while i + run < n and px[i + run] == p and run < 65535:
            run += 1
        out += struct.pack("<HI", run, p)
        i += run
    return bytes(out)


def write_bmp(path, w, h, px):
    """px = row-major 0xAARRGGBB -> 24-bit-BMP (bottom-up, BGR)."""
    row = ((w * 3 + 3) // 4) * 4
    pixdata = bytearray()
    for y in range(h - 1, -1, -1):            # BMP bottom-up
        line = bytearray()
        for x in range(w):
            v = px[y * w + x] if y * w + x < len(px) else 0
            line += bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF])   # BGR
        line += bytes(row - len(line))
        pixdata += line
    hdr = bytearray(b"BM")
    hdr += struct.pack("<IHHI", 54 + len(pixdata), 0, 0, 54)
    hdr += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pixdata), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + pixdata)


class Dev:
    def __init__(self, host, port=DEV_PORT, timeout=2.0):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.seq = 1

    def send(self, mtype, payload=b"", flags=0, seq=None):
        s = self.seq if seq is None else seq
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendto(build_hdr(mtype, flags, s), self.addr) if not payload \
            else self.sock.sendto(build_hdr(mtype, flags, s) + payload, self.addr)
        return s

    def recv(self):
        try:
            pkt, _ = self.sock.recvfrom(65535)
            return parse_hdr(pkt)
        except socket.timeout:
            return None

    def send_reliable(self, mtype, payload, seq):
        """Sendet, bis ein ACK mit derselben Seq kommt (bis 8 Versuche)."""
        for _ in range(8):
            self.send(mtype, payload, seq=seq)
            for _ in range(4):
                r = self.recv()
                if r and r[0] == ACK and r[2] == seq:
                    return True
        return False

    def key(self, text):
        data = text.encode("latin-1")
        self.send(KEY, data)

    def mouse(self, dx, dy, buttons):
        self.send(MOUSE, struct.pack("<hhB", dx, dy, buttons & 0xFF))

    def restart(self):
        self.send(RESTART)
        print("RESTART gesendet.")

    def sendfile(self, local, partition, path):
        with open(local, "rb") as f:
            data = f.read()
        total = len(data)
        nchunks = (total + CHUNK - 1) // CHUNK
        # FILE_BEGIN: partition(1) + pathlen(1) + path + total_size(4) + chunk_size(4)
        pb = path.encode("latin-1")
        begin = struct.pack("<BB", partition, len(pb)) + pb + struct.pack("<II", total, CHUNK)
        if not self.send_reliable(FILE_BEGIN, begin, self.seq):
            print("FILE_BEGIN nicht bestaetigt (laeuft der D2-Agent am Pi4?)"); return False
        self.seq = (self.seq + 1) & 0xFFFF
        for idx in range(nchunks):
            chunk = data[idx * CHUNK:(idx + 1) * CHUNK]
            payload = struct.pack("<I", idx) + chunk
            if not self.send_reliable(FILE_CHUNK, payload, self.seq):
                print("Chunk %d nicht bestaetigt." % idx); return False
            self.seq = (self.seq + 1) & 0xFFFF
        # FILE_END: einfache Pruefsumme (Summe mod 2^32)
        cksum = sum(data) & 0xFFFFFFFF
        self.send(FILE_END, struct.pack("<I", cksum))
        r = self.recv()
        ok = bool(r and r[0] == RESULT and r[3][:2] == b"OK")
        print("%s -> %s:%s : %s (%d Byte, %d Chunks)" %
              (local, ("hdd0", "hdd1")[partition], path, "OK" if ok else "FEHLER/keine Antwort", total, nchunks))
        return ok

    def screen(self, out_path):
        self.send(SCREEN_REQ)
        # SCREEN_DATA: erstes Paket traegt w(2)+h(2)+nchunks(2); dann chunk-indizierte RLE-Bloecke.
        w = h = nchunks = 0
        chunks = {}
        deadline = time.time() + 5.0
        while time.time() < deadline:
            r = self.recv()
            if not r or r[0] != SCREEN_DATA:
                continue
            body = r[3]
            idx = r[2]
            if idx == 0:
                w, h, nchunks = struct.unpack_from("<HHH", body, 0)
                chunks[0] = body[6:]
            else:
                chunks[idx] = body
            if nchunks and len(chunks) >= nchunks:
                break
        if not nchunks:
            print("Kein Framebuffer empfangen (laeuft der D2-Agent am Pi4?)"); return False
        rle = b"".join(chunks[i] for i in range(nchunks) if i in chunks)
        px = rle_decode(rle)
        write_bmp(out_path, w, h, px)
        print("Framebuffer %dx%d -> %s (%d Pixel)" % (w, h, out_path, len(px)))
        return True

    def output(self):
        print("Konsolen-Stream (Strg-C beendet):")
        self.sock.settimeout(1.0)
        try:
            while True:
                r = self.recv()
                if r and r[0] == OUTPUT:
                    sys.stdout.write(r[3].decode("latin-1", "replace"))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            print("\n[beendet]")


def selftest():
    """Protokoll-Kodierung STANDALONE pruefen (spiegelt den C-Kern D1)."""
    ok = True

    def check(name, cond):
        nonlocal ok
        print(("  [PASS] " if cond else "  [FAIL] ") + name)
        ok = ok and cond

    h = build_hdr(FILE_CHUNK, 0, 0x1234)
    check("Header-Bytes (Magic LE + type + seq)",
          h == bytes([0x44, 0x52, 0x45, 0x4D, 0x11, 0x00, 0x34, 0x12]))
    p = parse_hdr(h)
    check("Header-Parse-Roundtrip", p is not None and p[0] == FILE_CHUNK and p[2] == 0x1234)
    check("Header-Reject (falsches Magic)", parse_hdr(b"\x00\x00\x00\x00\x11\x00\x34\x12") is None)

    src = [0x11111111, 0x11111111, 0x11111111, 0x22222222, 0x33333333, 0x33333333]
    enc = rle_encode(src)
    check("RLE-Encode-Laenge (3 Laeufe * 6 B)", len(enc) == 18)
    dec = rle_decode(enc)
    check("RLE-Roundtrip == Original", dec == src)
    # Bytegleichheit mit dem C-Kern: erster Lauf = count 3, pixel 0x11111111
    check("RLE-Bytelayout (u16 count + u32 pixel LE)",
          enc[0:6] == struct.pack("<HI", 3, 0x11111111))

    print("Dev-Remote-Protokoll (Host): " + ("ALLE PASS" if ok else "FEHLER"))
    return ok


def usage():
    print(__doc__.strip())
    sys.exit(2)


def main(argv):
    if len(argv) < 2:
        usage()
    cmd = argv[1]
    if cmd == "selftest":
        sys.exit(0 if selftest() else 1)

    host = os.environ.get("RPI_DEV_HOST")
    args = argv[2:]
    if args and args[0] == "--host":
        host = args[1]; args = args[2:]
    if not host:
        print("Kein Host: --host <ip> oder $RPI_DEV_HOST setzen."); sys.exit(2)
    d = Dev(host)

    if cmd == "output":
        d.output()
    elif cmd == "screen":
        d.screen(args[0] if args else "screen.bmp")
    elif cmd == "key":
        d.key(args[0].replace("\\n", "\n").replace("\\t", "\t"))
    elif cmd == "mouse":
        d.mouse(int(args[0]), int(args[1]), int(args[2]) if len(args) > 2 else 0)
    elif cmd == "restart":
        d.restart()
    elif cmd == "sendfile":
        part = 0 if args[1].startswith("hdd0") else 1
        path = args[1].split(":", 1)[1]
        sys.exit(0 if d.sendfile(args[0], part, path) else 1)
    elif cmd == "deploy-kernel":
        okk = d.sendfile(args[0], 0, "KERNEL8.IMG")
        if okk:
            d.restart()
        sys.exit(0 if okk else 1)
    else:
        usage()


if __name__ == "__main__":
    main(sys.argv)
