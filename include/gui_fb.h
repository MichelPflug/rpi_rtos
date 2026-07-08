/*
 * include/gui_fb.h  --  Kernel-Seite der GUI-Grafik-Bruecke.
 *
 * Haelt einen 2-MiB-aligned Backbuffer, den die MMU in jeden EL0-Adressraum an GUI_FB_USER_VA
 * mappt. EL0-Apps zeichnen (cachend) in den Backbuffer; gui_fb_flush kopiert die betroffenen
 * Zeilen in den echten VideoCore-Framebuffer und pflegt dessen Cache fuer die GPU.
 */
#ifndef RPI_RTOS_GUI_FB_H
#define RPI_RTOS_GUI_FB_H

#include <stdint.h>
#include "gui_abi.h"

/* Groesse des GUI-Backbuffers in 2-MiB-Kacheln. Standard 1 Kachel (2 MiB) fuer 640x480x4 (1,2 MiB).
 * Mit -DFULLHD 5 Kacheln (10 MiB) fuer 1920x1080x4 (8,29 MiB) -> Full-HD-GUI. Die MMU mappt genau
 * diese Kacheln nach GUI_FB_USER_VA (mmu_create_aspace) und hoehlt sie im Kernel-Aspace aus. */
#ifdef FULLHD
#define GUI_BB_TILES 5u
#else
#define GUI_BB_TILES 1u
#endif
#define GUI_BB_SIZE  (GUI_BB_TILES * 0x200000u)

/* Physische (= Kernel-Identity-)Adresse des Backbuffers; 0 bevor der FB initialisiert ist.
 * Die MMU mappt die GUI_BB_TILES Kacheln ab hier nach GUI_FB_USER_VA. */
uint64_t gui_fb_phys(void);

/* Bindet den Backbuffer an den aktiven Framebuffer (Groesse pruefen). Nach fb_init aufrufen.
 * 0 = ok, -1 = kein FB / FB groesser als der Backbuffer. */
int gui_fb_init(void);

/* Fuellt die Geometrie + EL0-VA/Groesse des Backbuffers. 0 = ok, -1 = nicht bereit. */
int gui_fb_info(gui_fb_info_t *out);

/* Kopiert Backbuffer-Zeilen [y, y+nrows) in den echten Framebuffer + Cache-Flush (GPU).
 * y/nrows werden auf die FB-Hoehe geklemmt. 0 = ok, -1 = nicht bereit. */
int gui_fb_flush(uint32_t y, uint32_t nrows);

/* Maus-Cursor auf (x,y) bewegen (T2.3): der Kernel loescht den zuletzt gezeichneten Cursor
 * (altes Zeilenband aus dem Backbuffer restaurieren) und komponiert ihn am neuen Ort direkt in
 * den FB. Aus dem Timer-IRQ (Kern 0) aufgerufen; unabhaengig vom App-Flush -> keine Geisterspur. */
void gui_fb_move_cursor(int x, int y);

#ifdef RTOS_SELFTEST
/* Selbsttest der Bruecke: bekannte Farben in den Backbuffer schreiben, flushen, echte
 * FB-Pixel zuruecklesen + vergleichen. 1 = Kopie+Flush korrekt. */
int gui_fb_selftest(void);
/* Selbsttest des Cursor-Overlays: Cursor setzen, flushen, Kontur/Fuellung/Hintergrund pruefen. */
int gui_fb_cursor_selftest(void);
#endif

#endif /* RPI_RTOS_GUI_FB_H */
