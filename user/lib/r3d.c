/*
 * user/lib/r3d.c  --  Software-3D-Rasterizer MIT FP kompiliert.
 *
 * Pipeline je Dreieck (Vulkan-Konventionen, siehe r3d.h):
 *   Clip-Space -> Sutherland-Hodgman gegen NEAR (z>=0) und FAR (z<=w) -> perspektivische
 *   Division -> Viewport (y-down) -> 28.4-Festkomma-Snap -> Facing/Culling -> exakte
 *   Edge-Function-Rasterung mit Top-Left-Fill-Rule -> Depth-Test LESS -> perspektivisch
 *   korrekte Attribut-Interpolation -> Fragment-Farbe (Callback oder Gouraud-Pack).
 *
 * Exaktheit: Vertex-Positionen werden auf 1/16 Pixel gerastert; Kantenfunktionen sind
 * 64-bit-Ganzzahlen an Pixelzentren (16*px+8) -> die Innen-/Aussen-Entscheidung ist
 * deterministisch und geteilte Kanten gehoeren per Top-Left-Regel GENAU EINEM Dreieck
 * (wasserdicht, keine Doppel-Fragmente -- wichtig fuer spaetere Blend-Operationen).
 */
#include "r3d.h"

/* ---------------- Trigonometrie (ohne libm) ---------------- */
#define R3D_PI  3.14159265358979f
#define R3D_2PI 6.28318530717959f

float r3d_sinf(float x)
{
    while (x >  R3D_PI) { x -= R3D_2PI; }
    while (x < -R3D_PI) { x += R3D_2PI; }
    if (x >  0.5f * R3D_PI) { x =  R3D_PI - x; }      /* sin(pi-x) = sin(x) */
    if (x < -0.5f * R3D_PI) { x = -R3D_PI - x; }
    float x2 = x * x;
    /* Taylor bis x^9: |err| < 1e-5 auf [-pi/2, pi/2] */
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
                x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

float r3d_cosf(float x) { return r3d_sinf(x + 0.5f * R3D_PI); }

/* ---------------- Matrix/Vektor (Column-Major) ---------------- */
void r3d_mat4_identity(r3d_mat4 *out)
{
    for (int i = 0; i < 16; i++) { out->m[i] = 0.0f; }
    out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f;
}

void r3d_mat4_mul(r3d_mat4 *out, const r3d_mat4 *a, const r3d_mat4 *b)
{
    r3d_mat4 r;                                  /* lokale Kopie: out darf a/b aliasen */
    for (int c = 0; c < 4; c++) {
        for (int rw = 0; rw < 4; rw++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) { s += a->m[k * 4 + rw] * b->m[c * 4 + k]; }
            r.m[c * 4 + rw] = s;
        }
    }
    for (int i = 0; i < 16; i++) { out->m[i] = r.m[i]; }
}

void r3d_mat4_mul_vec4(r3d_vec4 *out, const r3d_mat4 *m, const r3d_vec4 *v)
{
    float x = v->x, y = v->y, z = v->z, w = v->w;  /* out darf v aliasen */
    out->x = m->m[0] * x + m->m[4] * y + m->m[8]  * z + m->m[12] * w;
    out->y = m->m[1] * x + m->m[5] * y + m->m[9]  * z + m->m[13] * w;
    out->z = m->m[2] * x + m->m[6] * y + m->m[10] * z + m->m[14] * w;
    out->w = m->m[3] * x + m->m[7] * y + m->m[11] * z + m->m[15] * w;
}

void r3d_mat4_rot_x(r3d_mat4 *out, float rad)
{
    float s = r3d_sinf(rad), c = r3d_cosf(rad);
    r3d_mat4_identity(out);
    out->m[5] = c;  out->m[9]  = -s;
    out->m[6] = s;  out->m[10] = c;
}

void r3d_mat4_rot_y(r3d_mat4 *out, float rad)
{
    float s = r3d_sinf(rad), c = r3d_cosf(rad);
    r3d_mat4_identity(out);
    out->m[0] = c;   out->m[8]  = s;
    out->m[2] = -s;  out->m[10] = c;
}

