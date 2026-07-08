/*
 * include/usbmouse.h  --  USB-HID-Boot-Maus -> Cursor-Position + Buttons.
 *
 * Nutzt denselben HID-Interrupt-IN-Ring wie usbkbd (nur EIN HID-Geraet gleichzeitig, dev_kind 3).
 * Dekodiert Boot-Maus-Reports (Byte0=Buttons, Byte1=dx, Byte2=dy, dx/dy signed 8-bit relativ) zu
 * einer auf den Bildschirm geklemmten Cursor-Position. Das Event-System pollt usbmouse_poll.
 */
#ifndef RPI_RTOS_USBMOUSE_H
#define RPI_RTOS_USBMOUSE_H

#include <stdint.h>

/* Aktiviert die Maus; width/height = Bildschirmgroesse (Cursor-Clamping); Cursor startet mittig. */
void     usbmouse_enable(uint32_t width, uint32_t height);

/* Fertige Reports aus dem HID-Ring holen + Cursor/Buttons aktualisieren. Liefert 1, wenn sich
 * Position oder Buttons geaendert haben (fuer das Event-System). */
int      usbmouse_poll(void);

int      usbmouse_x(void);
int      usbmouse_y(void);
unsigned usbmouse_buttons(void);   /* bit0=links, bit1=rechts, bit2=mitte */

#ifdef RTOS_SELFTEST
/* Dekodier-/Clamp-Selbsttest mit synthetischen Reports (unabhaengig von echter HW-Bewegung). */
int      usbmouse_selftest(void);
#endif

#endif /* RPI_RTOS_USBMOUSE_H */
