/*
 * user/lib/gui_ttf.c  --  Laufzeit-TrueType-Rasterung fuer die GUI
 *
 * DIESE Datei wird als EINZIGE OHNE -mgeneral-regs-only kompiliert (mit FP/SIMD) und NUR an GUI.ELF
 * gelinkt. Sie laeuft nur in GUI-Builds, in denen der Kernel CPACR_EL1.FPEN=0b11 setzt (-DGUI_FP).
 * Weil sonst niemand (Kernel + andere Apps sind integer-only) die V-Register anfasst, ueberlebt der
 * FP-Zustand der GUI die Preemption ohne Kontext-Save/Restore.
 *
 * Parst eine (mit fontTools auf ASCII gesubsettete) .ttf und rastert die Glyphen zur Laufzeit in
 * BELIEBIGER Groesse: SFNT-Tabellen (head/maxp/hhea/cmap/loca/glyf/hmtx), Quadratik-Bezier-Konturen
 * flatten, 4x-Supersampling-Scanline-Fill (nonzero winding) -> anti-aliased 8-bit-Glyphen. Ausgabe im
 * .rfn-kompatiblen Layout, sodass gui_text_ttf() sie unveraendert blittet.
 */
#include "gui.h"

/* Beweist EL0-FP: eine Float-Rechnung. Erwartet: (3.14159*2 + 1.5) * 100 = 778.318 -> 778.
 * Traeppt FP (FPEN!=0b11), faultet der Prozess vorher -> der Aufrufer sieht kein Ergebnis. */
int gui_ttf_fp_smoke(void)
{
    volatile float a = 3.14159f, b = 2.0f;
    float c = a * b + 1.5f;
    return (int)(c * 100.0f);
}

