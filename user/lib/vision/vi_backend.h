/*
 * user/lib/vision/vi_backend.h  --  Rechen-Backend-Grenze des Vision-Moduls.
 *
 * Schmale Schnittstelle zwischen den Koepfen (Detektor/Embedding) und dem
 * Rechen-Backend -- exakt die Idee der Vulkan-Backend-Weiche,
 *
 * Teil des gekapselten Vision-Moduls (user/lib/vision/) -- nur im -Vision-Build kompiliert.
 */
#ifndef RPI_RTOS_VI_BACKEND_H
#define RPI_RTOS_VI_BACKEND_H

typedef struct vi_backend {
    const char *name;
    /* C[MxN] = A[MxK] * B[KxN], alle row-major, fp32. C wird vollstaendig geschrieben. */
    void (*sgemm)(int M, int N, int K, const float *A, const float *B, float *C);
} vi_backend_t;

/* Das NEON-fp32-Backend (user/lib/vision/vi_engine.c). */
const vi_backend_t *vi_backend_neon(void);

#endif /* RPI_RTOS_VI_BACKEND_H */
