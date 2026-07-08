/*
 * user/lib/vision/vi_engine.c  --  Inferenz-Engine des Vision-Modul
 */
#include "vi_engine.h"
#include <arm_neon.h>

/*
 * Row-major C = A*B. Aeussere Schleife ueber die Ausgabe-Zeilen i; innere akkumuliert je
 * 4-breite Spaltengruppe von B mit vfmaq_n (FMA Vektor*Skalar = die GEMM-Grundoperation):
 *   acc[0..3] += B[k, j..j+3] * A[i, k]      ueber alle k.
 * Der N-Rest (nicht durch 4 teilbar) laeuft skalar -- korrekt fuer beliebige Dimensionen.
 * Bei -O2 bleiben die Akkumulatoren in q-Registern.
 */
/* Rechnet NUR die Ausgabe-Zeilen [r0, r1) -- Basis fuer die 4-Kern-Aufteilung (A1.5): jeder
 * Co-Thread rechnet einen disjunkten Zeilen-Block, das Ergebnis ist bit-identisch zum Einkern-
 * Lauf (jedes Ausgabe-Element wird gleich gerechnet, nur auf einen anderen Kern verteilt). */
static void sgemm_rows(int N, int K, const float *A, const float *B, float *C, int r0, int r1)
{
    int nb = N & ~3;                                  /* groesstes Vielfaches von 4 <= N */
    for (int i = r0; i < r1; i++) {
        const float *arow = A + (long)i * K;
        float *crow = C + (long)i * N;
        int j = 0;
        for (; j < nb; j += 4) {
            float32x4_t acc = vdupq_n_f32(0.0f);
            for (int k = 0; k < K; k++) {
                float32x4_t bv = vld1q_f32(B + (long)k * N + j);
                acc = vfmaq_n_f32(acc, bv, arow[k]);  /* acc += bv * arow[k] */
            }
            vst1q_f32(crow + j, acc);
        }
        for (; j < N; j++) {                          /* skalarer Rest (N % 4) */
            float acc = 0.0f;
            for (int k = 0; k < K; k++) {
                acc += arow[k] * B[(long)k * N + j];
            }
            crow[j] = acc;
        }
    }
}

static void neon_sgemm(int M, int N, int K,
                       const float *A, const float *B, float *C)
{
    sgemm_rows(N, K, A, B, C, 0, M);
}

void vi_sgemm_rows(int M, int N, int K, const float *A, const float *B, float *C, int r0, int r1)
{
    (void)M;
    sgemm_rows(N, K, A, B, C, r0, r1);
}

static const vi_backend_t g_neon_backend = { "neon-fp32", neon_sgemm };

const vi_backend_t *vi_backend_neon(void) { return &g_neon_backend; }

void vi_sgemm(int M, int N, int K, const float *A, const float *B, float *C)
{
    g_neon_backend.sgemm(M, N, K, A, B, C);
}

/*
 * conv2d = im2col + sgemm: die Faltung wird zur Matrix-Multiplikation weights[Cout][K] *
 * col[K][N] mit K=Cin*KH*KW, N=OH*OW. So laeuft der teure Teil ueber das NEON-sgemm und
 * bleibt Backend-agnostisch (ein spaeteres int8-/V3D-Backend erbt die Beschleunigung).
 * Die im2col-Reihenfolge (c,ky,kx) MUSS der Gewichts-Reihenfolge [Cout][Cin][KH][KW]
 * entsprechen -- sonst faltet man mit vertauschten Taps.
 */
