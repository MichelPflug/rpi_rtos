/*
 * drivers/usb/usb_hc.c  --  Registry fuer die USB-Host-Controller-Abstraktion.
 *
 * Haelt einen Zeiger auf die ops des aktuell aktiven Host-Controllers (DWC2 im raspi-Build,
 * xHCI im virt-Build). Single-Controller-Modell: es gibt genau einen aktiven HC pro Boot;
 * der HC-Treiber registriert seine ops nach dem Init, die Klassen-Treiber lesen sie ueber
 * usb_hc(). Alle Zugriffe im Kernkontext ohne Nebenlaeufigkeit -> kein Lock noetig.
 */
#include "usb_hc.h"

static const usb_hc_ops_t *s_ops;

void usb_hc_register(const usb_hc_ops_t *ops) { s_ops = ops; }

const usb_hc_ops_t *usb_hc(void) { return s_ops; }
