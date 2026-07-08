/*
 * user/aivision.c  --  AIVISION.ELF: Selbsttest-App des Vision-Tracks (KI-Bildauswertung).
 */
#include "abi.h"
#include "ulib.h"
#include "vision/vi_engine.h"
#include "vision/vi_model.h"
#include "vision/vi_img.h"
#include "vision/vi_par.h"
#include "vision/vi_detect.h"
#include "vision/vi_embed.h"

/* --- Ausgabe-Helfer (eine Zeile je SYS_WRITE -> cross-core atomar) --- */
static char lbuf[200];
static int  lpos;
static void lflush(void) { if (lpos > 0) { sys3(SYS_WRITE, 1, (long)lbuf, lpos); lpos = 0; } }
static void uwrite(const char *s)
{
    for (; *s; ++s) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = *s; } if (*s == '\n') { lflush(); } }
}
static void uwr_u(unsigned long v)
{
    char tmp[24], out[25]; int i = 0, p = 0;
    if (v == 0) { uwrite("0"); return; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i > 0) { out[p++] = tmp[--i]; }
    out[p] = 0;
    uwrite(out);
}

/* Auf die naechste ganze Zahl runden (Vergleich exakter Integer-Ergebnisse; vorzeichenrichtig). */
static long iround(float f) { return (long)(f + (f >= 0.0f ? 0.5f : -0.5f)); }
/* Naeherungsvergleich fuer approximierte Werte (Sigmoid/Softmax/Sqrt). */
static int approx(float a, float b) { float d = a - b; if (d < 0.0f) { d = -d; } return d < 1e-3f; }

/* Fall 1: A(4x3), B(3x4) -> C(4x4). */
static const float A0[12] = { 1,2,3,  4,5,6,  7,8,9,  10,11,12 };
static const float B0[12] = { 1,2,3,4,  5,6,7,8,  9,10,11,12 };
static float C0[16];

/* Fall 2: A(1x2), B(2x5) -> C(1x5) (N=5-Tail). */
static const float A1[2]  = { 2, 3 };
static const float B1[10] = { 1,1,1,1,1,  1,1,1,1,1 };
static float C1[5];

/* --- A1.2: conv2d + dwconv Referenz-Daten (ganzzahlig -> fp32 exakt) --- */
static const float cin33[9] = { 1,2,3, 4,5,6, 7,8,9 };
static const float wbox[9]  = { 1,1,1, 1,1,1, 1,1,1 };    /* 3x3-Box (Einsen) */
static const float wtl[9]   = { 1,0,0, 0,0,0, 0,0,0 };    /* nur Tap oben-links (asymmetrisch) */
static const float cbias[1] = { 10 };
static float cout9[9];
static float cscratch[256];                               /* im2col-Puffer (>= 9*9) */

static const float dcin[18] = { 1,2,3, 4,5,6, 7,8,9,   1,2,3, 4,5,6, 7,8,9 };  /* 2 Kanaele */
static const float wdw[18]  = { 1,1,1, 1,1,1, 1,1,1,   0,0,0, 0,2,0, 0,0,0 };  /* ch0 box, ch1 2*Zentrum */
static float dout18[18];

/* --- A1.3 Referenz-Daten --- */
static float a13buf[5];                    /* ReLU/ReLU6/Sigmoid (in-place) */
static float smbuf[4];                     /* Softmax [1,1,1,1] */
static float sm3[3];                       /* Softmax [0,0,10] */
static const float pool_in[16] = { 1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16 };
static float pool_out[4];
static float gap_out[1];
static float bn_scale[1], bn_shift[1], bn_x[3];

/* --- A1.4a: Modell-Blob-Loader Test-Fixtures --- */
static uint8_t blobbuf[2048] __attribute__((aligned(4)));
static vi_model_t g_model;

static void bw_u32(uint8_t *b, unsigned long *o, uint32_t v)
{
    b[*o] = v & 0xFF; b[*o + 1] = (v >> 8) & 0xFF;
    b[*o + 2] = (v >> 16) & 0xFF; b[*o + 3] = (v >> 24) & 0xFF;
    *o += 4;
}
static void bw_f32(uint8_t *b, unsigned long *o, float f)
{
    union { float f; uint32_t u; } x; x.f = f; bw_u32(b, o, x.u);
}
static void bw_u16(uint8_t *b, unsigned long *o, uint16_t v)
{
    b[*o] = v & 0xFF; b[*o + 1] = (v >> 8) & 0xFF; *o += 2;
}
static void blob_set_u32(uint8_t *b, unsigned long o, uint32_t v)
{
    b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF; b[o + 2] = (v >> 16) & 0xFF; b[o + 3] = (v >> 24) & 0xFF;
}
static void blob_get(const uint8_t *b, unsigned long o, uint8_t *sv) { for (int i = 0; i < 4; i++) { sv[i] = b[o + i]; } }
static void blob_put(uint8_t *b, unsigned long o, const uint8_t *sv) { for (int i = 0; i < 4; i++) { b[o + i] = sv[i]; } }

