/*
 * drivers/gpu/v3d_regs.h  --  VideoCore-VI-V3D-4.2-Register-Map (BCM2711, ARM-seitig MMIO).
 */
#ifndef RPI_RTOS_V3D_REGS_H
#define RPI_RTOS_V3D_REGS_H

/* BCM2711: der V3D-Block liegt im 0xFEC00000-Fenster (Low-Peripheral-Alias 0x0_FECxxxxx). Die V3D
 * hat eine HUB-Bank (Kern-uebergreifend: MMU/TFU/IDENT) + je-Core-Banken. Single-Core auf BCM2711. */
#define V3D_BASE            0xFEC00000UL

/* --- Bank-Basen (Offset vom V3D_BASE; HW-RE-zu-validieren) --- */
#define V3D_HUB_BASE        0x0000u   /* Hub-Register (IDENT, MMU, TFU) */
#define V3D_CORE0_BASE      0x4000u   /* Core-0-Register (CTL, CLE, PTB, PCTR, GMP, CSD) */

/* ================= HUB-Bank ================= */
/* IDENT: Technologie/Revision/Core-Anzahl (ASCII 'V','3','D' in IDENT0 unteren Bytes). */
#define V3D_HUB_IDENT0      (V3D_HUB_BASE + 0x0000u)
#define V3D_HUB_IDENT1      (V3D_HUB_BASE + 0x0004u)
#define V3D_HUB_IDENT2      (V3D_HUB_BASE + 0x0008u)
#define V3D_HUB_IDENT3      (V3D_HUB_BASE + 0x000Cu)

/* Hub-Interrupt (Job-fertig/MMU-Fault): STS/SET/CLR/MSK. */
#define V3D_HUB_INT_STS     (V3D_HUB_BASE + 0x0050u)
#define V3D_HUB_INT_SET     (V3D_HUB_BASE + 0x0054u)
#define V3D_HUB_INT_CLR     (V3D_HUB_BASE + 0x0058u)
#define V3D_HUB_INT_MSK_STS (V3D_HUB_BASE + 0x005Cu)
#define V3D_HUB_INT_MSK_SET (V3D_HUB_BASE + 0x0060u)
#define V3D_HUB_INT_MSK_CLR (V3D_HUB_BASE + 0x0064u)

/* GPU-MMU (Hub): Pagetable-Basis + Kontrolle (V5.1). Die GPU sieht BOs nur durch diese MMU. */
#define V3D_MMU_CTL         (V3D_HUB_BASE + 0x1200u)   /* enable, flush, abort-fault */
#define V3D_MMU_PT_PA_BASE  (V3D_HUB_BASE + 0x1204u)   /* Pagetable Physical Address >> 12 */
#define V3D_MMU_VIO_ADDR    (V3D_HUB_BASE + 0x1208u)   /* letzte Fault-Adresse (Diagnose) */
#define V3D_MMU_VIO_ID      (V3D_HUB_BASE + 0x120Cu)
#define V3D_MMU_DEBUG_INFO  (V3D_HUB_BASE + 0x1210u)
#define V3D_MMUC_CONTROL    (V3D_HUB_BASE + 0x1000u)   /* MMU-Cache: enable + flush */

/* TFU (Texture Formatting Unit, Hub): Textur-Upload/Umformatierung (spaeter, V5.3-Texturen). */
#define V3D_TFU_CS          (V3D_HUB_BASE + 0x0400u)
#define V3D_TFU_SU          (V3D_HUB_BASE + 0x0404u)
#define V3D_TFU_ICFG        (V3D_HUB_BASE + 0x0408u)
#define V3D_TFU_IIA         (V3D_HUB_BASE + 0x040Cu)

/* ================= Core-0-Bank ================= */
/* CTL: Core-Kontrolle/Ident + Core-Interrupt. */
#define V3D_CTL_IDENT0      (V3D_CORE0_BASE + 0x0000u)
#define V3D_CTL_IDENT1      (V3D_CORE0_BASE + 0x0004u)
#define V3D_CTL_IDENT2      (V3D_CORE0_BASE + 0x0008u)
#define V3D_CTL_MISCCFG     (V3D_CORE0_BASE + 0x0018u)
#define V3D_CTL_INT_STS     (V3D_CORE0_BASE + 0x0030u)
#define V3D_CTL_INT_SET     (V3D_CORE0_BASE + 0x0034u)
#define V3D_CTL_INT_CLR     (V3D_CORE0_BASE + 0x0038u)
#define V3D_CTL_INT_MSK_STS (V3D_CORE0_BASE + 0x003Cu)
#define V3D_CTL_INT_MSK_SET (V3D_CORE0_BASE + 0x0040u)
#define V3D_CTL_INT_MSK_CLR (V3D_CORE0_BASE + 0x0044u)
#define V3D_CTL_L2CACTL     (V3D_CORE0_BASE + 0x0020u)   /* L2C: enable/flush */
#define V3D_CTL_SLCACTL     (V3D_CORE0_BASE + 0x0024u)   /* Slices-Cache: flush uniforms/tmu/vcd */