void r3d_mat4_translate(r3d_mat4 *out, float x, float y, float z)
{
    r3d_mat4_identity(out);
    out->m[12] = x; out->m[13] = y; out->m[14] = z;
}

void r3d_mat4_perspective(r3d_mat4 *out, float fovy_rad, float aspect, float znear, float zfar)
{
    float half = 0.5f * fovy_rad;
    float f = r3d_cosf(half) / r3d_sinf(half);     /* cot(fovy/2) */
    for (int i = 0; i < 16; i++) { out->m[i] = 0.0f; }
    out->m[0]  = f / aspect;
    out->m[5]  = -f;                               /* Vulkan: +Y (Welt) -> oben auf y-down-FB */
    out->m[10] = zfar / (znear - zfar);            /* z_ndc in [0,1]: near->0, far->1 */
    out->m[11] = -1.0f;
    out->m[14] = (znear * zfar) / (znear - zfar);
}

void r3d_depth_clear(float *depth, int count, float value)
{
    for (int i = 0; i < count; i++) { depth[i] = value; }
}

/* Tiefen-Vergleich nach VkCompareOp (z = eingehend, stored = im Depth-Puffer). 1 = bestanden. */
static int r3d_depth_pass(int op, float z, float stored)
{
    switch (op) {
    case 0: return 0;                 /* NEVER */
    case 1: return z <  stored;       /* LESS (Default) */
    case 2: return z == stored;       /* EQUAL */
    case 3: return z <= stored;       /* LESS_OR_EQUAL */
    case 4: return z >  stored;       /* GREATER */
    case 5: return z != stored;       /* NOT_EQUAL */
    case 6: return z >= stored;       /* GREATER_OR_EQUAL */
    default: return 1;                /* ALWAYS (7) */
    }
}

/* ---------------- Clipping (homogen, Sutherland-Hodgman) ---------------- */
static void lerp_vtx(r3d_vtx_t *out, const r3d_vtx_t *v0, const r3d_vtx_t *v1, float t, int nattr)
{
    out->pos.x = v0->pos.x + (v1->pos.x - v0->pos.x) * t;
    out->pos.y = v0->pos.y + (v1->pos.y - v0->pos.y) * t;
    out->pos.z = v0->pos.z + (v1->pos.z - v0->pos.z) * t;
    out->pos.w = v0->pos.w + (v1->pos.w - v0->pos.w) * t;
    for (int i = 0; i < nattr; i++) {
        out->attr[i] = v0->attr[i] + (v1->attr[i] - v0->attr[i]) * t;
    }
}

/* Clip-Ebenen. NEAR/FAR sind Vulkan-Pflicht (0 <= z <= w). Die GUARD-BAND-Ebenen
 * (|x| <= G*w, |y| <= G*w, w >= eps) begrenzen zusaetzlich die FENSTER-Koordinaten:
 * ohne x/y-Clipping koennten weit ausserhalb liegende Ecken (oder w -> 0+) beliebig
 * grosse/inf Screen-Koordinaten liefern -> float->int-Cast in snap4 waere UB und die
 * 64-bit-Kantenfunktionen koennten ueberlaufen. Mit G=8 ist |sx| <= ~9*Breite ->
 * 28.4-Werte < 2^16, Kreuzprodukte < 2^34: alles satt im int64/float-Bereich.
 * Sichtbares Ergebnis unveraendert (Band >> Viewport; Vulkan clippt x/y ohnehin). */
#define R3D_GUARD 8.0f
#define R3D_WMIN  1e-6f
enum { CP_NEAR = 0, CP_FAR, CP_WMIN, CP_XMIN, CP_XMAX, CP_YMIN, CP_YMAX };

