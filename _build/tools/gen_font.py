#!/usr/bin/env python3
# tools/gen_font.py -- TTF -> rpi_rtos-Bitmap-Font (.rfn), rasterisiert auf dem Host.
#
# Warum Build-Zeit statt Laufzeit: EL0 laeuft integer-only (FP/SIMD getrappt), eine TrueType-
# Rasterung zur Laufzeit braucht Fliesskomma/Bezier-Mathematik. Also rastern wir die Glyphen HIER
# mit Pillow (Freetype) EINMAL zu anti-aliased Bitmaps; die GUI blittet sie nur noch (rein integer).
#
# Ausgabe:
#   Fonts/<Familie>/<datei>.ttf         -- die Original-TTFs, organisiert (Nutzerwunsch)
#   Fonts/<Familie>/<style>.rfn         -- das gerasterte Asset (pro Stil)
#   user/lib/font_<slug>.c              -- dasselbe Asset als eingebettetes C-Array (Roman-Stil)
#
# .rfn-Format (little-endian):
#   magic "RFN1" | u16 line_height | u16 ascent | u8 first_char | u8 count | u16 flags(1=AA8)
#   count x Glyph{ s16 advance,w,h,left,ytop,_pad ; u32 data_off }   (16 B je Glyph)
#   dann AA-Daten (w*h Bytes je Glyph, Graustufe 0..255), konkateniert.
#
#   python _build/tools/gen_font.py [--src docs/fonts] [--size 15]

import sys, os, struct, glob

try:
    from PIL import ImageFont
except ImportError:
    print("FEHLER: Pillow (PIL) fehlt -- 'pip install pillow'", file=sys.stderr)
    sys.exit(1)

FIRST = 0x20
LAST  = 0x7E
COUNT = LAST - FIRST + 1

def build_rfn(ttf_path, size):
    font = ImageFont.truetype(ttf_path, size)
    ascent, descent = font.getmetrics()
    line_h = ascent + descent

    glyph_meta = []   # (advance, w, h, left, ytop)
    glyph_bmp  = []   # bytes je Glyph
    for cp in range(FIRST, LAST + 1):
        ch = chr(cp)
        adv = int(round(font.getlength(ch)))
        try:
            mask, (ox, oy) = font.getmask2(ch, mode="L")
            w, h = mask.size
            data = bytes(mask) if (w and h) else b""
        except Exception:
            w = h = ox = oy = 0
            data = b""
        # Sicherheitsklemmung: Datenlaenge muss w*h sein.
        if len(data) != w * h:
            data = (data + b"\x00" * (w * h))[: w * h]
        glyph_meta.append((adv, w, h, ox, oy))
        glyph_bmp.append(data)

    # Header + Glyph-Tabelle + Datenoffsets berechnen.
    header = struct.pack("<4sHHBBH", b"RFN1", line_h, ascent, FIRST, COUNT, 1)
    table_off = len(header)
    data_off  = table_off + COUNT * 16
    off = data_off
    table = b""
    for (adv, w, h, left, ytop), bmp in zip(glyph_meta, glyph_bmp):
        table += struct.pack("<hhhhhhI", adv, w, h, left, ytop, 0, off)
        off += len(bmp)
    blob = header + table + b"".join(glyph_bmp)
    return blob, line_h, ascent

def slug(s):
    return "".join(c.lower() if c.isalnum() else "_" for c in s).strip("_")

# Regular/Buch-Schnitt erkennen: der Standard-Font ist der Schnitt OHNE Bold/Italic/Oblique-Marker
# (deckt "Roman"/"Regular"/"Book" ab, egal wie Datei bzw. Stilname heissen).
def is_regular_face(style, base):
    s = (style or "").lower(); b = base.lower()
    bad = ("bold", "italic", "oblique", "light", "thin", "black",
           "medium", "semib", "condensed")
    return not any(x in s or x in b for x in bad)

