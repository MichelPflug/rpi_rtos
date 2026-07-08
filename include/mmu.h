/*
 * include/mmu.h  --  MMU-Initialisierung
 */
#ifndef RPI_RTOS_MMU_H
#define RPI_RTOS_MMU_H

/*
 * Richtet eine Identity-Map ein und aktiviert MMU + Daten-/Instruktions-Caches.
 */
void mmu_init(void);

/* Diagnose/Guardian: gemappte Normal-RAM-Bytes bzw. ob die Groesse aus dem
 * DTB /memory kam (1) oder die Grobkarte genutzt wurde (0, z.B. QEMU). */
uint64_t mmu_ram_mapped(void);
int      mmu_ram_from_dtb(void);

/* Per-Core-Variante fuer Sekundaerkerne: aktiviert MMU + Caches mit den BEREITS von
 * mmu_init() (Core 0) aufgebauten, inner-shareable Page-Tables (MAIR/TCR/TTBR0/SCTLR),
 * ohne die Tabellen neu zu bauen. Voraussetzung: mmu_init() lief auf Core 0. */
void mmu_init_secondary(void);

/* --- Per-Prozess-Adressraeume (SMP: jeder Kern kann via eigenem TTBR0 einen anderen
 * User-Prozess fahren; User-Kachel ist nG/ASID-getaggt, Kernel-Seiten sind global) --- */

/* Legt einen Adressraum an (Kernel global + User-Kachel USER_BASE -> 'phys', EL0 RW).
 * Liefert die Aspace-ID (>=0) oder -1 (Pool voll). */
int      mmu_create_aspace(uint64_t phys, int map_gui_fb);

/* Gibt einen Adressraum frei (broadcast-invalidiert dessen ASID vor Reuse). */
void     mmu_free_aspace(int aspace);

/* TTBR0-Wert eines Aspace (Tabellenbasis + ASID); bzw. der Kernel-Adressraum (ASID 0). */
uint64_t mmu_aspace_ttbr(int aspace);
uint64_t mmu_kernel_ttbr(void);

/* TTBR0_EL1 auf 'ttbr' umschalten (kein Flush noetig -- ASID-getaggt + global). */
void     mmu_switch(uint64_t ttbr);

#endif /* RPI_RTOS_MMU_H */