/* --- Big-Endian-Leser fuer die .ttf --- */
static unsigned be16u(const unsigned char *p) { return (unsigned)((p[0] << 8) | p[1]); }
static int      be16s(const unsigned char *p) { return (int)(short)(unsigned short)((p[0] << 8) | p[1]); }
static unsigned be32u(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

static int ifloorf(float x) { int i = (int)x; return (x < (float)i) ? i - 1 : i; }
static int iceilf(float x)  { int i = (int)x; return (x > (float)i) ? i + 1 : i; }
static int iroundf(float x) { return ifloorf(x + 0.5f); }

/* Tabelle im SFNT-Verzeichnis finden -> Offset (oder 0). */
static unsigned find_table(const unsigned char *ttf, unsigned len, const char *tag)
{
    if (len < 12u) { return 0; }
    unsigned num = be16u(ttf + 4);
    for (unsigned i = 0; i < num; i++) {
        const unsigned char *rec = ttf + 12 + i * 16;
        if ((unsigned)(rec - ttf) + 16u > len) { break; }
        if (rec[0] == (unsigned char)tag[0] && rec[1] == (unsigned char)tag[1] &&
            rec[2] == (unsigned char)tag[2] && rec[3] == (unsigned char)tag[3]) {
            return be32u(rec + 8);
        }
    }
    return 0;
}

/* cmap-Format-4-Lookup: Unicode-Codepoint -> Glyph-ID (0, wenn nicht gefunden). */
static unsigned cmap4_gid(const unsigned char *sub, unsigned c)
{
    unsigned segX2 = be16u(sub + 6);
    unsigned segc  = segX2 / 2;
    const unsigned char *endC   = sub + 14;
    const unsigned char *startC = endC + segX2 + 2;
    const unsigned char *idD    = startC + segX2;
    const unsigned char *idR    = idD + segX2;
    for (unsigned i = 0; i < segc; i++) {
        if (c <= be16u(endC + i * 2)) {
            unsigned start = be16u(startC + i * 2);
            if (c < start) { return 0; }
            int delta = be16s(idD + i * 2);
            unsigned ro = be16u(idR + i * 2);
            if (ro == 0) { return (unsigned)((int)c + delta) & 0xFFFFu; }
            const unsigned char *gp = idR + i * 2 + ro + (c - start) * 2;
            unsigned g = be16u(gp);
            return g ? ((unsigned)((int)g + delta) & 0xFFFFu) : 0;
        }
    }
    return 0;
}

/* --- Rasterizer-Puffer (statisch, EL0-bss) --- */
#define TTF_MAXP   1024        /* Punkte je Glyph */
#define TTF_MAXC   64          /* Konturen je Glyph */
#define TTF_MAXE   4096        /* geflattete Kanten je Glyph */
#define TTF_SS     4           /* Supersampling-Faktor */
#define TTF_MAXGPX 56          /* max. Glyph-Kantenlaenge in Pixeln */
#define TTF_MASK   (TTF_MAXGPX * TTF_SS)
#define TTF_MAXX   128         /* max. Kreuzungen je Scanline */

static int   g_gx[TTF_MAXP], g_gy[TTF_MAXP];
static unsigned char g_onc[TTF_MAXP], g_flag[TTF_MAXP];
static float g_mx[TTF_MAXP], g_my[TTF_MAXP];
static int   g_cend[TTF_MAXC];
static float g_ex0[TTF_MAXE], g_ey0[TTF_MAXE], g_ex1[TTF_MAXE], g_ey1[TTF_MAXE];
static int   g_ne;
static unsigned char g_mask[TTF_MASK * TTF_MASK];

static void emit_edge(float x0, float y0, float x1, float y1)
{
    if (g_ne >= TTF_MAXE || y0 == y1) { return; }     /* horizontale Kanten tragen nicht bei */
    g_ex0[g_ne] = x0; g_ey0[g_ne] = y0; g_ex1[g_ne] = x1; g_ey1[g_ne] = y1; g_ne++;
}

/* Quadratik-Bezier p0->(cx,cy)->p1 in N Segmente flatten (Mask-Koordinaten). */
static void emit_quad(float x0, float y0, float cx, float cy, float x1, float y1)
{
    const int N = 8;
    float px = x0, py = y0;
    for (int i = 1; i <= N; i++) {
        float t = (float)i / (float)N, u = 1.0f - t;
        float x = u * u * x0 + 2.0f * u * t * cx + t * t * x1;
        float y = u * u * y0 + 2.0f * u * t * cy + t * t * y1;
        emit_edge(px, py, x, y);
        px = x; py = y;
    }
}

/* Eine Kontur [s, e] (Mask-Koordinaten in g_mx/g_my/onc) zu Kanten flatten. */
static void flatten_contour(const float *mx, const float *my, int s, int e)
{
    int n = e - s + 1;
    if (n < 2) { return; }
    /* Startpunkt (on-curve) finden; ggf. Mittelpunkt zweier off-curve als virtuellen Start. */
    int startIdx = -1;
    for (int i = 0; i < n; i++) { if (g_onc[s + i]) { startIdx = i; break; } }
    float ax, ay;      /* aktueller Anker (on-curve) */
    int haveCtrl = 0; float cxp = 0, cyp = 0;
    int begin;
    if (startIdx < 0) {                               /* keine on-curve -> Mittelpunkt p0/pn-1 */
        ax = (mx[s] + mx[e]) * 0.5f; ay = (my[s] + my[e]) * 0.5f; begin = 0;
    } else {
        ax = mx[s + startIdx]; ay = my[s + startIdx]; begin = startIdx + 1;
    }
    float sx = ax, sy = ay;                            /* Konturschluss */
    for (int k = 0; k <= n; k++) {
        int idx = s + ((begin + k) % n);
        int last = (k == n);
        float px = last ? sx : mx[idx];
        float py = last ? sy : my[idx];
        int on   = last ? 1  : (int)g_onc[idx];
        if (on) {
            if (haveCtrl) { emit_quad(ax, ay, cxp, cyp, px, py); haveCtrl = 0; }
            else          { emit_edge(ax, ay, px, py); }
            ax = px; ay = py;
        } else {
            if (haveCtrl) {                            /* zwei off-curve -> impliziter Mittelpunkt */
                float mxi = (cxp + px) * 0.5f, myi = (cyp + py) * 0.5f;
                emit_quad(ax, ay, cxp, cyp, mxi, myi);
                ax = mxi; ay = myi;
            }
            cxp = px; cyp = py; haveCtrl = 1;
        }
    }
}

/* g_mask (mw x mh) per nonzero-winding-Scanline fuellen. */
static void fill_mask(int mw, int mh)
{
    for (int i = 0; i < mw * mh; i++) { g_mask[i] = 0; }
    for (int my = 0; my < mh; my++) {
        float ys = (float)my + 0.5f;
        float xs[TTF_MAXX]; int dir[TTF_MAXX]; int nc = 0;
        for (int i = 0; i < g_ne && nc < TTF_MAXX; i++) {
            float y0 = g_ey0[i], y1 = g_ey1[i];
            int down = (y1 > y0);
            float lo = down ? y0 : y1, hi = down ? y1 : y0;
            if (ys < lo || ys >= hi) { continue; }
            float t = (ys - g_ey0[i]) / (y1 - y0);
            xs[nc]  = g_ex0[i] + t * (g_ex1[i] - g_ex0[i]);
            dir[nc] = down ? 1 : -1;
            nc++;
        }
        /* nach x sortieren (Insertion) */
        for (int i = 1; i < nc; i++) {
            float xv = xs[i]; int dv = dir[i]; int j = i - 1;
            while (j >= 0 && xs[j] > xv) { xs[j + 1] = xs[j]; dir[j + 1] = dir[j]; j--; }
            xs[j + 1] = xv; dir[j + 1] = dv;
        }
        int wind = 0;
        unsigned char *row = g_mask + my * mw;
        for (int i = 0; i < nc - 1; i++) {
            wind += dir[i];
            if (wind != 0) {
                int xa = iceilf(xs[i] - 0.5f), xb = ifloorf(xs[i + 1] - 0.5f);
                if (xa < 0) { xa = 0; }
                if (xb >= mw) { xb = mw - 1; }
                for (int x = xa; x <= xb; x++) { row[x] = 1; }
            }
        }
    }
}

/* Simple-Glyph decoden -> g_gx/g_gy (font units), g_onc, g_cend, pnc + pnp. 0 = ok. */
static int decode_glyph(const unsigned char *g, int *pnc, int *pnp)
{
    int nc = be16s(g);
    if (nc <= 0 || nc > TTF_MAXC) { return -1; }      /* leer oder composite -> nicht gestuetzt */
    const unsigned char *p = g + 10;
    for (int i = 0; i < nc; i++) { g_cend[i] = (int)be16u(p + i * 2); }
    int np = g_cend[nc - 1] + 1;
    if (np <= 0 || np > TTF_MAXP) { return -1; }
    p += nc * 2;
    unsigned insLen = be16u(p); p += 2 + insLen;      /* Hinting-Instruktionen ueberspringen */
    for (int i = 0; i < np; ) {                        /* Flags (mit Wiederholung) */
        unsigned char fl = *p++;
        g_flag[i] = fl; g_onc[i] = (unsigned char)(fl & 1u); i++;
        if (fl & 0x08u) { int rep = *p++; while (rep-- > 0 && i < np) { g_flag[i] = fl; g_onc[i] = (unsigned char)(fl & 1u); i++; } }
    }
    int xa = 0;                                        /* x-Deltas */
    for (int i = 0; i < np; i++) {
        unsigned char fl = g_flag[i]; int dx = 0;
        if (fl & 0x02) { dx = *p++; if (!(fl & 0x10)) { dx = -dx; } }
        else if (!(fl & 0x10)) { dx = be16s(p); p += 2; }
        xa += dx; g_gx[i] = xa;
    }
    int ya = 0;                                        /* y-Deltas */
    for (int i = 0; i < np; i++) {
        unsigned char fl = g_flag[i]; int dy = 0;
        if (fl & 0x04) { dy = *p++; if (!(fl & 0x20)) { dy = -dy; } }
        else if (!(fl & 0x20)) { dy = be16s(p); p += 2; }
        ya += dy; g_gy[i] = ya;
    }
    *pnc = nc; *pnp = np;
    return 0;
}

/* Glyph rastern -> AA-Bitmap out[w*h]. scale + Pixel-bbox-Ursprung (xlo,ylo). */
static void raster_glyph(const unsigned char *g, float scale, int xlo, int ylo, int w, int h, unsigned char *out)
{
    for (int i = 0; i < w * h; i++) { out[i] = 0; }
    int nc, np;
    if (decode_glyph(g, &nc, &np) != 0) { return; }
    int mw = w * TTF_SS, mh = h * TTF_SS;
    if (mw <= 0 || mh <= 0 || mw > TTF_MASK || mh > TTF_MASK) { return; }
    for (int i = 0; i < np; i++) {                     /* Punkte -> Mask-Koordinaten (y nach unten) */
        g_mx[i] = ((float)g_gx[i] * scale - (float)xlo) * (float)TTF_SS;
        g_my[i] = ((float)(-g_gy[i]) * scale - (float)ylo) * (float)TTF_SS;
    }
    g_ne = 0;
    int s = 0;
    for (int c = 0; c < nc; c++) { flatten_contour(g_mx, g_my, s, g_cend[c]); s = g_cend[c] + 1; }
    fill_mask(mw, mh);
    for (int oy = 0; oy < h; oy++) {                   /* SSxSS -> AA-Byte */
        for (int ox = 0; ox < w; ox++) {
            int cnt = 0;
            for (int sy = 0; sy < TTF_SS; sy++) {
                const unsigned char *row = g_mask + (oy * TTF_SS + sy) * mw + ox * TTF_SS;
                for (int sx = 0; sx < TTF_SS; sx++) { cnt += row[sx]; }
            }
            out[oy * w + ox] = (unsigned char)((cnt * 255) / (TTF_SS * TTF_SS));
        }
    }
}

/* --- Little-Endian-Schreiber fuer das .rfn-Layout --- */
static void put16(unsigned char *b, unsigned v) { b[0] = (unsigned char)(v & 0xFF); b[1] = (unsigned char)((v >> 8) & 0xFF); }
static void put32(unsigned char *b, unsigned v) { b[0] = (unsigned char)(v & 0xFF); b[1] = (unsigned char)((v >> 8) & 0xFF); b[2] = (unsigned char)((v >> 16) & 0xFF); b[3] = (unsigned char)((v >> 24) & 0xFF); }

/* Eine .ttf im Speicher zur Laufzeit auf ASCII (0x20..0x7E) in Pixelhoehe pixel_em rastern und als
 * .rfn ins arena schreiben; font wird darauf gebunden. 0 = ok, -1 = ungueltig / zu klein. */
int gui_font_rasterize_ttf(gui_font_t *font, const unsigned char *ttf, unsigned ttf_len, int pixel_em,
                           unsigned char *arena, unsigned arena_size)
{
    unsigned head = find_table(ttf, ttf_len, "head"), maxp = find_table(ttf, ttf_len, "maxp");
    unsigned hhea = find_table(ttf, ttf_len, "hhea"), hmtx = find_table(ttf, ttf_len, "hmtx");
    unsigned loca = find_table(ttf, ttf_len, "loca"), glyf = find_table(ttf, ttf_len, "glyf");
    unsigned cmap = find_table(ttf, ttf_len, "cmap");
    if (!head || !maxp || !hhea || !hmtx || !loca || !glyf || !cmap) { return -1; }
    unsigned upm = be16u(ttf + head + 18);
    int locfmt = be16s(ttf + head + 50);
    int ascent = be16s(ttf + hhea + 4), descent = be16s(ttf + hhea + 6);
    unsigned numHM = be16u(ttf + hhea + 34), numG = be16u(ttf + maxp + 4);
    if (upm == 0 || pixel_em <= 0) { return -1; }
    float scale = (float)pixel_em / (float)upm;
    int ascent_px = iroundf((float)ascent * scale);
    int line_h = ascent_px + iroundf((float)(-descent) * scale);

    unsigned nsub = be16u(ttf + cmap + 2), sub4 = 0;   /* erste cmap-Format-4-Subtable */
    for (unsigned i = 0; i < nsub; i++) {
        unsigned off = be32u(ttf + cmap + 4 + i * 8 + 4);
        if (cmap + off + 2u <= ttf_len && be16u(ttf + cmap + off) == 4) { sub4 = cmap + off; break; }
    }
    if (!sub4) { return -1; }

    const int FIRST = 0x20, COUNT = 0x7F - 0x20;       /* 95 Zeichen */
    unsigned dataoff = 12u + (unsigned)COUNT * 16u;
    if (dataoff > arena_size) { return -1; }
    arena[0] = 'R'; arena[1] = 'F'; arena[2] = 'N'; arena[3] = '1';
    put16(arena + 4, (unsigned)line_h); put16(arena + 6, (unsigned)ascent_px);
    arena[8] = (unsigned char)FIRST; arena[9] = (unsigned char)COUNT; put16(arena + 10, 1);

    for (int ci = 0; ci < COUNT; ci++) {
        unsigned char *te = arena + 12 + ci * 16;
        unsigned gid = cmap4_gid(ttf + sub4, (unsigned)(FIRST + ci));
        int adv = (gid < numHM) ? (int)be16u(ttf + hmtx + gid * 4)
                                : (numHM ? (int)be16u(ttf + hmtx + (numHM - 1) * 4) : 0);
        int adv_px = iroundf((float)adv * scale);
        unsigned go, gnext;
        if (locfmt == 0) { go = be16u(ttf + loca + gid * 2) * 2u; gnext = be16u(ttf + loca + (gid + 1) * 2) * 2u; }
        else             { go = be32u(ttf + loca + gid * 4);      gnext = be32u(ttf + loca + (gid + 1) * 4); }
        int w = 0, h = 0, left = 0, ytop = 0;
        if (gid < numG && gnext > go && glyf + go + 10u <= ttf_len) {
            const unsigned char *g = ttf + glyf + go;
            int xMin = be16s(g + 2), yMin = be16s(g + 4), xMax = be16s(g + 6), yMax = be16s(g + 8);
            int xlo = ifloorf((float)xMin * scale), xhi = iceilf((float)xMax * scale);
            int ylo = ifloorf((float)(-yMax) * scale), yhi = iceilf((float)(-yMin) * scale);
            w = xhi - xlo; h = yhi - ylo; left = xlo; ytop = ascent_px + ylo;
            if (w < 0) { w = 0; } if (h < 0) { h = 0; }
            if (w > TTF_MAXGPX) { w = TTF_MAXGPX; } if (h > TTF_MAXGPX) { h = TTF_MAXGPX; }
            if (w > 0 && h > 0) {
                if (dataoff + (unsigned)(w * h) > arena_size) { w = 0; h = 0; }
                else { raster_glyph(g, scale, xlo, ylo, w, h, arena + dataoff); }
            }
        }
        put16(te + 0, (unsigned)adv_px); put16(te + 2, (unsigned)w); put16(te + 4, (unsigned)h);
        put16(te + 6, (unsigned)left);   put16(te + 8, (unsigned)ytop); put16(te + 10, 0);
        put32(te + 12, dataoff);
        dataoff += (unsigned)(w * h);
    }
    return gui_font_from_mem(font, arena, dataoff);
}
