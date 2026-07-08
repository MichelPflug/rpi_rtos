/*
 * user/lib/vision/vi_engine.h  --  Oeffentliche API der Vision-Inferenz-Engine.
 *
 * Duenn im ersten Inkrement: das ueber das Standard-Backend (NEON-fp32) laufende sgemm.
 * erweitert um conv2d/dwconv/Aktivierungen/Pooling/Softmax -- alle auf sgemm bzw.
 * die vi_backend_t-Grenze abgestuetzt.
 */
#ifndef RPI_RTOS_VI_ENGINE_H
#define RPI_RTOS_VI_ENGINE_H

#include "vi_backend.h"

/* C[MxN] = A[MxK] * B[KxN], row-major fp32, ueber das Standard-Backend (NEON). */
void vi_sgemm(int M, int N, int K, const float *A, const float *B, float *C);

/* Wie vi_sgemm, aber rechnet NUR die Ausgabe-Zeilen [r0, r1) -- Bausteine fuer die 4-Kern-
 * Aufteilung (A1.5). Ergebnis bit-identisch zum vollen vi_sgemm ueber alle Bloecke. */
void vi_sgemm_rows(int M, int N, int K, const float *A, const float *B, float *C, int r0, int r1);

/*
 * 2D-Faltung (ein Batch) via im2col -> sgemm. Layouts (alle row-major):
 *   in      [Cin][H][W]              (channel-major)
 *   weights [Cout][Cin][KH][KW]
 *   bias    [Cout]                   (oder NULL)
 *   out     [Cout][OH][OW],  OH=(H+2*pad-KH)/stride+1, OW analog
 *   scratch >= (Cin*KH*KW)*(OH*OW) floats  (im2col-Matrix; vom Aufrufer gestellt --
 *                                           die Engine allokiert nie selbst)
 */
void vi_conv2d(int Cin, int H, int W,
               int Cout, int KH, int KW, int stride, int pad,
               const float *in, const float *weights, const float *bias,
               float *out, float *scratch);

/*
 * Depthwise-Faltung: je Eingabekanal EIN Kernel (Gruppen=Cin, Cout==Cin) -- die
 * MobileNet-Grundoperation. weights [C][KH][KW], bias [C] oder NULL. Direkt gerechnet.
 */
void vi_dwconv(int C, int H, int W,
               int KH, int KW, int stride, int pad,
               const float *in, const float *weights, const float *bias,
               float *out);

/* --- Aktivierungen / Pooling / Softmax / BatchNorm (alle in-place bzw. out-Puffer) --- */

void vi_relu(float *x, int n);                  /* max(0,x)            */
void vi_relu6(float *x, int n);                 /* clamp(x, 0, 6)      */
void vi_sigmoid(float *x, int n);               /* 1/(1+exp(-x))       */
void vi_softmax(float *x, int n);              /* stabile Softmax ueber n Logits, in-place */

/* Pooling. in [C][H][W]; out [C][OH][OW] mit OH=(H-KH)/stride+1 (kein Padding), OW analog. */
void vi_maxpool2d(int C, int H, int W, int KH, int KW, int stride, const float *in, float *out);
void vi_avgpool2d(int C, int H, int W, int KH, int KW, int stride, const float *in, float *out);
/* Global-Average-Pool: Mittel je Kanal ueber H*W -> out[C]. */
void vi_global_avgpool(int C, int H, int W, const float *in, float *out);

/* Inferenz-BatchNorm als Per-Kanal-Affine: x[c][i] = x[c][i]*scale[c] + shift[c]. HW = H*W. */
void vi_affine_per_channel(float *x, int C, int HW, const float *scale, const float *shift);
/* BatchNorm in scale/shift falten: scale=gamma/sqrt(var+eps), shift=beta-mean*scale (je Kanal). */
void vi_bn_fold(int C, const float *gamma, const float *beta,
                const float *mean, const float *var, float eps,
                float *scale, float *shift);

/* Name des Standard-Rechen-Backends (fuer Diagnose-/Selbsttest-Marker). */
const char *vi_backend_name(void);

#endif /* RPI_RTOS_VI_ENGINE_H */