static float plane_dist(const r3d_vtx_t *v, int plane)
{
    switch (plane) {
    case CP_NEAR: return v->pos.z;                                  /* z >= 0        */
    case CP_FAR:  return v->pos.w - v->pos.z;                       /* z <= w        */
    case CP_WMIN: return v->pos.w - R3D_WMIN;                       /* w >= eps      */
    case CP_XMIN: return v->pos.x + R3D_GUARD * v->pos.w;           /* x >= -G*w     */
    case CP_XMAX: return R3D_GUARD * v->pos.w - v->pos.x;           /* x <=  G*w     */
    case CP_YMIN: return v->pos.y + R3D_GUARD * v->pos.w;           /* y >= -G*w     */
    default:      return R3D_GUARD * v->pos.w - v->pos.y;           /* y <=  G*w     */
    }
}

/* Polygon 'in' (n Ecken) gegen Ebene 'plane' clippen (Sutherland-Hodgman, homogen VOR
 * der Division -> Schnittpunkte auch bei w<=0-Ecken korrekt). Liefert neue Eckenzahl. */
static int clip_plane(r3d_vtx_t *out, const r3d_vtx_t *in, int n, int nattr, int plane)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const r3d_vtx_t *cur = &in[i];
        const r3d_vtx_t *nxt = &in[(i + 1) % n];
        float dc = plane_dist(cur, plane);
        float dn = plane_dist(nxt, plane);
        int ic = (dc >= 0.0f), in2 = (dn >= 0.0f);
        if (ic) { out[m++] = *cur; }
        if (ic != in2) { lerp_vtx(&out[m++], cur, nxt, dc / (dc - dn), nattr); }
    }
    return m;
}

/* ---------------- Projektion + Rasterung ---------------- */
typedef struct {
    float sx, sy;                 /* Fenster-Koordinaten (Pixel, float) */
    float z;                      /* Tiefe nach Viewport ([minz,maxz])  */
    float iw;                     /* 1/w_clip                            */
    float aow[R3D_MAX_ATTR];      /* attr/w (perspektivische Korrektur)  */
    int   X, Y;                   /* 28.4-Festkomma-Fensterkoordinaten   */
} svtx_t;

