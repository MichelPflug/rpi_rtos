/*
 * user/lib/vision/vi_embed.h  --  Embedding + Anomalie
 *
 * Der Embedding-Kopf laesst ein CNN (vi_model_run) ein Gesicht auf einen Vektor abbilden; hier
 * liegt die Metrik obendrauf: L2-Normierung, Distanz zum „bekannt"-Set (Enrollment) und die
 * known/unknown-Entscheidung (Anomalie = Distanz ueber Schwelle). Ein Netz, zwei Anwendungen
 * (Identitaet = naechster bekannter Vektor; Anomalie = zu weit von allen bekannten entfernt).
 */
#ifndef RPI_RTOS_VI_EMBED_H
#define RPI_RTOS_VI_EMBED_H

/* Vektor auf Einheitslaenge normieren (in-place). */
void  vi_l2_normalize(float *v, int n);

/* Quadrierte L2-Distanz zweier Vektoren. */
float vi_l2_dist2(const float *a, const float *b, int n);

/* Naechster bekannter Vektor (min L2^2). known = n_known x dim (row-major). Rueckgabe: Index
 * (<0 wenn n_known==0); *out_d2 = zugehoerige Distanz^2. */
int   vi_nearest(const float *emb, const float *known, int n_known, int dim, float *out_d2);

/* known wenn min-Distanz^2 <= thresh2, sonst Anomalie. Rueckgabe 0 = known, 1 = anomaly;
 * *idx = naechster bekannter Index, *out_d2 = dessen Distanz^2. */
int   vi_anomaly(const float *emb, const float *known, int n_known, int dim,
                 float thresh2, int *idx, float *out_d2);

#endif /* RPI_RTOS_VI_EMBED_H */
