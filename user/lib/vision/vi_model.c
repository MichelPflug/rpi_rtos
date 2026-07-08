/*
 * user/lib/vision/vi_model.c  --  gehaerteter *.net-Blob-Loader.
 *
 * Der Blob ist UNTRUSTED (hdd1). Jeder Lesezugriff laeuft ueber rd_u32(), das VOR dem Zugriff
 * prueft: (a) off <= len, (b) len-off >= 4 -- beides overflow-frei (kein off+4, das ueberlaufen
 * koennte). Gewichts-/Bias-Zahlen werden gegen die im Blob VERBLEIBENDEN float-Slots geprueft,
 * bevor Zeiger gesetzt werden. So kann ein bösartiger Blob (falsches Magic, abgeschnitten,
 * Riesen-w_count, unbekannter Layer-Typ, zu viele Layer) nur zu einer Ablehnung führen, nie zu
 * einem OOB-Read oder Integer-Overflow.
 */
#include "vi_model.h"
#include "vi_engine.h"

/* Liest ein little-endian u32 an *off, wenn 4 Bytes verfuegbar sind; schiebt *off weiter. */
static int rd_u32(const uint8_t *b, unsigned long len, unsigned long *off, uint32_t *out)
{
    unsigned long o = *off;
    if (o > len || len - o < 4ul) { return -1; }         /* overflow-frei geprueft */
    *out = (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8)
         | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
    *off = o + 4ul;
    return 0;
}

int vi_model_load(const uint8_t *blob, unsigned long len, vi_model_t *m)
{
    unsigned long off = 0;
    uint32_t magic, ver;

    if (!blob || !m) { return -1; }
    if (rd_u32(blob, len, &off, &magic) || magic != VI_NET_MAGIC)   { return -1; }
    if (rd_u32(blob, len, &off, &ver)   || ver   != VI_NET_VERSION) { return -1; }
    if (rd_u32(blob, len, &off, &m->n_layers))       { return -1; }
    if (m->n_layers > VI_MAX_LAYERS)                 { return -1; }  /* Layer-Zahl gekappt */
    if (rd_u32(blob, len, &off, &m->in_c)
     || rd_u32(blob, len, &off, &m->in_h)
     || rd_u32(blob, len, &off, &m->in_w))           { return -1; }

    for (uint32_t i = 0; i < m->n_layers; i++) {
        vi_layer_t *L = &m->layers[i];
        if (rd_u32(blob, len, &off, &L->type))       { return -1; }
        if (L->type < VI_L_CONV || L->type > VI_L_SOFTMAX) { return -1; }   /* Typ validiert */
        for (int k = 0; k < 5; k++) {
            if (rd_u32(blob, len, &off, &L->p[k]))   { return -1; }
        }
        if (rd_u32(blob, len, &off, &L->w_count)
         || rd_u32(blob, len, &off, &L->b_count))    { return -1; }

        /* Gewichts-/Bias-Floats muessen in den verbleibenden Blob passen (overflow-frei:
         * Division statt Multiplikation, unsigned long ist 64-bit -> w_count+b_count faellt
         * nie um). */
        unsigned long avail = (len - off) / 4ul;
        unsigned long need  = (unsigned long)L->w_count + (unsigned long)L->b_count;
        if (need > avail) { return -1; }

        L->w = L->w_count ? (const float *)(blob + off) : (const float *)0;
        off += (unsigned long)L->w_count * 4ul;
        L->b = L->b_count ? (const float *)(blob + off) : (const float *)0;
        off += (unsigned long)L->b_count * 4ul;
    }
    return 0;
}

const float *vi_model_run(const vi_model_t *m, const float *input,
                          float *buf_a, float *buf_b, float *im2col, int *out_len)
{
    int C = (int)m->in_c, H = (int)m->in_h, W = (int)m->in_w;
    int n = C * H * W;
    for (int i = 0; i < n; i++) { buf_a[i] = input[i]; }   /* Eingabe in den Ping-Pong holen */
    float *cur = buf_a, *oth = buf_b;

    for (uint32_t li = 0; li < m->n_layers; li++) {
        const vi_layer_t *L = &m->layers[li];
        switch (L->type) {
        case VI_L_CONV: {
            int Cout=(int)L->p[0], KH=(int)L->p[1], KW=(int)L->p[2], st=(int)L->p[3], pad=(int)L->p[4];
            vi_conv2d(C, H, W, Cout, KH, KW, st, pad, cur, L->w, L->b, oth, im2col);
            C = Cout; H = (H + 2*pad - KH)/st + 1; W = (W + 2*pad - KW)/st + 1;
            float *t = cur; cur = oth; oth = t;
            break; }
        case VI_L_DWCONV: {
            int KH=(int)L->p[0], KW=(int)L->p[1], st=(int)L->p[2], pad=(int)L->p[3];
            vi_dwconv(C, H, W, KH, KW, st, pad, cur, L->w, L->b, oth);
            H = (H + 2*pad - KH)/st + 1; W = (W + 2*pad - KW)/st + 1;
            float *t = cur; cur = oth; oth = t;
            break; }
        case VI_L_RELU:    vi_relu(cur, C*H*W);  break;
        case VI_L_RELU6:   vi_relu6(cur, C*H*W); break;
        case VI_L_SOFTMAX: vi_softmax(cur, C*H*W); break;
        case VI_L_MAXPOOL: {
            int KH=(int)L->p[0], KW=(int)L->p[1], st=(int)L->p[2];
            vi_maxpool2d(C, H, W, KH, KW, st, cur, oth);
            H = (H - KH)/st + 1; W = (W - KW)/st + 1;
            float *t = cur; cur = oth; oth = t;
            break; }
        case VI_L_AVGPOOL: {
            int KH=(int)L->p[0], KW=(int)L->p[1], st=(int)L->p[2];
            vi_avgpool2d(C, H, W, KH, KW, st, cur, oth);
            H = (H - KH)/st + 1; W = (W - KW)/st + 1;
            float *t = cur; cur = oth; oth = t;
            break; }
        case VI_L_GAP: {
            vi_global_avgpool(C, H, W, cur, oth);
            H = 1; W = 1;
            float *t = cur; cur = oth; oth = t;
            break; }
        case VI_L_FC: {
            int outf = (int)L->p[0], inf = C * H * W;
            vi_sgemm(outf, 1, inf, L->w, cur, oth);         /* [outf x 1] = W[outf x inf] * x[inf x 1] */
            if (L->b) { for (int i = 0; i < outf; i++) { oth[i] += L->b[i]; } }
            C = outf; H = 1; W = 1;
            float *t = cur; cur = oth; oth = t;
            break; }
        default: break;
        }
    }
    if (out_len) { *out_len = C * H * W; }
    return cur;
}
