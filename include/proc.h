/*
 * include/proc.h  --  User-Prozess-Layout & Ausfuehrung
 */
#ifndef RPI_RTOS_PROC_H
#define RPI_RTOS_PROC_H

#include <stdint.h>

/* Feste User-Region (eine 2-MiB-Kachel, identitaetsgemappt, EL0-zugaenglich).
 * Die User-App wird auf USER_BASE gelinkt; Code/Daten unten, Stack von oben. */
#define USER_BASE       0x10000000UL
#define USER_SIZE       0x00800000UL          /* 8 MiB pro Prozess (war 2 MiB; Vulkan-Runtime
                                                * VKTEST braucht ~2 MiB Image + Stack -> 2 MiB Tile
                                                * lief in Stack/BSS-Kollision. Muss Vielfaches von
                                                * 2 MiB sein: mmu_create_aspace mappt USER_SIZE>>21
                                                * L2-Bloecke. 4 Prozesse x 8 MiB ab USER_PHYS_BASE
                                                * (512 MiB) => [512,544) MiB, RAM-gedeckt.) */
#define USER_STACK_TOP  (USER_BASE + USER_SIZE)

/* Jeder Prozess erhaelt einen eigenen physischen 2-MiB-Bereich ab hier; im per-Prozess-
 * Adressraum (eigenes TTBR0/ASID) zeigt die User-VA USER_BASE darauf. */
#define USER_PHYS_BASE  0x20000000UL          /* 512 MiB */
#define MAX_USER_PROCS  4

/* GUI-Grafik-Bruecke: der Framebuffer-Backbuffer wird an dieser festen EL0-VA gemappt --
 * NUR fuer Prozesse mit USER_CAP_GUI (eine 2-MiB-Kachel, RW+UXN, nG). Bewusste Wahl bei 384 MiB:
 *  - NICHT im VideoCore-Carveout-Bereich (oberes 1. GiB, ~0x30000000..0x40000000) -> kollidiert
 *    nie mit der firmware-gewaehlten FB-Basis (die per & 0x3FFFFFFF in die untere 1 GiB faellt);
 *  - der zugehoerige L2-Eintrag wird im Kernel-Aspace ausgehoehlt (wie USER_BASE) -> kein
 *    global+nG-TLB-Konflikt; der Kernel nutzt weder VA noch phys 0x18000000. */
#define GUI_FB_USER_VA  0x18000000UL          /* 384 MiB */

/* Laedt ein ELF64-Programm via VFS, mappt die User-Region EL0-zugaenglich und
 * legt einen Task an, der nach EL0 wechselt. Liefert die tid (>=0) / <0.
 * proc_exec laeuft als root (uid 0, alle Caps); proc_exec_as setzt den
 * angegebenen Credential (uid/caps) -> Least Privilege fuer den EL0-Prozess. */
int proc_exec(const char *path);
int proc_exec_as(const char *path, uint32_t uid, uint32_t caps);

/* Wie proc_exec_as, bindet den Prozess aber an einen bestimmten Kern (SMP-Affinitaet) ->
 * EL0-Prozess laeuft auf einem Sekundaerkern. 'ppid' = Eltern-PID (0 = kernel-gestartet, keine
 * wait/kill-Beziehung); 'out_pid' (falls != NULL) erhaelt die monotone PID des Kindes, erfasst
 * BEVOR der Task schedulebar wird (reap-race-frei -- SYS_SPAWN gibt sie an EL0 zurueck). */
int proc_exec_as_on(uint32_t core, const char *path, uint32_t uid, uint32_t caps,
                    uint64_t ppid, uint64_t *out_pid);

#endif /* RPI_RTOS_PROC_H */
