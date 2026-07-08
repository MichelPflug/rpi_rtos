#!/usr/bin/env python3
# tools/gen_vision_fixtures.py -- M0-Test-Fixtures fuer den Vision-Track (docs/architecture/19).
#
# Erzeugt zwei Dateien, die auf hdd1 (untrusted-Datenpfad) gelegt und von AIVISION.ELF ueber
# SYS_READ_FILE geladen werden:
#   vision_img.bmp  -- 8x8 24bpp, ROT-dominant (R=200, G=20, B=20)
#   vision_net.net  -- Mini-Klassifikator:  GAP -> FC(3->2) -> Softmax
#                      FC-Gewichte W=[[1,0,-1],[-1,0,1]] -> out0=meanR-meanB, out1=meanB-meanR
#                      => rot-dominantes Bild -> argmax = Klasse 0 ("rot"), blau -> Klasse 1.
# Pure-stdlib (kein numpy/PyTorch), analog tools/gen_spirv.py. Der vollstaendige PyTorch/ONNX->
# *.net-Konverter (gen_model.py) folgt in Block A2, sobald ein echtes trainiertes Modell noetig ist.
import struct, sys

VI_NET_MAGIC   = 0x54454E56          # 'V','N','E','T'
VI_L_GAP, VI_L_FC, VI_L_SOFTMAX = 7, 8, 9


def make_bmp(w, h, rgb):
    r, g, b = rgb
    row = ((w * 3 + 3) // 4) * 4     # 4-Byte-Zeilenpadding
    pix = bytearray()
    for _y in range(h):
        line = bytearray()
        for _x in range(w):
            line += bytes([b, g, r])  # BGR
        line += bytes(row - len(line))
        pix += line
    pix_off = 54
    hdr = bytearray(b'BM')
    hdr += struct.pack('<IHHI', pix_off + len(pix), 0, 0, pix_off)
    hdr += struct.pack('<IiiHH', 40, w, h, 1, 24)                 # DIB, w, h, planes, bpp
    hdr += struct.pack('<IIiiII', 0, len(pix), 2835, 2835, 0, 0)  # comp, size, ppm x/y, colors
    return bytes(hdr) + bytes(pix)


def u32(x):  return struct.pack('<I', x)
def f32(x):  return struct.pack('<f', x)
def zeros5(): return b''.join(u32(0) for _ in range(5))


def make_net():
    out = bytearray()
    out += u32(VI_NET_MAGIC) + u32(1) + u32(3)          # magic, version, n_layers
    out += u32(3) + u32(8) + u32(8)                     # in 3x8x8
    # L0: Global-Average-Pool -> [3][1][1]
    out += u32(VI_L_GAP) + zeros5() + u32(0) + u32(0)
    # L1: FC 3->2 (out=2, w=6, b=2)
    out += u32(VI_L_FC) + u32(2) + b''.join(u32(0) for _ in range(4)) + u32(6) + u32(2)
    out += b''.join(f32(v) for v in (1.0, 0.0, -1.0,   -1.0, 0.0, 1.0))   # [out][in]
    out += f32(0.0) + f32(0.0)                          # bias
    # L2: Softmax ueber 2 Logits
    out += u32(VI_L_SOFTMAX) + zeros5() + u32(0) + u32(0)
    return bytes(out)


def main(argv):
    bmp_path = argv[1] if len(argv) > 1 else 'user/vision_img.bmp'
    net_path = argv[2] if len(argv) > 2 else 'user/vision_net.net'
    with open(bmp_path, 'wb') as f:
        f.write(make_bmp(8, 8, (200, 20, 20)))
    with open(net_path, 'wb') as f:
        f.write(make_net())
    print("OK: %s + %s" % (bmp_path, net_path))


if __name__ == '__main__':
    main(sys.argv)
