/*
 * drivers/usb/usbmouse.c  --  USB-HID-Boot-Maus.
 *
 * Holt HID-Boot-Maus-Reports ueber die HCD-vtable (usb_hc()->mouse_getreport; bei DWC2 derselbe
 * Interrupt-IN-Ring wie die Tastatur) und fuehrt daraus eine auf den Bildschirm geklemmte
 * Cursor-Position + Button-Maske. Report-Format (Boot-Protokoll): Byte0 = Buttons (bit0 links,
 * bit1 rechts, bit2 mitte), Byte1 = dx, Byte2 = dy (beide signed 8-bit, relativ).
 */
#include <stdint.h>
#include "usb_hc.h"
#include "usbmouse.h"

static int hc_mouse_getreport(uint8_t report[8])
{
    const usb_hc_ops_t *h = usb_hc();
    return (h && h->mouse_getreport) ? h->mouse_getreport(report) : -1;
}

static int      s_enabled;
static int      s_x, s_y;              /* Cursor-Position (geklemmt auf [0,w) x [0,h)) */
static int      s_w = 640, s_h = 480;  /* Bildschirmgrenzen */
static unsigned s_buttons;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void usbmouse_enable(uint32_t width, uint32_t height)
{
    s_w = width  ? (int)width  : 640;
    s_h = height ? (int)height : 480;
    s_x = s_w / 2;
    s_y = s_h / 2;
    s_buttons = 0;
    s_enabled = 1;
}

/* Einen Boot-Maus-Report anwenden. Liefert 1, wenn sich Position oder Buttons aenderten. */
static int mouse_decode(const uint8_t rep[8])
{
    int      dx = (int)(int8_t)rep[1];
    int      dy = (int)(int8_t)rep[2];
    unsigned b  = rep[0] & 0x07u;
    int      ox = s_x, oy = s_y;
    unsigned ob = s_buttons;

    s_x = clampi(s_x + dx, 0, s_w - 1);
    s_y = clampi(s_y + dy, 0, s_h - 1);
    s_buttons = b;
    return (s_x != ox || s_y != oy || s_buttons != ob) ? 1 : 0;
}

int usbmouse_poll(void)
{
    if (!s_enabled) { return 0; }
    int changed = 0;
    uint8_t rep[8];
    while (hc_mouse_getreport(rep) > 0) {
        if (mouse_decode(rep)) { changed = 1; }
    }
    return changed;
}

int      usbmouse_x(void)       { return s_x; }
int      usbmouse_y(void)       { return s_y; }
unsigned usbmouse_buttons(void) { return s_buttons; }

#ifdef RTOS_SELFTEST
int usbmouse_selftest(void)
{
    int save_x = s_x, save_y = s_y, save_w = s_w, save_h = s_h;
    unsigned save_b = s_buttons;
    s_w = 640; s_h = 480; s_x = 100; s_y = 100; s_buttons = 0;

    uint8_t r1[8] = { 0x01, 20, 10, 0, 0, 0, 0, 0 };            /* Button links, +20/+10 */
    int c1 = mouse_decode(r1);
    int ok = (c1 == 1 && s_x == 120 && s_y == 110 && s_buttons == 0x01);

    uint8_t r2[8] = { 0x00, (uint8_t)(-30), (uint8_t)(-20), 0, 0, 0, 0, 0 };  /* -30/-20, Button los */
    mouse_decode(r2);
    ok = ok && (s_x == 90 && s_y == 90 && s_buttons == 0);

    uint8_t r3[8] = { 0x00, (uint8_t)(-128), (uint8_t)(-128), 0, 0, 0, 0, 0 };  /* Clamp gegen 0 */
    mouse_decode(r3);
    mouse_decode(r3);
    ok = ok && (s_x == 0 && s_y == 0);

    s_x = save_x; s_y = save_y; s_w = save_w; s_h = save_h; s_buttons = save_b;
    return ok ? 1 : 0;
}
#endif