/* Baut einen gueltigen 2-Layer-Blob (CONV 4x1x3x3 + RELU); liefert die Blob-Laenge. */
static unsigned long build_valid_blob(uint8_t *b)
{
    unsigned long o = 0;
    bw_u32(b, &o, VI_NET_MAGIC);
    bw_u32(b, &o, VI_NET_VERSION);
    bw_u32(b, &o, 2);                                       /* n_layers */
    bw_u32(b, &o, 1); bw_u32(b, &o, 8); bw_u32(b, &o, 8);   /* in 1x8x8 */
    bw_u32(b, &o, VI_L_CONV);                               /* Layer 0: CONV */
    bw_u32(b, &o, 4); bw_u32(b, &o, 3); bw_u32(b, &o, 3); bw_u32(b, &o, 1); bw_u32(b, &o, 1);
    bw_u32(b, &o, 36); bw_u32(b, &o, 4);                    /* w=4*1*3*3=36, b=4 */
    for (int i = 0; i < 36; i++) { bw_f32(b, &o, (float)i); }
    for (int i = 0; i < 4; i++)  { bw_f32(b, &o, (float)i); }
    bw_u32(b, &o, VI_L_RELU);                               /* Layer 1: RELU (keine w/b) */
    bw_u32(b, &o, 0); bw_u32(b, &o, 0); bw_u32(b, &o, 0); bw_u32(b, &o, 0); bw_u32(b, &o, 0);
    bw_u32(b, &o, 0); bw_u32(b, &o, 0);
    return o;
}

/* 40 gueltige RELU-Layer (je 32 B): NUR die VI_MAX_LAYERS-Kappung verhindert den Ueberlauf
 * von m->layers[32] (der Cursor liest problemlos alle 40 -> kappt hier NICHT von selbst). */
static unsigned long build_manylayer_blob(uint8_t *b)
{
    unsigned long o = 0;
    bw_u32(b, &o, VI_NET_MAGIC); bw_u32(b, &o, VI_NET_VERSION);
    bw_u32(b, &o, 40); bw_u32(b, &o, 1); bw_u32(b, &o, 1); bw_u32(b, &o, 1);
    for (int i = 0; i < 40; i++) {
        bw_u32(b, &o, VI_L_RELU);
        for (int k = 0; k < 5; k++) { bw_u32(b, &o, 0); }
        bw_u32(b, &o, 0); bw_u32(b, &o, 0);
    }
    return o;
}

/* 1 CONV-Layer, dessen w_count (100) mehr Floats behauptet, als der Blob (endet nach den
 * Zaehlern) enthaelt: NUR die need>avail-Pruefung verhindert ein SUCCESS mit OOB-Zeiger. */
static unsigned long build_badcount_blob(uint8_t *b)
{
    unsigned long o = 0;
    bw_u32(b, &o, VI_NET_MAGIC); bw_u32(b, &o, VI_NET_VERSION);
    bw_u32(b, &o, 1); bw_u32(b, &o, 1); bw_u32(b, &o, 1); bw_u32(b, &o, 3);
    bw_u32(b, &o, VI_L_CONV);
    bw_u32(b, &o, 1); bw_u32(b, &o, 3); bw_u32(b, &o, 3); bw_u32(b, &o, 1); bw_u32(b, &o, 0);
    bw_u32(b, &o, 100); bw_u32(b, &o, 0);                   /* w_count=100, aber KEINE Gewichts-Bytes */
    return o;                                                /* len == 56 -> avail=0 < need=100 */
}

/* Mini-Netz fuer den Graph-Runner: 1x3x3 -> CONV(box,bias0) -> RELU -> GAP -> FC(w=2,b=1).
 * Box-Summe von 1..9 = 245; GAP = 245/9; FC = (245/9)*2 + 1 (handgerechnete Referenz). */
static unsigned long build_mininet_blob(uint8_t *b)
{
    unsigned long o = 0;
    bw_u32(b, &o, VI_NET_MAGIC); bw_u32(b, &o, VI_NET_VERSION);
    bw_u32(b, &o, 4);                                       /* n_layers */
    bw_u32(b, &o, 1); bw_u32(b, &o, 3); bw_u32(b, &o, 3);   /* in 1x3x3 */
    bw_u32(b, &o, VI_L_CONV);                               /* L0: CONV 1x1x3x3 s1 p1, box + bias 0 */
    bw_u32(b, &o, 1); bw_u32(b, &o, 3); bw_u32(b, &o, 3); bw_u32(b, &o, 1); bw_u32(b, &o, 1);
    bw_u32(b, &o, 9); bw_u32(b, &o, 1);
    for (int i = 0; i < 9; i++) { bw_f32(b, &o, 1.0f); }
    bw_f32(b, &o, 0.0f);
    bw_u32(b, &o, VI_L_RELU);                               /* L1: RELU */
    for (int k = 0; k < 5; k++) { bw_u32(b, &o, 0); }
    bw_u32(b, &o, 0); bw_u32(b, &o, 0);
    bw_u32(b, &o, VI_L_GAP);                                /* L2: Global-Average-Pool */
    for (int k = 0; k < 5; k++) { bw_u32(b, &o, 0); }
    bw_u32(b, &o, 0); bw_u32(b, &o, 0);
    bw_u32(b, &o, VI_L_FC);                                 /* L3: FC 1->1, w=2, b=1 */
    bw_u32(b, &o, 1); bw_u32(b, &o, 0); bw_u32(b, &o, 0); bw_u32(b, &o, 0); bw_u32(b, &o, 0);
    bw_u32(b, &o, 1); bw_u32(b, &o, 1);
    bw_f32(b, &o, 2.0f);
    bw_f32(b, &o, 1.0f);
    return o;
}

static vi_model_t g_net;
static float run_a[64], run_b[64], run_scr[256];
static const float netin[9] = { 1,2,3,4,5,6,7,8,9 };

