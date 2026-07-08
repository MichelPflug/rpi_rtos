/*
 * user/lib/vision/vi_img.h  --  Bild-Loader: 24-bit-BMP -> float-Tensor.
 *
 * BMP-Dateien liegen auf hdd1 und sind damit UNTRUSTED -- der Loader prueft Format, Masse
 * und Pixel-Offset overflow-frei, bevor er liest. Ergebnis ist
 * ein channel-major float-Tensor out[3][H][W] (R/G/B-Ebenen), normiert auf [0,1].
 */
#ifndef RPI_RTOS_VI_IMG_H
#define RPI_RTOS_VI_IMG_H

#include <stdint.h>

/*
 * Laedt ein 24-bit unkomprimiertes BMP. out muss >= 3*max_h*max_w floats fassen.
 * Rueckgabe 0 = ok (H nach *out_h, W nach *out_w), <0 = abgelehnt (Format/zu gross/abgeschnitten).
 */
int vi_bmp_load(const uint8_t *data, unsigned long len,
                float *out, int max_h, int max_w, int *out_h, int *out_w);

/*
 * YUYV/YUY2 (4:2:2, Bytefolge Y0 U Y1 V je 2 Pixel) -> out[3][h][w] (channel-major, [0,1]).
 * BT.601-Farbraum + Clamping. Untrusted Kamera-Daten -> bounds-checked (len >= w*h*2, w gerade,
 * out muss >= 3*w*h floats fassen). Rueckgabe 0 = ok, <0 = abgelehnt. (A4.2: der QEMU-verifizierbare
 * Teil des Kamera-Pfads; MJPEG/JPEG-Decode ist ein spaeterer additiver Schritt.)
 */
int vi_yuyv_to_rgb(const uint8_t *data, unsigned long len, int w, int h, float *out);

#endif /* RPI_RTOS_VI_IMG_H */
