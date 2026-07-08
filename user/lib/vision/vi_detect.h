/*
 * user/lib/vision/vi_detect.h  --  Detektions-Pipeline
 *
 * Der Detektor-Kopf laesst ein CNN (vi_model_run) eine Heatmap ausgeben; diese Pipeline
 * dekodiert Peaks zu Boxen, unterdrueckt Ueberlappungen (NMS) und zeichnet die Rahmen. Das
 * echte trainierte Detektor-Modell ist ein spaeterer Daten-/Trainings-Schritt (gen_model.py);
 * die hier gebaute Pipeline ist backend-agnostisch + deterministisch verifizierbar.
 */
#ifndef RPI_RTOS_VI_DETECT_H
#define RPI_RTOS_VI_DETECT_H

#include <stdint.h>

typedef struct { int x, y, w, h; float score; } vi_box_t;

/* Zellen >= thresh im HxW-Heatmap zu je einer (cell x cell)-Box dekodieren. Rueckgabe: Anzahl (<= max). */
int  vi_decode_peaks(const float *heat, int H, int W, float thresh, int cell, vi_box_t *out, int max);

/* Non-Maximum-Suppression: nach score absteigend sortieren, Boxen mit IoU > iou_thresh gegen eine
 * hoeher bewertete unterdruecken, Ueberlebende nach vorn kompaktieren. Rueckgabe: Anzahl Ueberlebende. */
int  vi_nms(vi_box_t *boxes, int n, float iou_thresh);

/* Box-Rahmen (Farbe 0xAARRGGBB) in fb (row-major, pitch Pixel/Zeile) zeichnen, mit Clipping. */
void vi_draw_box(uint32_t *fb, int fbw, int fbh, int pitch, vi_box_t b, uint32_t color);

#endif /* RPI_RTOS_VI_DETECT_H */