/* --- A1.6: 2x2-24bpp-BMP Test-Fixture --- */
static float img_out[3 * 16 * 16];

/* Baut ein 2x2 24bpp-BMP; Bild (top-down logisch): (0,0)=rot (1,0)=gruen / (0,1)=blau (1,1)=weiss. */
static unsigned long build_bmp(uint8_t *b)
{
    unsigned long o = 0;
    b[o++] = 'B'; b[o++] = 'M';
    bw_u32(b, &o, 70);                        /* Dateigroesse */
    bw_u32(b, &o, 0);                         /* reserviert */
    bw_u32(b, &o, 54);                        /* Pixel-Offset */
    bw_u32(b, &o, 40);                        /* DIB-Groesse (BITMAPINFOHEADER) */
    bw_u32(b, &o, 2); bw_u32(b, &o, 2);       /* width=2, height=2 (bottom-up) */
    bw_u16(b, &o, 1); bw_u16(b, &o, 24);      /* planes, bpp */
    bw_u32(b, &o, 0); bw_u32(b, &o, 16);      /* compression, Bildgroesse */
    bw_u32(b, &o, 2835); bw_u32(b, &o, 2835); /* ppm x/y */
    bw_u32(b, &o, 0); bw_u32(b, &o, 0);       /* colors used/important */
    /* Datei-Zeile 0 = UNTERE Bildzeile (y=1): blau, weiss (BGR) + 2 Pad */
    b[o++] = 255; b[o++] = 0;   b[o++] = 0;     /* blau  BGR */
    b[o++] = 255; b[o++] = 255; b[o++] = 255;   /* weiss */
    b[o++] = 0;   b[o++] = 0;                    /* Padding */
    /* Datei-Zeile 1 = OBERE Bildzeile (y=0): rot, gruen (BGR) + 2 Pad */
    b[o++] = 0;   b[o++] = 0;   b[o++] = 255;    /* rot   BGR */
    b[o++] = 0;   b[o++] = 255; b[o++] = 0;      /* gruen */
    b[o++] = 0;   b[o++] = 0;                    /* Padding */
    return o;                                    /* == 70 */
}

/* --- A1.5: 4-Kern-Parallel-GEMM Test-Fixtures --- */
#define PGM 64
#define PGN 64
#define PGK 64
static float pgA[PGM * PGK], pgB[PGK * PGN], pgCref[PGM * PGN], pgCpar[PGM * PGN];
struct gemm_job { int M, N, K; const float *A, *B; float *C; volatile int cpu[4]; };
static struct gemm_job g_job;

/* Slice-Fn: rechnet den Zeilen-Block [wid*M/n, (wid+1)*M/n) und merkt sich den Kern. */
static void gemm_slice(void *arg, int wid, int n)
{
    struct gemm_job *j = (struct gemm_job *)arg;
    int r0 = wid * j->M / n;
    int r1 = (wid + 1) * j->M / n;
    vi_sgemm_rows(j->M, j->N, j->K, j->A, j->B, j->C, r0, r1);
    if (wid >= 0 && wid < 4) { j->cpu[wid] = (int)sys0(SYS_GETCPU); }
}

/* --- M0: hdd1-Datei-Bild + -Modell -> Klassifikation --- */
static uint8_t   m0_bmp[1024];
static uint8_t   m0_net[512] __attribute__((aligned(4)));
static vi_model_t m0_model;
static float     m0_img[3 * 8 * 8];
static float     m0_a[3 * 8 * 8], m0_b[3 * 8 * 8], m0_scr[64];

/* --- A2: Detektions-Pipeline Test-Fixtures --- */
static float    a2_heat[64];
static vi_box_t a2_boxes[16];
static vi_box_t a2_nms[3] = { {10,10,20,20, 0.9f}, {12,12,20,20, 0.7f}, {100,100,20,20, 0.8f} };
static uint32_t a2_fb[32 * 32];

/* --- A3: Embedding + Anomalie Test-Fixtures --- */
static float a3_known[2 * 4] = { 1,0,0,0,  0,1,0,0 };    /* 2 enrollte (Einheits-)Vektoren */
static float a3_qk[4] = { 0.9f, 0.1f, 0, 0 };            /* nah an bekannt[0] */
static float a3_qa[4] = { 0, 0, 1, 0 };                  /* fremd */
static float a3_norm[4] = { 3, 4, 0, 0 };                /* -> [0.6,0.8,0,0] */

/* --- A4.2: YUYV->RGB Test-Fixtures --- */
static uint8_t a42_yuyv[8] = { 128,128,128,255,  128,128,128,255 };   /* 4 Pixel, V=255 (rot-verschoben) */
static float   a42_rgb[3 * 4];

/* --- A5.1: Echtzeit-Pipeline-Körper Test-Fixtures --- */
static float    a51_frame[16 * 16];
static float    a51_w[16];               /* 4x4-Mittelungskernel */
static float    a51_bias[1] = { 0 };
static float    a51_heat[4 * 4];
static float    a51_scratch[4 * 4 * 16]; /* im2col: Cin*KH*KW * OH*OW = 16*16 */
static vi_box_t a51_boxes[16];
static uint32_t a51_rgb[16 * 16];
static uint8_t  a5_frame[8];              /* Dummy-Grab-Puffer (A5-Loop) */