static int snap4(float v)         /* auf 1/16 Pixel runden */
{
    return (int)(v * 16.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

static void project(const r3d_target_t *t, const r3d_vtx_t *v, svtx_t *s, int nattr)
{
    float iw = 1.0f / v->pos.w;
    float nx = v->pos.x * iw, ny = v->pos.y * iw, nz = v->pos.z * iw;
    s->sx = t->vp_x + (nx * 0.5f + 0.5f) * t->vp_w;
    s->sy = t->vp_y + (ny * 0.5f + 0.5f) * t->vp_h;      /* y-down */
    s->z  = t->vp_minz + nz * (t->vp_maxz - t->vp_minz);
    s->iw = iw;
    for (int i = 0; i < nattr; i++) { s->aow[i] = v->attr[i] * iw; }
    s->X = snap4(s->sx);
    s->Y = snap4(s->sy);
}

static unsigned pack_rgb(float r, float g, float b)
{
    int ri = (int)(r * 255.0f + 0.5f);
    int gi = (int)(g * 255.0f + 0.5f);
    int bi = (int)(b * 255.0f + 0.5f);
    if (ri < 0) { ri = 0; } if (ri > 255) { ri = 255; }
    if (gi < 0) { gi = 0; } if (gi > 255) { gi = 255; }
    if (bi < 0) { bi = 0; } if (bi > 255) { bi = 255; }
    return ((unsigned)ri << 16) | ((unsigned)gi << 8) | (unsigned)bi;
}

static int imin3(int a, int b, int c) { int m = a; if (b < m) { m = b; } if (c < m) { m = c; } return m; }
static int imax3(int a, int b, int c) { int m = a; if (b > m) { m = b; } if (c > m) { m = c; } return m; }

/* Ein (bereits projiziertes) Dreieck rastern. Facing/Culling/Fill-Rule wie in r3d.h. */
static void raster_tri(const r3d_target_t *t, const svtx_t *v0, const svtx_t *v1, const svtx_t *v2,
                       int nattr, r3d_fs_fn fs, void *user)
{
    /* Doppelte vorzeichenbehaftete Flaeche in 24.8 (Kreuzprodukt zweier 28.4-Vektoren).
     * In y-down-Fensterkoordinaten gilt: area2 > 0 <=> Eckenfolge laeuft am Schirm
     * IM UHRZEIGERSINN ("clockwise" im Sinne von VkFrontFace). */
    long long area2 = (long long)(v1->X - v0->X) * (v2->Y - v0->Y)
                    - (long long)(v1->Y - v0->Y) * (v2->X - v0->X);
    if (area2 == 0) { return; }                                  /* degeneriert */

    int is_ccw = (area2 < 0);
    int front  = t->front_ccw ? is_ccw : !is_ccw;
    if (t->cull_mode == R3D_CULL_BACK  && !front) { return; }
    if (t->cull_mode == R3D_CULL_FRONT &&  front) { return; }

    /* Fuer die Rasterung positiv orientieren (Innen-Test: alle Kanten >= 0). */
    if (area2 < 0) { const svtx_t *tmp = v1; v1 = v2; v2 = tmp; area2 = -area2; }

    /* Raster-Region: Bounding-Box ∩ Viewport ∩ Scissor ∩ Target (Guard-Band-Prinzip:
     * x/y-Klemmung ersetzt das x/y-Clipping im Clip-Space -- sichtbares Ergebnis identisch). */
    int rx0 = t->sc_x,             ry0 = t->sc_y;
    int rx1 = t->sc_x + t->sc_w,   ry1 = t->sc_y + t->sc_h;
    int vx0 = (int)t->vp_x,        vy0 = (int)t->vp_y;
    int vx1 = (int)(t->vp_x + t->vp_w), vy1 = (int)(t->vp_y + t->vp_h);
    if (rx0 < vx0) { rx0 = vx0; }  if (ry0 < vy0) { ry0 = vy0; }
    if (rx1 > vx1) { rx1 = vx1; }  if (ry1 > vy1) { ry1 = vy1; }
    if (rx0 < 0) { rx0 = 0; }      if (ry0 < 0) { ry0 = 0; }
    if (rx1 > t->width)  { rx1 = t->width;  }
    if (ry1 > t->height) { ry1 = t->height; }

    int minX = imin3(v0->X, v1->X, v2->X), maxX = imax3(v0->X, v1->X, v2->X);
    int minY = imin3(v0->Y, v1->Y, v2->Y), maxY = imax3(v0->Y, v1->Y, v2->Y);
    /* Pixel px ist erfasst, wenn sein Zentrum 16*px+8 in [minX, maxX] liegt. */
    int px0 = (minX - 8 + 15) >> 4, px1 = (maxX - 8) >> 4;
    int py0 = (minY - 8 + 15) >> 4, py1 = (maxY - 8) >> 4;
    if (px0 < rx0) { px0 = rx0; }  if (py0 < ry0) { py0 = ry0; }
    if (px1 > rx1 - 1) { px1 = rx1 - 1; }
    if (py1 > ry1 - 1) { py1 = ry1 - 1; }
    if (px0 > px1 || py0 > py1) { return; }

    /* Kantenfunktionen: E_i(p) >= 0 fuer innen; Kante i liegt GEGENUEBER Ecke i
     * (E0: v1->v2 wichtet v0, E1: v2->v0 wichtet v1, E2: v0->v1 wichtet v2). */
    const svtx_t *ep[3] = { v1, v2, v0 };
    const svtx_t *eq[3] = { v2, v0, v1 };
    long long A[3], B[3], E[3], bias[3];
    for (int i = 0; i < 3; i++) {
        long long pX = ep[i]->X, pY = ep[i]->Y, qX = eq[i]->X, qY = eq[i]->Y;
        A[i] = pY - qY;                          /* dE / d(sub-x) */
        B[i] = qX - pX;                          /* dE / d(sub-y) */
        /* Top-Left-Regel (positiv orientiert, y-down): "top" = horizontale Kante mit
         * Innerem darunter (dy==0 && dx>0), "left" = Kante laeuft nach oben (dy<0). */
        long long dx = qX - pX, dy = qY - pY;
        bias[i] = ((dy == 0 && dx > 0) || dy < 0) ? 0 : -1;
        /* Startwert am Zentrum des ersten Pixels. */
        long long sx0 = 16LL * px0 + 8, sy0 = 16LL * py0 + 8;
        E[i] = A[i] * (sx0 - pX) + B[i] * (sy0 - pY);
    }

    float inv_area = 1.0f / (float)area2;

    /* V1.6 MSAA: 4 Sample-Positionen (1/16-Subpixel, relativ zum Pixelzentrum). Standard-
     * gedrehtes Gitter. E an einem Sample = E_zentrum + A*ox + B*oy. */
    static const int msox[4] = { -2,  6, -6,  2 };
    static const int msoy[4] = { -6, -2,  6,  2 };
    int ns = (t->samples > 1) ? 4 : 1;
    long long splane = (long long)t->height * t->pitch_px;   /* Wort-Abstand color-Sample-Ebene */
    long long dplane = (long long)t->height * t->width;      /* Float-Abstand depth-Sample-Ebene */

    for (int py = py0; py <= py1; py++) {
        long long e0 = E[0], e1 = E[1], e2 = E[2];
        unsigned *crow = t->color + (long long)py * t->pitch_px;
        float    *drow = t->depth ? t->depth + (long long)py * t->width : 0;
        for (int px = px0; px <= px1; px++) {
            if (ns == 1) {
                /* ---- Single-Sample (unveraendert) ---- */
                if ((e0 + bias[0]) >= 0 && (e1 + bias[1]) >= 0 && (e2 + bias[2]) >= 0) {
                    float l0 = (float)e0 * inv_area;
                    float l1 = (float)e1 * inv_area;
                    float l2 = (float)e2 * inv_area;
                    float z = l0 * v0->z + l1 * v1->z + l2 * v2->z;
                    if (t->depth_test && drow) {
                        if (!r3d_depth_pass(t->depth_compare, z, drow[px])) { goto next_px; }
                        if (t->depth_write) { drow[px] = z; }
                    }
                    float iw = l0 * v0->iw + l1 * v1->iw + l2 * v2->iw;
                    float wr = 1.0f / iw;
                    float attr[R3D_MAX_ATTR];
                    for (int i = 0; i < nattr; i++) {
                        attr[i] = (l0 * v0->aow[i] + l1 * v1->aow[i] + l2 * v2->aow[i]) * wr;
                    }
                    crow[px] = fs ? fs(user, px, py, z, attr)
                                  : pack_rgb(attr[0], attr[1], attr[2]);
                }
            } else {
                /* ---- 4x MSAA: Coverage + Tiefe JE Sample; FS EINMAL je Pixel (am Zentrum). ---- */
                unsigned col = 0; int shaded = 0;
                for (int s = 0; s < ns; s++) {
                    long long se0 = e0 + A[0] * msox[s] + B[0] * msoy[s];
                    long long se1 = e1 + A[1] * msox[s] + B[1] * msoy[s];
                    long long se2 = e2 + A[2] * msox[s] + B[2] * msoy[s];
                    if (!((se0 + bias[0]) >= 0 && (se1 + bias[1]) >= 0 && (se2 + bias[2]) >= 0)) { continue; }
                    float zl0 = (float)se0 * inv_area, zl1 = (float)se1 * inv_area, zl2 = (float)se2 * inv_area;
                    float z = zl0 * v0->z + zl1 * v1->z + zl2 * v2->z;   /* Tiefe je Sample */
                    if (t->depth_test && t->depth) {
                        float *dps = t->depth + (long long)s * dplane + (long long)py * t->width + px;
                        if (!r3d_depth_pass(t->depth_compare, z, *dps)) { continue; }
                        if (t->depth_write) { *dps = z; }
                    }
                    if (!shaded) {                                 /* Shading-Rate = 1x/Pixel (Zentrum) */
                        float l0 = (float)e0 * inv_area, l1 = (float)e1 * inv_area, l2 = (float)e2 * inv_area;
                        float cz = l0 * v0->z + l1 * v1->z + l2 * v2->z;
                        float iw = l0 * v0->iw + l1 * v1->iw + l2 * v2->iw;
                        float wr = 1.0f / iw;
                        float attr[R3D_MAX_ATTR];
                        for (int i = 0; i < nattr; i++) {
                            attr[i] = (l0 * v0->aow[i] + l1 * v1->aow[i] + l2 * v2->aow[i]) * wr;
                        }
                        col = fs ? fs(user, px, py, cz, attr) : pack_rgb(attr[0], attr[1], attr[2]);
                        shaded = 1;
                    }
                    t->color[(long long)s * splane + (long long)py * t->pitch_px + px] = col;
                }
            }
next_px:
            e0 += A[0] << 4; e1 += A[1] << 4; e2 += A[2] << 4;   /* +1 Pixel = +16 Sub */
        }
        E[0] += B[0] << 4; E[1] += B[1] << 4; E[2] += B[2] << 4;
    }
}

void r3d_draw_tri(const r3d_target_t *t,
                  const r3d_vtx_t *a, const r3d_vtx_t *b, const r3d_vtx_t *c,
                  int nattr, r3d_fs_fn fs, void *user)
{
    if (nattr < 0) { nattr = 0; }
    if (nattr > R3D_MAX_ATTR) { nattr = R3D_MAX_ATTR; }

    /* Clipping: NEAR + FAR immer; die Guard-Band-Ebenen (w>=eps, |x|<=Gw, |y|<=Gw) nur,
     * wenn eine Ecke sie verletzt (Fast-Path fuer den Normalfall). Jede Ebene fuegt
     * hoechstens 1 Ecke hinzu: 3 Ecken + 7 Ebenen <= 10 -> Puffer [12]. */
    r3d_vtx_t p1[12], p2[12];
    p1[0] = *a; p1[1] = *b; p1[2] = *c;
    int n = clip_plane(p2, p1, 3, nattr, CP_NEAR);
    if (n < 3) { return; }
    n = clip_plane(p1, p2, n, nattr, CP_FAR);
    if (n < 3) { return; }

    int in_band = 1;
    for (int i = 0; i < n && in_band; i++) {
        if (plane_dist(&p1[i], CP_WMIN) < 0.0f ||
            plane_dist(&p1[i], CP_XMIN) < 0.0f || plane_dist(&p1[i], CP_XMAX) < 0.0f ||
            plane_dist(&p1[i], CP_YMIN) < 0.0f || plane_dist(&p1[i], CP_YMAX) < 0.0f) {
            in_band = 0;
        }
    }
    if (!in_band) {
        n = clip_plane(p2, p1, n, nattr, CP_WMIN);  if (n < 3) { return; }
        n = clip_plane(p1, p2, n, nattr, CP_XMIN);  if (n < 3) { return; }
        n = clip_plane(p2, p1, n, nattr, CP_XMAX);  if (n < 3) { return; }
        n = clip_plane(p1, p2, n, nattr, CP_YMIN);  if (n < 3) { return; }
        n = clip_plane(p2, p1, n, nattr, CP_YMAX);  if (n < 3) { return; }
        for (int i = 0; i < n; i++) { p1[i] = p2[i]; }
    }

    svtx_t sv[12];
    for (int i = 0; i < n; i++) { project(t, &p1[i], &sv[i], nattr); }

    /* Faecher-Triangulation des konvexen Clip-Polygons (einheitliche Orientierung). */
    for (int i = 2; i < n; i++) {
        raster_tri(t, &sv[0], &sv[i - 1], &sv[i], nattr, fs, user);
    }
}