void vi_conv2d(int Cin, int H, int W,
               int Cout, int KH, int KW, int stride, int pad,
               const float *in, const float *weights, const float *bias,
               float *out, float *scratch)
{
    int OH = (H + 2 * pad - KH) / stride + 1;
    int OW = (W + 2 * pad - KW) / stride + 1;
    int K  = Cin * KH * KW;
    int N  = OH * OW;

    for (int c = 0; c < Cin; c++) {
        for (int ky = 0; ky < KH; ky++) {
            for (int kx = 0; kx < KW; kx++) {
                int row = (c * KH + ky) * KW + kx;      /* == c*KH*KW + ky*KW + kx */
                float *srow = scratch + (long)row * N;
                for (int oy = 0; oy < OH; oy++) {
                    int iy = oy * stride + ky - pad;
                    for (int ox = 0; ox < OW; ox++) {
                        int ix = ox * stride + kx - pad;
                        float v = 0.0f;                 /* Zero-Padding ausserhalb */
                        if (iy >= 0 && iy < H && ix >= 0 && ix < W) {
                            v = in[((long)c * H + iy) * W + ix];
                        }
                        srow[oy * OW + ox] = v;
                    }
                }
            }
        }
    }

    vi_sgemm(Cout, N, K, weights, scratch, out);        /* out[Cout][N] = W * col */

    if (bias) {
        for (int oc = 0; oc < Cout; oc++) {
            float b = bias[oc];
            float *orow = out + (long)oc * N;
            for (int n = 0; n < N; n++) { orow[n] += b; }
        }
    }
}

/*
 * Depthwise: jeder Kanal faltet NUR mit seinem eigenen Kernel (keine Kanal-Summe). Direkt
 * gerechnet -- im2col+GEMM lohnt hier nicht (K=KH*KW winzig, Kanaele unabhaengig). NEON-
 * Optimierung ueber die Ausgabe-x moeglich (spaeter); A1.2 rechnet korrekt skalar.
 */
void vi_dwconv(int C, int H, int W,
               int KH, int KW, int stride, int pad,
               const float *in, const float *weights, const float *bias,
               float *out)
{
    int OH = (H + 2 * pad - KH) / stride + 1;
    int OW = (W + 2 * pad - KW) / stride + 1;

    for (int c = 0; c < C; c++) {
        const float *ich = in      + (long)c * H * W;
        const float *wch = weights + (long)c * KH * KW;
        float *och = out + (long)c * OH * OW;
        float b = bias ? bias[c] : 0.0f;
        for (int oy = 0; oy < OH; oy++) {
            for (int ox = 0; ox < OW; ox++) {
                float acc = b;
                for (int ky = 0; ky < KH; ky++) {
                    int iy = oy * stride + ky - pad;
                    if (iy < 0 || iy >= H) { continue; }
                    for (int kx = 0; kx < KW; kx++) {
                        int ix = ox * stride + kx - pad;
                        if (ix < 0 || ix >= W) { continue; }
                        acc += ich[iy * W + ix] * wch[ky * KW + kx];
                    }
                }
                och[oy * OW + ox] = acc;
            }
        }
    }
}

/* --- A1.3: Aktivierungen / Pooling / Softmax / BatchNorm --- */

/*
 * expf ohne libm (freestanding): exp(x) = 2^(x*log2e). Ganzzahliger Exponent per Bit-Trick
 * in das float-Exponentenfeld, 2^f (f in [-0.5,0.5]) per Taylor-Polynom 3. Grades. Genauigkeit
 * ~1e-3 relativ -- reichlich fuer Sigmoid/Softmax in einer Vision-Pipeline.
 */
static float vi_expf(float x)
{
    if (x > 88.0f)  { x = 88.0f; }
    if (x < -88.0f) { x = -88.0f; }
    const float LOG2E = 1.4426950408889634f;
    float t = x * LOG2E;
    int i = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));      /* runden zur naechsten Ganzzahl */
    float f = t - (float)i;                              /* Rest in [-0.5, 0.5] */
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * 0.0555041f));  /* 2^f (Taylor) */
    union { float f; unsigned u; } b;
    b.f = p;
    int e = (int)((b.u >> 23) & 0xFFu) + i;              /* 2^i an den Exponenten addieren */
    if (e <= 0)   { return 0.0f; }                       /* Underflow */
    if (e >= 255) { e = 254; }                           /* geklammert (x ohnehin begrenzt) */
    b.u = (b.u & 0x807FFFFFu) | ((unsigned)e << 23);
    return b.f;
}