/* CLE (Control-List-Executor): ZWEI Threads -- CT0 = Bin, CT1 = Render. Je Queue-Begin/-End-Adresse
 * (QBA/QEA) + Control/Status (CS) + aktuelle Adresse (CA). Ein Job = QBA/QEA setzen -> CLE laeuft die
 * Control-List zwischen den Adressen ab. Das ist die zentrale Submit-Naht (V5.2b). */
#define V3D_CLE_CT0CS       (V3D_CORE0_BASE + 0x0100u)   /* Bin: control/status (run/stop/reset) */
#define V3D_CLE_CT1CS       (V3D_CORE0_BASE + 0x0104u)   /* Render: control/status */
#define V3D_CLE_CT0EA       (V3D_CORE0_BASE + 0x0108u)   /* Bin: end address */
#define V3D_CLE_CT1EA       (V3D_CORE0_BASE + 0x010Cu)
#define V3D_CLE_CT0CA       (V3D_CORE0_BASE + 0x0110u)   /* Bin: current address (Fortschritt) */
#define V3D_CLE_CT1CA       (V3D_CORE0_BASE + 0x0114u)
#define V3D_CLE_CT0RA0      (V3D_CORE0_BASE + 0x0118u)   /* return address 0 (sub-list) */
#define V3D_CLE_CT1RA0      (V3D_CORE0_BASE + 0x011Cu)
#define V3D_CLE_CT0LC       (V3D_CORE0_BASE + 0x0120u)   /* list counter (Semaphore/Primitive) */
#define V3D_CLE_CT1LC       (V3D_CORE0_BASE + 0x0124u)
#define V3D_CLE_CT0PC       (V3D_CORE0_BASE + 0x0128u)   /* primitive counter */
#define V3D_CLE_CT1PC       (V3D_CORE0_BASE + 0x012Cu)
#define V3D_CLE_PCS         (V3D_CORE0_BASE + 0x0130u)   /* pipeline control/status (bin/render busy) */
#define V3D_CLE_BFC         (V3D_CORE0_BASE + 0x0134u)   /* bin flush count (Bin-Job fertig) */
#define V3D_CLE_RFC         (V3D_CORE0_BASE + 0x0138u)   /* render frame count (Render-Job fertig) */
#define V3D_CLE_CT0QBA      (V3D_CORE0_BASE + 0x0200u)   /* Bin: queue begin address (Submit) */
#define V3D_CLE_CT1QBA      (V3D_CORE0_BASE + 0x0204u)   /* Render: queue begin address */
#define V3D_CLE_CT0QEA      (V3D_CORE0_BASE + 0x0208u)   /* Bin: queue end address */
#define V3D_CLE_CT1QEA      (V3D_CORE0_BASE + 0x020Cu)

/* PTB (Primitive Tile Binner): Tile-State-Basis fuer den Bin-Durchlauf. */
#define V3D_PTB_BPCA        (V3D_CORE0_BASE + 0x0300u)   /* current tile-state address */
#define V3D_PTB_BPCS        (V3D_CORE0_BASE + 0x0304u)   /* current tile-state size */
#define V3D_PTB_BPOA        (V3D_CORE0_BASE + 0x0308u)   /* overspill address */
#define V3D_PTB_BPOS        (V3D_CORE0_BASE + 0x030Cu)   /* overspill size */

/* GMP (Global Memory Protection): grobkoernige Speicherschutz-Bitmap (vor voller MMU nutzbar). */
#define V3D_GMP_STATUS      (V3D_CORE0_BASE + 0x0800u)
#define V3D_GMP_CFG         (V3D_CORE0_BASE + 0x0804u)
#define V3D_GMP_TABLE_ADDR  (V3D_CORE0_BASE + 0x0808u)
#define V3D_GMP_CLEAR_LOAD  (V3D_CORE0_BASE + 0x080Cu)

/* --- ausgewaehlte Bit-Felder (HW-RE-zu-validieren) --- */
#define V3D_CT_CS_RUN       (1u << 5)   /* CTnCS: Thread laeuft */
#define V3D_CT_CS_RESET     (1u << 15)  /* CTnCS: Thread-Reset */
#define V3D_INT_FLDONE      (1u << 0)   /* Frame/List done (grob) */
#define V3D_INT_OUTOMEM     (1u << 1)   /* Bin: out of memory (Overspill noetig) */
#define V3D_MMU_CTL_ENABLE  (1u << 0)
#define V3D_MMU_CTL_PT_INVALIDATE (1u << 1)

#endif /* RPI_RTOS_V3D_REGS_H */
