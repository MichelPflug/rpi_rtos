/*
 * user/lib/vision/vi_img.c  --  gehaerteter 24-bit-BMP-Loader.
 *
 * Untrusted hdd1-Daten: Header (14 B File + >=40 B DIB) und Pixel-Region werden overflow-frei
 * gegen die Blob-Laenge geprueft, bevor gelesen wird. Nur 24 bpp, unkomprimiert. Ausgabe als
 * out[3][H][W] (channel-major R/G/B, [0,1]); BMP ist bottom-up (h>0) bzw. top-down (h<0).
 */
#include "vi_img.h"

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int vi_bmp_load(const uint8_t *data, unsigned long len,
                float *out, int max_h, int max_w, int *out_h, int *out_w)
{
    if (!data || len < 54ul) { return -1; }                  /* 14 File-Header + 40 DIB */
    if (data[0] != 'B' || data[1] != 'M') { return -1; }

    unsigned long pix_off = rd_le32(data + 10);
    uint32_t dib = rd_le32(data + 14);
    if (dib < 40u) { return -1; }
    int32_t  w   = (int32_t)rd_le32(data + 18);
    int32_t  h   = (int32_t)rd_le32(data + 22);
    uint16_t planes = rd_le16(data + 26);
    uint16_t bpp    = rd_le16(data + 28);
    uint32_t comp   = rd_le32(data + 30);
    if (planes != 1 || bpp != 24 || comp != 0) { return -1; }

    int top_down = 0;
    if (h < 0) { top_down = 1; h = -h; }
    if (w <= 0 || h <= 0 || w > max_w || h > max_h) { return -1; }

    unsigned long row = (((unsigned long)w * 3ul + 3ul) / 4ul) * 4ul;   /* 4-Byte-Zeilenpadding */
    if (pix_off > len) { return -1; }
    unsigned long need = row * (unsigned long)h;                        /* overflow-frei: h<=max_h */
    if (need > len - pix_off) { return -1; }

    int W = w, H = h;
    unsigned long plane = (unsigned long)H * (unsigned long)W;
    const float inv255 = 1.0f / 255.0f;
    for (int y = 0; y < H; y++) {
        int src_y = top_down ? y : (H - 1 - y);              /* bottom-up: erste Datei-Zeile = unten */
        const uint8_t *r = data + pix_off + (unsigned long)src_y * row;
        for (int x = 0; x < W; x++) {
            const uint8_t *px = r + (unsigned long)x * 3ul;  /* BGR */
            unsigned long idx = (unsigned long)y * (unsigned long)W + (unsigned long)x;
            out[0 * plane + idx] = (float)px[2] * inv255;     /* R */
            out[1 * plane + idx] = (float)px[1] * inv255;     /* G */
            out[2 * plane + idx] = (float)px[0] * inv255;     /* B */
        }
    }
    if (out_h) { *out_h = H; }
    if (out_w) { *out_w = W; }
    return 0;
}

int vi_yuyv_to_rgb(const uint8_t *data, unsigned long len, int w, int h, float *out)
{
    if (!data || w <= 0 || h <= 0 || (w & 1)) { return -1; }        /* YUYV: 2 Pixel je Makropixel */
    unsigned long need = (unsigned long)w * (unsigned long)h * 2ul; /* 2 Byte/Pixel, overflow-frei */
    if (len < need) { return -1; }
    unsigned long plane = (unsigned long)w * (unsigned long)h;
    const float inv255 = 1.0f / 255.0f;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = data + (unsigned long)y * (unsigned long)w * 2ul;
        for (int x = 0; x < w; x += 2) {
            const uint8_t *mp = row + (unsigned long)x * 2ul;      /* Y0 U Y1 V */
            int Y0 = mp[0], U = mp[1], Y1 = mp[2], V = mp[3];
            float cu = (float)(U - 128), cv = (float)(V - 128);
            for (int p = 0; p < 2; p++) {
                float Yv = (float)(p ? Y1 : Y0);
                float r = Yv + 1.402f * cv;
                float g = Yv - 0.344f * cu - 0.714f * cv;
                float b = Yv + 1.772f * cu;
                r = r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r);
                g = g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g);
                b = b < 0.0f ? 0.0f : (b > 255.0f ? 255.0f : b);
                unsigned long idx = (unsigned long)y * (unsigned long)w + (unsigned long)(x + p);
                out[0 * plane + idx] = r * inv255;
                out[1 * plane + idx] = g * inv255;
                out[2 * plane + idx] = b * inv255;
            }
        }
    }
    return 0;
}
