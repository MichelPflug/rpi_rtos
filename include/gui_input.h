/*
 * include/gui_input.h  --  Kernel-Eingabe-Event-Queue fuer GUI-Apps.
 *
 * Ein SPSC-Ring von gui_event_t: Producer = gui_input_tick (100-Hz-Timer-IRQ auf Kern 0, pollt die
 * Maus), Consumer = SYS_POLL_EVENT/SYS_WAIT_EVENT (EL0-GUI-Task). Feeds den Application.Run-Loop.
 */
#ifndef RPI_RTOS_GUI_INPUT_H
#define RPI_RTOS_GUI_INPUT_H

#include "gui_abi.h"

/* Maus-Poll im Timer-Tick aktivieren (nach usbmouse_enable). */
void gui_input_enable(void);

/* Tastatur-Poll im Timer-Tick aktivieren (GUI-Sitzung): jedes Konsolenzeichen -> GUI_EV_KEY. */
void gui_input_enable_kbd(void);

/* Aus dem 100-Hz-Timer-Tick (Kern 0): Maus pollen, bei Aenderung ein GUI_EV_MOUSE einreihen. */
void gui_input_tick(void);

/* Ein Event einreihen (Producer). Ring voll -> verworfen. 1 = eingereiht. */
int  gui_input_push(const gui_event_t *ev);

/* Non-blocking: naechstes Event nach *out. 1 = Event, 0 = leer. */
int  gui_input_pop(gui_event_t *out);

#ifdef RTOS_SELFTEST
/* Ring-Selbsttest (einreihen/poppen, FIFO, leer). 1 = ok. Laesst den Ring leer zurueck. */
int  gui_input_selftest(void);
#endif

#endif /* RPI_RTOS_GUI_INPUT_H */
