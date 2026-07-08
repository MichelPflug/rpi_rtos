/*
 * user/lib/vision/vi_detect.c  --  Detektions-Pipeline.
 *
 * Backend-agnostisch: dekodiert eine Detektor-Heatmap zu Boxen, NMS, Rahmen-Zeichnen. Reine
 * EL0-Rechnung (FP nur fuer score/IoU). Gehoert zum gekapselten Vision-Modul (nur -Vision).
 */
#include "vi_detect.h"

int vi_decode_peaks(const float *heat, int H, int W, float thresh, int cell, vi_box_t *out, int max)
{
    int n = 0;
    for (int cy = 0; cy < H; cy++) {
        for (int cx = 0; cx < W; cx++) {
            float s = heat[cy * W + cx];
            if (s >= thresh && n < max) {
                out[n].x = cx * cell; out[n].y = cy * cell;
                out[n].w = cell;      out[n].h = cell;
                out[n].score = s;
                n++;
            }
        }
    }
    return n;
}

static float box_iou(const vi_box_t *a, const vi_box_t *b)
{
    int ax2 = a->x + a->w, ay2 = a->y + a->h;
    int bx2 = b->x + b->w, by2 = b->y + b->h;
    int ix1 = a->x > b->x ? a->x : b->x;
    int iy1 = a->y > b->y ? a->y : b->y;
    int ix2 = ax2 < bx2 ? ax2 : bx2;
    int iy2 = ay2 < by2 ? ay2 : by2;
    int iw = ix2 - ix1, ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) { return 0.0f; }
    float inter = (float)iw * (float)ih;
    float uni   = (float)(a->w * a->h) + (float)(b->w * b->h) - inter;
    return (uni > 0.0f) ? (inter / uni) : 0.0f;
}

int vi_nms(vi_box_t *boxes, int n, float iou_thresh)
{
    for (int i = 0; i < n; i++) {                        /* nach score absteigend (Selection-Sort) */
        int best = i;
        for (int j = i + 1; j < n; j++) { if (boxes[j].score > boxes[best].score) { best = j; } }
        if (best != i) { vi_box_t t = boxes[i]; boxes[i] = boxes[best]; boxes[best] = t; }
    }
    for (int i = 0; i < n; i++) {                        /* staerker Ueberlappende unterdruecken */
        if (boxes[i].score < 0.0f) { continue; }
        for (int j = i + 1; j < n; j++) {
            if (boxes[j].score < 0.0f) { continue; }
            if (box_iou(&boxes[i], &boxes[j]) > iou_thresh) { boxes[j].score = -1.0f; }
        }
    }
    int k = 0;                                           /* Ueberlebende kompaktieren */
    for (int i = 0; i < n; i++) {
        if (boxes[i].score >= 0.0f) { boxes[k++] = boxes[i]; }
    }
    return k;
}

static void put_px(uint32_t *fb, int fbw, int fbh, int pitch, int x, int y, uint32_t c)
{
    if (x >= 0 && x < fbw && y >= 0 && y < fbh) { fb[y * pitch + x] = c; }
}

void vi_draw_box(uint32_t *fb, int fbw, int fbh, int pitch, vi_box_t b, uint32_t color)
{
    for (int x = b.x; x < b.x + b.w; x++) {
        put_px(fb, fbw, fbh, pitch, x, b.y, color);
        put_px(fb, fbw, fbh, pitch, x, b.y + b.h - 1, color);
    }
    for (int y = b.y; y < b.y + b.h; y++) {
        put_px(fb, fbw, fbh, pitch, b.x, y, color);
        put_px(fb, fbw, fbh, pitch, b.x + b.w - 1, y, color);
    }
}
