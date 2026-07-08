/*
 * drivers/gpu/v3d.c  --  VideoCore-VI-V3D-Hardware-Erkennung.
 */
#ifdef V3D_PROBE

#include <stdint.h>
#include "v3d.h"
#include "uart.h"

/* Pi4 (BCM2711) V3D-Core-MMIO. Die IDENT-Register beschreiben Technologie/Revision/Kern-Anzahl. */
#define V3D_BASE    0xFEC00000UL
#define V3D_IDENT0  0x00000u
#define V3D_IDENT1  0x00004u

static inline uint32_t v3d_rd(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(V3D_BASE + off);
}

static void put_hex32(uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    for (int s = 28; s >= 0; s -= 4) { uart_putc(hx[(v >> s) & 0xF]); }
}

void v3d_probe(void)
{
    uint32_t id0 = v3d_rd(V3D_IDENT0);
    /* V3D-IDENT0 traegt in den unteren Bytes das ASCII-Tag 'V','3','D' (0x56 0x33 0x44...). */
    int present = ((id0 & 0xFFu) == 0x56u) || (id0 != 0u && id0 != 0xFFFFFFFFu);

    uart_begin();
    if (present) {
        uint32_t id1 = v3d_rd(V3D_IDENT1);
        uart_puts("    [v3d] VideoCore-VI V3D gefunden: IDENT0=0x");
        put_hex32(id0);
        uart_puts(" cores=");
        uart_putdec((uint32_t)(id1 & 0xFu));               /* NCORES (grob) */
        uart_puts(" rev=");
        uart_putdec((uint32_t)((id1 >> 8) & 0xFu));         /* Revision (grob) */
        uart_puts(" -> V5-Backend-Bring-up moeglich\n");
    } else {
        uart_puts("    [v3d] V3D nicht gefunden (IDENT0=0x");
        put_hex32(id0);
        uart_puts(") -- in QEMU erwartet; echtes V3D nur am Pi4\n");
    }
    uart_end();
}

#endif /* V3D_PROBE */
