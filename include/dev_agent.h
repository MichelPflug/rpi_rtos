/*
 * include/dev_agent.h  --  Dev-Remote-Interface (UDP).
 *
 * Haengt den Protokoll-Kern (net/dev_remote.c) an das laufende System: UDP-Socket (GENET),
 * Konsolen-Mirror -> OUTPUT, Framebuffer -> SCREEN_DATA, Eingabe-Injektion (KEY/MOUSE), Datei-
 * Schreiben hdd0/hdd1 (inkl. kernel8.img -> Boot-FAT), Neustart (PM-Watchdog). Der GESAMTE Inhalt
 * steht unter #ifdef DEV_REMOTE -> ohne das Flag kein Byte (Kernel byte-identisch)..
 */
#ifndef RPI_RTOS_DEV_AGENT_H
#define RPI_RTOS_DEV_AGENT_H
#ifdef DEV_REMOTE

#include <stdint.h>
#include "dev_remote.h"

typedef struct dev_agent dev_agent_t;

/* Framebuffer-Sicht fuer den SCREEN_REQ-Pfad (row-major 0x00RRGGBB, Zeilenschritt in Pixeln). */
typedef struct { const volatile uint32_t *px; int w, h, stride_px; } dev_fb_view_t;

/* Abstrakte Systemschnittstelle -> QEMU-Test mit Mock-Ops (kein Netz/HW noetig), Pi4 mit realen Ops. */
typedef struct {
    void (*send)(dev_agent_t *a, uint8_t type, uint8_t flags, uint16_t seq,
                 const uint8_t *pl, int len);                           /* ein Antwort-Datagramm */
    int  (*write_file)(dev_agent_t *a, int partition, const char *path, /* 0=ok; part 0=hdd0,1=hdd1 */
                       const uint8_t *buf, uint32_t len);
    void (*inject_key)(dev_agent_t *a, uint8_t key);
    void (*inject_mouse)(dev_agent_t *a, int16_t dx, int16_t dy, uint8_t buttons);
    int  (*fb_get)(dev_agent_t *a, dev_fb_view_t *out);                 /* 0=ok, -1=keiner */
    void (*reboot)(dev_agent_t *a);                                     /* kehrt normal nicht zurueck */
} dev_ops_t;

struct dev_agent {
    const dev_ops_t *ops;
    void            *user;             /* Aufrufer-privat */
    /* Datei-Reassembly-Zustand (Aufrufer stellt den Puffer). */
    dev_file_t       file;
    uint8_t         *file_buf;
    uint32_t         file_cap;
    int              file_partition;
    char             file_path[64];
    int              file_active;
};

void dev_agent_init(dev_agent_t *a, const dev_ops_t *ops, void *user,
                    uint8_t *file_buf, uint32_t file_cap);
/* Ein empfangenes UDP-Datagramm verarbeiten (Header + Dispatch; untrusted -> alles bounds-checked). */
void dev_agent_input(dev_agent_t *a, const uint8_t *pkt, int len);

/* --- System-Nahtstellen (aus uart.c / gui_input.c / usbkbd.c, jeweils #ifdef DEV_REMOTE) --- */
void dev_console_tee(char c);     /* jedes Konsolen-Byte -> OUTPUT-Ring (Kern 0, unter UART-Lock) */
void dev_input_drain(void);       /* gestagete MOUSE-Events -> gui_input_push (Timer-IRQ, Kern 0) */
int  dev_console_inject_get(void); /* ferngesteuerte Taste fuer console_getc(_nb); -1 = nichts anliegend */

/* Startet den Agenten: Dispatch-Selbsttest (synthetisch, QEMU) + Live-Netz-Task (nur am Pi4). kmain-Naht. */
void dev_agent_start(void);

#endif /* DEV_REMOTE */
#endif /* RPI_RTOS_DEV_AGENT_H */
