/*
 * user/lib/vision/vi_embed.c  --  Embedding-Metrik + Anomalie.
 *
 * Reine EL0-Rechnung (fp32). `__builtin_sqrtf` -> fsqrt-Instruktion (kein libm). Gehoert zum
 * gekapselten Vision-Modul (nur -Vision).
 */
#include "vi_embed.h"

void vi_l2_normalize(float *v, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) { s += v[i] * v[i]; }
    float inv = (s > 0.0f) ? (1.0f / __builtin_sqrtf(s)) : 0.0f;
    for (int i = 0; i < n; i++) { v[i] *= inv; }
}

float vi_l2_dist2(const float *a, const float *b, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) { float d = a[i] - b[i]; s += d * d; }
    return s;
}

int vi_nearest(const float *emb, const float *known, int n_known, int dim, float *out_d2)
{
    int best = -1;
    float bd = 0.0f;
    for (int k = 0; k < n_known; k++) {
        float d = vi_l2_dist2(emb, known + (long)k * dim, dim);
        if (best < 0 || d < bd) { best = k; bd = d; }
    }
    if (out_d2) { *out_d2 = bd; }
    return best;
}

int vi_anomaly(const float *emb, const float *known, int n_known, int dim,
               float thresh2, int *idx, float *out_d2)
{
    float d2 = 0.0f;
    int nn = vi_nearest(emb, known, n_known, dim, &d2);
    if (idx)    { *idx = nn; }
    if (out_d2) { *out_d2 = d2; }
    return (nn >= 0 && d2 <= thresh2) ? 0 : 1;   /* 0 = known, 1 = anomaly */
}
