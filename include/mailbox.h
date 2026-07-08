/*
 * include/mailbox.h  --  ARM<->VideoCore Mailbox (Property-Channel)
 */
#ifndef RPI_RTOS_MAILBOX_H
#define RPI_RTOS_MAILBOX_H

#include <stdint.h>

/* Sendet einen Property-Puffer (buf[0] = Gesamtgroesse in Byte, 16-Byte-aligned)
 * an die VideoCore und wartet auf die Antwort. 0 = ok (Response-Code 0x80000000).
 * Die gesamte Transaktion ist ueber alle Kerne serialisiert (s_mboxlock). */
int mailbox_property(volatile uint32_t *buf);

/* Idempotenter, seiteneffektfreier Property-Read (Tag 0x00000001: Firmware-Revision).
 * Nutzt einen kern-eigenen Puffer -> gefahrlos gleichzeitig von mehreren Kernen aufrufbar.
 * 0 = ok (*out = Revision, falls != 0), -1 = Fehler. */
int mailbox_get_fw_rev(uint32_t *out);

/* Fragt die ARM-zugaengliche RAM-Basis+Groesse ab (Tag 0x00010005). 32-bit-Felder ->
 * nur die LOW-Region (<4 GiB); die Gesamtgroesse auf 4/8-GB-Boards steht im DTB (/memory).
 * 0 = ok (base und size gesetzt), -1 = Fehler. */
int mailbox_get_arm_memory(uint32_t *base, uint32_t *size);

/* VideoCore/GPU-Speicherregion (Tag 0x00010006). ARM-Mem + VC-Mem = gesamter LOW-RAM
 * (<4 GiB). Der Framebuffer liegt in der VC-Region -> muss mitgemappt werden. */
int mailbox_get_vc_memory(uint32_t *base, uint32_t *size);

#ifdef RTOS_SELFTEST
/* T1.9-Guardian: beobachtetes Maximum gleichzeitig in der Transaktion befindlicher Kerne
 * (muss 1 sein) bzw. Latch, ob je >1 auftrat (muss 0 sein). */
uint32_t mailbox_occ_max(void);
uint32_t mailbox_occ_violation(void);
#endif

#endif /* RPI_RTOS_MAILBOX_H */