void vi_relu(float *x, int n)
{
    for (int i = 0; i < n; i++) { if (x[i] < 0.0f) { x[i] = 0.0f; } }
}

void vi_relu6(float *x, int n)
{
    for (int i = 0; i < n; i++) {
        float v = x[i];
        x[i] = v < 0.0f ? 0.0f : (v > 6.0f ? 6.0f : v);
    }
}

void vi_sigmoid(float *x, int n)
{
    for (int i = 0; i < n; i++) { x[i] = 1.0f / (1.0f + vi_expf(-x[i])); }
}

void vi_softmax(float *x, int n)
{
    if (n <= 0) { return; }
    float mx = x[0];
    for (int i = 1; i < n; i++) { if (x[i] > mx) { mx = x[i]; } }   /* Stabilitaet: max abziehen */
    float sum = 0.0f;
    for (int i = 0; i < n; i++) { x[i] = vi_expf(x[i] - mx); sum += x[i]; }
    float inv = (sum > 0.0f) ? (1.0f / sum) : 0.0f;
    for (int i = 0; i < n; i++) { x[i] *= inv; }
}

void vi_maxpool2d(int C, int H, int W, int KH, int KW, int stride, const float *in, float *out)
{
    int OH = (H - KH) / stride + 1;
    int OW = (W - KW) / stride + 1;
    for (int c = 0; c < C; c++) {
        const float *ich = in  + (long)c * H * W;
        float *och = out + (long)c * OH * OW;
        for (int oy = 0; oy < OH; oy++) {
            for (int ox = 0; ox < OW; ox++) {
                float m = ich[(oy * stride) * W + (ox * stride)];
                for (int ky = 0; ky < KH; ky++) {
                    for (int kx = 0; kx < KW; kx++) {
                        float v = ich[(oy * stride + ky) * W + (ox * stride + kx)];
                        if (v > m) { m = v; }
                    }
                }
                och[oy * OW + ox] = m;
            }
        }
    }
}

void vi_avgpool2d(int C, int H, int W, int KH, int KW, int stride, const float *in, float *out)
{
    int OH = (H - KH) / stride + 1;
    int OW = (W - KW) / stride + 1;
    float inv = 1.0f / (float)(KH * KW);
    for (int c = 0; c < C; c++) {
        const float *ich = in  + (long)c * H * W;
        float *och = out + (long)c * OH * OW;
        for (int oy = 0; oy < OH; oy++) {
            for (int ox = 0; ox < OW; ox++) {
                float s = 0.0f;
                for (int ky = 0; ky < KH; ky++) {
                    for (int kx = 0; kx < KW; kx++) {
                        s += ich[(oy * stride + ky) * W + (ox * stride + kx)];
                    }
                }
                och[oy * OW + ox] = s * inv;
            }
        }
    }
}

void vi_global_avgpool(int C, int H, int W, const float *in, float *out)
{
    float inv = 1.0f / (float)(H * W);
    for (int c = 0; c < C; c++) {
        const float *ich = in + (long)c * H * W;
        float s = 0.0f;
        for (int i = 0; i < H * W; i++) { s += ich[i]; }
        out[c] = s * inv;
    }
}

void vi_affine_per_channel(float *x, int C, int HW, const float *scale, const float *shift)
{
    for (int c = 0; c < C; c++) {
        float s = scale[c], b = shift[c];
        float *xc = x + (long)c * HW;
        for (int i = 0; i < HW; i++) { xc[i] = xc[i] * s + b; }
    }
}

void vi_bn_fold(int C, const float *gamma, const float *beta,
                const float *mean, const float *var, float eps,
                float *scale, float *shift)
{
    for (int c = 0; c < C; c++) {
        float s = gamma[c] / __builtin_sqrtf(var[c] + eps);
        scale[c] = s;
        shift[c] = beta[c] - mean[c] * s;
    }
}

const char *vi_backend_name(void) { return g_neon_backend.name; }