void _start(void)
{
    long cpu = sys0(SYS_GETCPU);

    /* Fall 1: 4x3 * 3x4 -> Summe=2156, C[3][3]=272, C[0][0]=38. */
    vi_sgemm(4, 4, 3, A0, B0, C0);
    float sum0 = 0.0f;
    for (int i = 0; i < 16; i++) { sum0 += C0[i]; }
    int ok0 = (iround(sum0) == 2156) && (iround(C0[15]) == 272) && (iround(C0[0]) == 38);

    /* Fall 2: N=5-Tail -> alle 5 Elemente = 5, Summe=25. */
    vi_sgemm(1, 5, 2, A1, B1, C1);
    float sum1 = 0.0f; int tailok = 1;
    for (int j = 0; j < 5; j++) { sum1 += C1[j]; if (iround(C1[j]) != 5) { tailok = 0; } }
    tailok = tailok && (iround(sum1) == 25);

    uwrite("[aivision] backend=");
    uwrite(vi_backend_name());
    uwrite(" cpu=");
    uwr_u((unsigned long)cpu);
    uwrite("\n");

    if (ok0 && tailok) {
        uwrite("[aivision] A1.1 sgemm NEON-fp32: checksum=2156 c33=272 tail=ok\n");
    } else {
        uwrite("[aivision] A1.1 FEHLER: sgemm falsch (checksum=");
        uwr_u((unsigned long)iround(sum0));
        uwrite(" c33=");
        uwr_u((unsigned long)iround(C0[15]));
        uwrite(" tail=");
        uwrite(tailok ? "ok" : "falsch");
        uwrite(")\n");
    }

    /* --- A1.2: conv2d (im2col->sgemm) + dwconv --- */
    /* box-Kernel (3x3 Einsen) + bias=10 auf 3x3-Bild -> Summe=335, out[1][1]=55, out[0][0]=22. */
    vi_conv2d(1, 3, 3,  1, 3, 3, 1, 1,  cin33, wbox, cbias, cout9, cscratch);
    float csum = 0.0f; for (int i = 0; i < 9; i++) { csum += cout9[i]; }
    int conv_ok = (iround(csum) == 335) && (iround(cout9[4]) == 55) && (iround(cout9[0]) == 22);

    /* Single-Tap oben-links (nur w[0][0]=1), ohne bias -> out[oy][ox]=in[oy-1][ox-1]
     * (asymmetrisch -> faengt Kernel-Flip/Transpose): out[1][1]=1, out[2][2]=5, out[0][0]=0. */
    vi_conv2d(1, 3, 3,  1, 3, 3, 1, 1,  cin33, wtl, 0, cout9, cscratch);
    int orient_ok = (iround(cout9[4]) == 1) && (iround(cout9[8]) == 5) && (iround(cout9[0]) == 0);

    /* dwconv 2 Kanaele: ch0 box -> Summe 245; ch1 nur Zentrum=2 -> 2*in, Summe 90, out[1][1]=10.
     * Prueft, dass jeder Kanal NUR seinen eigenen Kernel sieht (keine Kanal-Vermischung). */
    vi_dwconv(2, 3, 3,  3, 3, 1, 1,  dcin, wdw, 0, dout18);
    float s0 = 0.0f, s1 = 0.0f;
    for (int i = 0; i < 9; i++) { s0 += dout18[i]; s1 += dout18[9 + i]; }
    int dw_ok = (iround(s0) == 245) && (iround(s1) == 90) && (iround(dout18[9 + 4]) == 10);

    if (conv_ok && orient_ok && dw_ok) {
        uwrite("[aivision] A1.2 conv2d+bias=ok orient=ok dwconv(per-channel)=ok\n");
    } else {
        uwrite("[aivision] A1.2 FEHLER: conv=");
        uwrite(conv_ok ? "ok" : "falsch");
        uwrite(" orient=");
        uwrite(orient_ok ? "ok" : "falsch");
        uwrite(" dwconv=");
        uwrite(dw_ok ? "ok" : "falsch");
        uwrite("\n");
    }

    /* --- A1.3: Aktivierungen / Pooling / Softmax / BatchNorm --- */
    a13buf[0]=-2; a13buf[1]=-1; a13buf[2]=0; a13buf[3]=1; a13buf[4]=2;
    vi_relu(a13buf, 5);
    int relu_ok = iround(a13buf[0])==0 && iround(a13buf[2])==0 && iround(a13buf[3])==1 && iround(a13buf[4])==2;

    a13buf[0]=-1; a13buf[1]=3; a13buf[2]=7;
    vi_relu6(a13buf, 3);
    int relu6_ok = iround(a13buf[0])==0 && iround(a13buf[1])==3 && iround(a13buf[2])==6;

    a13buf[0]=0; a13buf[1]=10; a13buf[2]=-10;     /* Sigmoid: 0->0.5, gross->~1, klein->~0 */
    vi_sigmoid(a13buf, 3);
    int sig_ok = approx(a13buf[0], 0.5f) && a13buf[1] > 0.99f && a13buf[2] < 0.01f;

    smbuf[0]=1; smbuf[1]=1; smbuf[2]=1; smbuf[3]=1;           /* gleichverteilt -> je 0.25 */
    vi_softmax(smbuf, 4);
    float smsum = smbuf[0]+smbuf[1]+smbuf[2]+smbuf[3];
    sm3[0]=0; sm3[1]=0; sm3[2]=10;                            /* dominanter Logit -> ~1 */
    vi_softmax(sm3, 3);
    int soft_ok = approx(smbuf[0], 0.25f) && approx(smsum, 1.0f) && sm3[2] > 0.999f;

    vi_maxpool2d(1,4,4, 2,2,2, pool_in, pool_out);            /* -> [6,8,14,16] */
    int mp_ok = iround(pool_out[0])==6 && iround(pool_out[1])==8 && iround(pool_out[2])==14 && iround(pool_out[3])==16;

    vi_avgpool2d(1,4,4, 2,2,2, pool_in, pool_out);            /* -> [3.5,5.5,11.5,13.5] */
    int ap_ok = approx(pool_out[0], 3.5f) && approx(pool_out[3], 13.5f);

    vi_global_avgpool(1,4,4, pool_in, gap_out);               /* Mittel 1..16 = 8.5 */
    int gap_ok = approx(gap_out[0], 8.5f);

    { float g=2, be=1, m=3, v=4; vi_bn_fold(1, &g, &be, &m, &v, 0.0f, bn_scale, bn_shift); }
    bn_x[0]=5; bn_x[1]=3; bn_x[2]=1;                          /* scale=1, shift=-2 -> y=x-2 */
    vi_affine_per_channel(bn_x, 1, 3, bn_scale, bn_shift);
    int bn_ok = approx(bn_scale[0], 1.0f) && approx(bn_shift[0], -2.0f)
                && iround(bn_x[0])==3 && iround(bn_x[1])==1 && iround(bn_x[2])==-1;

    int a13_ok = relu_ok && relu6_ok && sig_ok && soft_ok && mp_ok && ap_ok && gap_ok && bn_ok;
    if (a13_ok) {
        uwrite("[aivision] A1.3 relu/relu6/sigmoid=ok pool(max/avg/global)=ok softmax=ok bn-fold=ok\n");
    } else {
        uwrite("[aivision] A1.3 FEHLER: relu="); uwrite(relu_ok?"ok":"x");
        uwrite(" relu6="); uwrite(relu6_ok?"ok":"x");
        uwrite(" sigmoid="); uwrite(sig_ok?"ok":"x");
        uwrite(" softmax="); uwrite(soft_ok?"ok":"x");
        uwrite(" maxpool="); uwrite(mp_ok?"ok":"x");
        uwrite(" avgpool="); uwrite(ap_ok?"ok":"x");
        uwrite(" gap="); uwrite(gap_ok?"ok":"x");
        uwrite(" bn="); uwrite(bn_ok?"ok":"x");
        uwrite("\n");
    }

    /* --- A1.4a: gehaerteter Modell-Blob-Loader (untrusted hdd1) --- */
    unsigned long blen = build_valid_blob(blobbuf);
    int parse_ok = (vi_model_load(blobbuf, blen, &g_model) == 0)
                   && g_model.n_layers == 2
                   && g_model.layers[0].type == VI_L_CONV
                   && g_model.layers[0].w_count == 36 && g_model.layers[0].b_count == 4
                   && g_model.layers[0].w != 0
                   && g_model.layers[1].type == VI_L_RELU;

    int rej = 1;                                            /* jede Verfaelschung MUSS abgelehnt werden */
    uint8_t sv[4];
    blob_get(blobbuf, 0, sv);  blob_set_u32(blobbuf, 0, 0xDEADBEEFu);      /* verfaelschtes Magic */
    if (vi_model_load(blobbuf, blen, &g_model) == 0) { rej = 0; }
    blob_put(blobbuf, 0, sv);
    if (vi_model_load(blobbuf, 10, &g_model) == 0) { rej = 0; }            /* abgeschnittener Header */
    if (vi_model_load(blobbuf, 66, &g_model) == 0) { rej = 0; }            /* mitten in den Gewichten ab */
    blob_get(blobbuf, 24, sv); blob_set_u32(blobbuf, 24, 99);              /* unbekannter Layer-Typ */
    if (vi_model_load(blobbuf, blen, &g_model) == 0) { rej = 0; }
    blob_put(blobbuf, 24, sv);
    /* Eigene Blobs fuer die praezisen Grenzfaelle (die der Cursor NICHT schon selbst kappt): */
    unsigned long mlen = build_manylayer_blob(blobbuf);                    /* 40 > VI_MAX_LAYERS(32) */
    if (vi_model_load(blobbuf, mlen, &g_model) == 0) { rej = 0; }
    unsigned long clen = build_badcount_blob(blobbuf);                     /* w_count > verbleibende Floats */
    if (vi_model_load(blobbuf, clen, &g_model) == 0) { rej = 0; }

    int a14a_ok = parse_ok && rej;
    if (a14a_ok) {
        uwrite("[aivision] A1.4a blob-loader: parse=ok reject(magic/trunc/type/nlayers/weights)=ok\n");
    } else {
        uwrite("[aivision] A1.4a FEHLER: parse="); uwrite(parse_ok ? "ok" : "x");
        uwrite(" reject="); uwrite(rej ? "ok" : "x"); uwrite("\n");
    }

    /* --- A1.4b: Graph-Runner (Mini-Netz end-to-end gegen handgerechnete Referenz) --- */
    unsigned long nlen = build_mininet_blob(blobbuf);
    int run_ok = 0;
    if (vi_model_load(blobbuf, nlen, &g_net) == 0) {
        int olen = 0;
        const float *y = vi_model_run(&g_net, netin, run_a, run_b, run_scr, &olen);
        float ref = (245.0f / 9.0f) * 2.0f + 1.0f;          /* CONV(box)->RELU->GAP->FC(*2+1) */
        run_ok = (olen == 1) && approx(y[0], ref);
    }
    if (run_ok) {
        uwrite("[aivision] A1.4b graph-runner: mininet(conv->relu->gap->fc)=ok\n");
    } else {
        uwrite("[aivision] A1.4b FEHLER: graph-runner falsch\n");
    }

    /* --- A1.6: gehaerteter BMP-Loader (untrusted hdd1) --- */
    unsigned long bmplen = build_bmp(blobbuf);
    int ih = 0, iw = 0, bmp_ok = 0;
    if (vi_bmp_load(blobbuf, bmplen, img_out, 16, 16, &ih, &iw) == 0 && ih == 2 && iw == 2) {
        unsigned long pl = 4;                                   /* H*W = 2*2 */
        bmp_ok = approx(img_out[0*pl + 0], 1.0f)                /* R @ (y0,x0)=rot */
              && approx(img_out[1*pl + 1], 1.0f)                /* G @ (y0,x1)=gruen */
              && approx(img_out[2*pl + 2], 1.0f)                /* B @ (y1,x0)=blau */
              && approx(img_out[0*pl + 3], 1.0f) && approx(img_out[1*pl + 3], 1.0f)
              && approx(img_out[2*pl + 3], 1.0f);               /* weiss @ (y1,x1) */
    }
    uint8_t bsv = blobbuf[0]; blobbuf[0] = 'X';                 /* Haertung: falsches Magic -> Ablehnung */
    int bmp_rej = (vi_bmp_load(blobbuf, bmplen, img_out, 16, 16, &ih, &iw) < 0);
    blobbuf[0] = bsv;
    int a16_ok = bmp_ok && bmp_rej;
    if (a16_ok) {
        uwrite("[aivision] A1.6 bmp-loader: 2x2 rgb-planes=ok reject-bad-magic=ok\n");
    } else {
        uwrite("[aivision] A1.6 FEHLER: bmp="); uwrite(bmp_ok ? "ok" : "x");
        uwrite(" reject="); uwrite(bmp_rej ? "ok" : "x"); uwrite("\n");
    }

    /* --- A1.5: 4-Kern-Parallel-GEMM ueber den VISION-Kernel-Parallel-For --- */
    for (int i = 0; i < PGM * PGK; i++) { pgA[i] = (float)((i * 7 + 3) % 13) - 6.0f; }
    for (int i = 0; i < PGK * PGN; i++) { pgB[i] = (float)((i * 5 + 1) % 11) - 5.0f; }
    vi_sgemm(PGM, PGN, PGK, pgA, pgB, pgCref);           /* Einkern-Referenz */
    g_job.M = PGM; g_job.N = PGN; g_job.K = PGK;
    g_job.A = pgA; g_job.B = pgB; g_job.C = pgCpar;
    for (int i = 0; i < 4; i++) { g_job.cpu[i] = -1; }
    vi_parallel(gemm_slice, &g_job, 4);                  /* 4-Kern-Aufteilung (wid 0 selbst, 1..3 Co-Threads) */
    int par_match = 1;
    for (int i = 0; i < PGM * PGN; i++) { if (pgCpar[i] != pgCref[i]) { par_match = 0; break; } }
    int ncores = 0;                                      /* distinkte Kerne -> beweist Mehrkern-Lauf */
    for (int c = 0; c < 4; c++) {
        int seen = 0;
        for (int d = 0; d < c; d++) { if (g_job.cpu[d] == g_job.cpu[c]) { seen = 1; } }
        if (g_job.cpu[c] >= 0 && !seen) { ncores++; }
    }
    int a15_ok = par_match && (ncores >= 2);
    if (a15_ok) {
        uwrite("[aivision] A1.5 parallel-sgemm: ergebnis==single-core kerne=");
        uwr_u((unsigned long)ncores);
        uwrite("\n");
    } else {
        uwrite("[aivision] A1.5 FEHLER: match="); uwrite(par_match ? "ok" : "x");
        uwrite(" kerne="); uwr_u((unsigned long)ncores); uwrite("\n");
    }

    /* --- M0: end-to-end Klassifikation aus einer echten hdd1-Datei (Bild + Modell, untrusted) --- */
    int m0_ok = 0, m0_cls = -1;
    long nb = sys3(SYS_READ_FILE, (long)"hdd1:VISIMG.BMP", (long)m0_bmp, (long)sizeof(m0_bmp));
    long nn = sys3(SYS_READ_FILE, (long)"hdd1:VISNET.NET", (long)m0_net, (long)sizeof(m0_net));
    if (nb > 0 && nn > 0) {
        int mh = 0, mw = 0;
        if (vi_bmp_load(m0_bmp, (unsigned long)nb, m0_img, 8, 8, &mh, &mw) == 0
            && vi_model_load(m0_net, (unsigned long)nn, &m0_model) == 0) {
            int olen = 0;
            const float *y = vi_model_run(&m0_model, m0_img, m0_a, m0_b, m0_scr, &olen);
            m0_cls = 0;
            for (int i = 1; i < olen; i++) { if (y[i] > y[m0_cls]) { m0_cls = i; } }
            m0_ok = (olen == 2) && (m0_cls == 0);       /* rot-dominantes Bild -> Klasse 0 */
        }
    }
    if (m0_ok) {
        uwrite("[aivision] M0 klassifikation: hdd1-bild+modell -> klasse=0 (rot-dominant) ok\n");
    } else {
        uwrite("[aivision] M0 FEHLER: nb="); uwr_u((unsigned long)nb);
        uwrite(" nn="); uwr_u((unsigned long)nn);
        uwrite(" klasse="); uwr_u((unsigned long)(long)m0_cls); uwrite("\n");
    }

    /* --- A2: Detektions-Pipeline (Heatmap-Decode + NMS + Box-Overlay) --- */
    for (int i = 0; i < 64; i++) { a2_heat[i] = 0.1f; }
    a2_heat[2 * 8 + 3] = 0.9f;                            /* ein Peak bei Zelle (cx=3, cy=2) */
    int nb2 = vi_decode_peaks(a2_heat, 8, 8, 0.5f, 8, a2_boxes, 16);
    int dec_ok = (nb2 == 1) && a2_boxes[0].x == 24 && a2_boxes[0].y == 16
                 && a2_boxes[0].w == 8 && a2_boxes[0].h == 8;

    int nk = vi_nms(a2_nms, 3, 0.3f);                     /* B(0.7) ueberlappt A(0.9) stark -> weg; C(0.8) bleibt */
    int nms_ok = (nk == 2) && (iround(a2_nms[0].score * 100) == 90) && (iround(a2_nms[1].score * 100) == 80);

    for (int i = 0; i < 32 * 32; i++) { a2_fb[i] = 0; }
    vi_box_t db = { 5, 5, 10, 8, 1.0f };
    vi_draw_box(a2_fb, 32, 32, 32, db, 0xFFFF0000u);
    int draw_ok = (a2_fb[5 * 32 + 5] == 0xFFFF0000u)      /* oben-links */
               && (a2_fb[5 * 32 + 14] == 0xFFFF0000u)     /* oben-rechts (x=5+10-1) */
               && (a2_fb[12 * 32 + 5] == 0xFFFF0000u)     /* unten-links (y=5+8-1) */
               && (a2_fb[10 * 32 + 10] == 0u);            /* Inneres NICHT gezeichnet */
    int a2_ok = dec_ok && nms_ok && draw_ok;
    if (a2_ok) {
        uwrite("[aivision] A2 detektor-pipeline: decode=ok nms(overlap-suppressed)=ok box-overlay=ok\n");
    } else {
        uwrite("[aivision] A2 FEHLER: decode="); uwrite(dec_ok ? "ok" : "x");
        uwrite(" nms="); uwrite(nms_ok ? "ok" : "x");
        uwrite(" draw="); uwrite(draw_ok ? "ok" : "x"); uwrite("\n");
    }

    /* --- A3: Embedding-Metrik + Anomalie (ein Netz, zwei Anwendungen: Identitaet + Anomalie) --- */
    vi_l2_normalize(a3_norm, 4);                         /* [3,4,0,0] -> [0.6,0.8,0,0] */
    int norm_ok = approx(a3_norm[0], 0.6f) && approx(a3_norm[1], 0.8f);
    vi_l2_normalize(a3_qk, 4);
    int e_idx = -1; float e_d2 = 0.0f;
    int r_known = vi_anomaly(a3_qk, a3_known, 2, 4, 0.2f, &e_idx, &e_d2);
    int known_ok = (r_known == 0) && (e_idx == 0);       /* nah an bekannt[0] -> known, Nachbar 0 */
    int r_anom = vi_anomaly(a3_qa, a3_known, 2, 4, 0.2f, &e_idx, &e_d2);
    int anom_ok = (r_anom == 1);                         /* fremd -> anomaly */
    int a3_ok = norm_ok && known_ok && anom_ok;
    if (a3_ok) {
        uwrite("[aivision] A3 embedding+anomalie: l2-norm=ok known(idx=0)=ok anomaly(fremd)=ok\n");
    } else {
        uwrite("[aivision] A3 FEHLER: norm="); uwrite(norm_ok ? "ok" : "x");
        uwrite(" known="); uwrite(known_ok ? "ok" : "x");
        uwrite(" anomaly="); uwrite(anom_ok ? "ok" : "x"); uwrite("\n");
    }

    /* --- A4.2: YUYV(4:2:2)->RGB Decode (untrusted Kamera-Daten, gehaertet) --- */
    int yuv_ok = 0;
    if (vi_yuyv_to_rgb(a42_yuyv, sizeof(a42_yuyv), 4, 1, a42_rgb) == 0) {
        unsigned long pl = 4;
        yuv_ok = approx(a42_rgb[0 * pl + 0], 1.0f)       /* R: 128+1.402*127 -> clamp 1.0 */
              && approx(a42_rgb[1 * pl + 0], 0.146f)     /* G: Chroma-Term */
              && approx(a42_rgb[2 * pl + 0], 0.502f);    /* B: ~128/255 (U neutral) */
    }
    int yuv_rej = (vi_yuyv_to_rgb(a42_yuyv, 4, 4, 1, a42_rgb) < 0);   /* len 4 < need 8 -> Ablehnung */
    int a42_ok = yuv_ok && yuv_rej;
    if (a42_ok) {
        uwrite("[aivision] A4.2 yuyv-decode: farbe(R-clamp+chroma)=ok reject-short=ok\n");
    } else {
        uwrite("[aivision] A4.2 FEHLER: farbe="); uwrite(yuv_ok ? "ok" : "x");
        uwrite(" reject="); uwrite(yuv_rej ? "ok" : "x"); uwrite("\n");
    }

    /* --- A5.1: Echtzeit-Pipeline-Körper (Frame -> Conv-Detektor -> Heatmap-Peak -> Box-Overlay) --- */
    for (int i = 0; i < 16 * 16; i++) { a51_frame[i] = 0.0f; }
    for (int r = 4; r < 8; r++) { for (int c = 8; c < 12; c++) { a51_frame[r * 16 + c] = 1.0f; } }  /* helles 4x4-Quadrat */
    for (int i = 0; i < 16; i++) { a51_w[i] = 1.0f / 16.0f; }
    vi_conv2d(1, 16, 16,  1, 4, 4, 4, 0,  a51_frame, a51_w, a51_bias, a51_heat, a51_scratch);   /* Detektor-Heatmap 4x4 */
    int np = vi_decode_peaks(a51_heat, 4, 4, 0.5f, 4, a51_boxes, 16);
    int p_det = (np == 1) && a51_boxes[0].x == 8 && a51_boxes[0].y == 4 && a51_boxes[0].w == 4 && a51_boxes[0].h == 4;
    for (int i = 0; i < 16 * 16; i++) { a51_rgb[i] = 0; }
    if (np == 1) { vi_draw_box(a51_rgb, 16, 16, 16, a51_boxes[0], 0xFF00FF00u); }   /* gruener Rahmen ums Objekt */
    int p_draw = (a51_rgb[4 * 16 + 8] == 0xFF00FF00u)         /* Box oben-links (8,4) */
              && (a51_rgb[7 * 16 + 11] == 0xFF00FF00u);       /* unten-rechts (11,7) */
    int a51_ok = p_det && p_draw;
    if (a51_ok) {
        uwrite("[aivision] A5.1 pipeline: frame->conv-detektor->heatmap-peak->box(8,4)->overlay=ok\n");
    } else {
        uwrite("[aivision] A5.1 FEHLER: det="); uwrite(p_det ? "ok" : "x");
        uwrite(" draw="); uwrite(p_draw ? "ok" : "x"); uwrite("\n");
    }

    /* --- A5: Echtzeit-Schleife (Kamera-Grab-Seam + Fallback-Pipeline x3) --- */
    long cg = sys3(SYS_VI_CAM_GRAB, (long)a5_frame, (long)sizeof(a5_frame), 0);
    int cam_seam = (cg < 0);                             /* QEMU: kein UVC-Geraet -> Fallback-Frame */
    int loops = 0;
    for (int it = 0; it < 3; it++) {                     /* Loop-Körper: Frame -> Detektor -> Boxen */
        vi_conv2d(1, 16, 16,  1, 4, 4, 4, 0,  a51_frame, a51_w, a51_bias, a51_heat, a51_scratch);
        int n = vi_decode_peaks(a51_heat, 4, 4, 0.5f, 4, a51_boxes, 16);
        if (n == 1 && a51_boxes[0].x == 8) { loops++; }
    }
    int a5_ok = cam_seam && (loops == 3);
    if (a5_ok) {
        uwrite("[aivision] A5 loop: cam-grab=kein-geraet(qemu-fallback) pipeline-loop x3=ok\n");
    } else {
        uwrite("[aivision] A5 FEHLER: cam="); uwrite(cam_seam ? "ok" : "x");
        uwrite(" loops="); uwr_u((unsigned long)(long)loops); uwrite("\n");
    }

    /* --- A5.2: FPS-Mess-Infrastruktur (monotoner CNTPCT + CNTFRQ; QEMU: winzige Last -> degenerierter
     * Wert; am Pi4 mit Kamera: echte Capture+Verarbeitungs-Rate ueber dieselbe Messung). --- */
    unsigned long frq_hz = (unsigned long)sys1(SYS_VI_TICKS, 1);   /* Frequenz vom Kernel (EL1-Lesung) */
    unsigned long tk0 = (unsigned long)sys0(SYS_VI_TICKS);
    for (int it = 0; it < 30; it++) {
        vi_conv2d(1, 16, 16,  1, 4, 4, 4, 0,  a51_frame, a51_w, a51_bias, a51_heat, a51_scratch);
        vi_decode_peaks(a51_heat, 4, 4, 0.5f, 4, a51_boxes, 16);
    }
    unsigned long tk1 = (unsigned long)sys0(SYS_VI_TICKS);
    unsigned long dtk = (tk1 > tk0) ? (tk1 - tk0) : 1ul;
    unsigned long fps = (frq_hz > 0) ? (30ul * frq_hz) / dtk : 0ul;
    int a52_ok = (tk1 >= tk0) && (frq_hz > 0);               /* Infra verifiziert (Zeitmessung funktioniert) */
    if (a52_ok) {
        uwrite("[aivision] A5.2 fps-infra: ticks-monoton=ok cntfrq=");
        uwr_u(frq_hz);
        uwrite(" fps~=");
        uwr_u(fps);
        uwrite("\n");
    } else {
        uwrite("[aivision] A5.2 FEHLER: fps-infra (mono/frq)\n");
    }

    int all_ok = ok0 && tailok && conv_ok && orient_ok && dw_ok && a13_ok && a14a_ok && run_ok && a16_ok && a15_ok && m0_ok && a2_ok && a3_ok && a42_ok && a51_ok && a5_ok && a52_ok;
    uwrite("[aivision] engine-selftest fertig\n");
    sys3(SYS_EXIT, all_ok ? 0 : 1, 0, 0);
    for (;;) { }
}
