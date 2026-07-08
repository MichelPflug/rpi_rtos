/*
 * include/v3d.h  --  VideoCore-VI-V3D-Hardware-Erkennung (Vulkan V5, erster Schritt).
 */
#ifndef RPI_RTOS_V3D_H
#define RPI_RTOS_V3D_H

/* Liest die V3D-IDENT-Register (MMIO) und meldet ueber Serial "[v3d] ...", ob eine V3D-GPU
 * vorhanden ist (am echten Pi4) bzw. nicht (QEMU). Der Startpunkt des V5-HW-Backends. */
void v3d_probe(void);

#endif /* RPI_RTOS_V3D_H */
