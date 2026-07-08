/*
 * user/lib/vision/vi_model.h  --  Modell-Blob-Format (*.net) + gehaerteter Loader.
 *
 * Ein *.net-Blob beschreibt ein sequentielles CNN (Layer-Kette). Die Blobs liegen auf hdd1
 * und sind damit **UNTRUSTED** -- der Loader parst sie mit STRIKTEN Grenzpruefungen (jeder
 * Offset/jede Laenge wird VOR dem Zugriff overflow-frei validiert), analog zur SPIR-V-Haertung.
 * Kein Zeiger/Index wird aus Blob-Daten ungeprueft gebildet.
 *
 * Format (little-endian, alle Felder u32 sofern nicht anders):
 *   Header:  magic('VNET') | version | n_layers | in_c | in_h | in_w
 *   je Layer: type | p[5] | w_count | b_count | <w_count float32> | <b_count float32>
 * Gewichte beginnen stets 4-aligned (Header 24 B, Layer-Kopf 32 B, je float 4 B).
 */
#ifndef RPI_RTOS_VI_MODEL_H
#define RPI_RTOS_VI_MODEL_H

#include <stdint.h>

#define VI_NET_MAGIC   0x54454E56u   /* 'V','N','E','T' little-endian */
#define VI_NET_VERSION 1u
#define VI_MAX_LAYERS  32

enum {
    VI_L_CONV = 1, VI_L_DWCONV = 2, VI_L_RELU = 3, VI_L_RELU6 = 4,
    VI_L_MAXPOOL = 5, VI_L_AVGPOOL = 6, VI_L_GAP = 7, VI_L_FC = 8, VI_L_SOFTMAX = 9
};

typedef struct {
    uint32_t type;
    uint32_t p[5];               /* typ-spezifische Parameter (z.B. Conv: Cout,KH,KW,stride,pad) */
    uint32_t w_count, b_count;   /* Anzahl float32-Gewichte / -Bias */
    const float *w, *b;          /* Zeiger IN den Blob (erst nach Validierung gesetzt) */
} vi_layer_t;

typedef struct {
    uint32_t n_layers, in_c, in_h, in_w;
    vi_layer_t layers[VI_MAX_LAYERS];
} vi_model_t;

/* Parst + VALIDIERT einen untrusted Blob. 0 = ok, <0 = abgelehnt (fail-loud, nie OOB/Overflow). */
int vi_model_load(const uint8_t *blob, unsigned long len, vi_model_t *m);

/*
 * Fuehrt das geladene Modell auf input[in_c*in_h*in_w] aus (sequentielle Layer-Kette ueber die
 * vi_engine-Primitive). buf_a/buf_b: je >= groesster Zwischentensor (Ping-Pong-Puffer); im2col:
 * Scratch fuer conv2d (>= max Cin*KH*KW * OH*OW). Das Ergebnis liegt am Ende in buf_a ODER buf_b
 * -> Rueckgabezeiger; *out_len erhaelt die Ausgabe-Elementzahl.
 */
const float *vi_model_run(const vi_model_t *m, const float *input,
                          float *buf_a, float *buf_b, float *im2col, int *out_len);

#endif /* RPI_RTOS_VI_MODEL_H */
