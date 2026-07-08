/*
 * boards/virt/smp_stub.c  --  SMP-Stub fuer den virt-Harness
 *
 * Der virt-Build laeuft single-core (-smp 1) und linkt weder den Scheduler noch den
 * Timer-/MMU-Code des raspi4b-Targets. start.S referenziert aber secondary_main
 * (Spin-Table-Einsprung). Dieser Stub liefert das Symbol; er wird nie ausgefuehrt,
 * da main_virt.c die Sekundaerkerne nie freigibt.
 */
#include <stdint.h>
#include "aarch64.h"

void secondary_main(uint64_t core_id)
{
    (void)core_id;
    for (;;) {
        wfe();
    }
}