def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src  = os.path.join(root, "docs", "fonts")
    size = 15
    i = 1
    while i < len(argv):
        if argv[i] == "--src" and i + 1 < len(argv): src = argv[i + 1]; i += 2
        elif argv[i] == "--size" and i + 1 < len(argv): size = int(argv[i + 1]); i += 2
        else: i += 1

    ttfs = sorted(glob.glob(os.path.join(src, "*.ttf")))
    if not ttfs:
        print(f"FEHLER: keine .ttf in {src}", file=sys.stderr); sys.exit(1)

    # Familienname aus der ersten TTF lesen -> Ordner Fonts/<Familie>/.
    family = ImageFont.truetype(ttfs[0], size).getname()[0] or "Font"
    fam_dir = os.path.join(root, "Fonts", family)
    os.makedirs(fam_dir, exist_ok=True)

    roman_c = None
    for ttf in ttfs:
        base = os.path.basename(ttf)
        style = ImageFont.truetype(ttf, size).getname()[1] or os.path.splitext(base)[0]
        # 1) Original-TTF organisiert ablegen.
        with open(ttf, "rb") as f: raw = f.read()
        with open(os.path.join(fam_dir, base), "wb") as f: f.write(raw)
        # 2) .rfn erzeugen.
        blob, line_h, ascent = build_rfn(ttf, size)
        rfn_name = os.path.splitext(base)[0] + ".rfn"
        with open(os.path.join(fam_dir, rfn_name), "wb") as f: f.write(blob)
        print(f"  {family} / {style}: {len(blob)} B, Zeilenhoehe {line_h}px  -> Fonts/{family}/{rfn_name}")
        # Regular/Buch-Stil zusaetzlich als eingebettetes C-Array (Standard-GUI-Font).
        if roman_c is None and is_regular_face(style, base):
            roman_c = (blob, slug(family))

    if roman_c is None:   # Fallback: erste TTF als Standard
        blob, _, _ = build_rfn(ttfs[0], size)
        roman_c = (blob, slug(family))

    # --- ASCII-Subset der Roman-.ttf fuer die LAUFZEIT-Rasterung (die Originale sind ~14 MB und
    #     passen nicht in den 2-MiB-EL0-Adressraum; das Subset ~9 KB). Landet auf hdd1:GUIFONT.TTF. ---
    roman_ttf = next(
        (t for t in ttfs
         if is_regular_face(ImageFont.truetype(t, size).getname()[1], os.path.basename(t))),
        ttfs[0])
    try:
        from fontTools import subset as _sub
        import io as _io
        sopt = _sub.Options()
        sopt.glyph_names = False
        sopt.notdef_outline = True
        # GSUB/GPOS/... = Layout (ungenutzt); fpgm/prep/cvt/gasp/hdmx/LTSH/VDMX = Hinting-Bytecode
        # (unser Laufzeit-Rasterizer interpretiert keine Hints) -> raus, haelt das Subset klein genug
        # fuer den EL0-Ladepuffer (siehe g_ttf_buf in user/gui.c).
        sopt.drop_tables += ["GSUB", "GPOS", "GDEF", "morx", "kern",
                             "fpgm", "prep", "cvt", "gasp", "hdmx", "LTSH", "VDMX"]
        sopt.hinting = False
        sf = _sub.load_font(roman_ttf, sopt)
        ss = _sub.Subsetter(options=sopt)
        ss.populate(text="".join(chr(c) for c in range(FIRST, LAST + 1)))
        ss.subset(sf)
        sbuf = _io.BytesIO(); _sub.save_font(sf, sbuf, sopt); subttf = sbuf.getvalue()
        with open(os.path.join(root, "user", "gui_font.ttf"), "wb") as f: f.write(subttf)
        with open(os.path.join(fam_dir, "roman_ascii_subset.ttf"), "wb") as f: f.write(subttf)
        print(f"  Laufzeit-Subset (ASCII) -> user/gui_font.ttf ({len(subttf)} B)")
    except ImportError:
        print("  (fontTools fehlt -> kein Laufzeit-Subset erzeugt; 'pip install fonttools')")

    blob, name = roman_c
    out_c = os.path.join(root, "user", "lib", f"font_{name}.c")
    with open(out_c, "w") as f:
        f.write("/* AUTO-GENERIERT von tools/gen_font.py -- nicht von Hand aendern. */\n")
        f.write("#include <stddef.h>\n\n")
        f.write(f"const unsigned char font_{name}[] = {{\n")
        for j in range(0, len(blob), 16):
            f.write("    " + ",".join(str(b) for b in blob[j:j + 16]) + ",\n")
        f.write("};\n")
        f.write(f"const unsigned int font_{name}_len = {len(blob)}u;\n")
    print(f"  eingebettet -> {os.path.relpath(out_c, root)} ({len(blob)} B)")
    print(f"OK: Font-Familie '{family}' generiert (Groesse {size}px).")

if __name__ == "__main__":
    main(sys.argv)
