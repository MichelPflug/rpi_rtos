/*
 * boards/virt/trap_virt.S  --  Minimale Exception-Vektortabelle (Diagnose)
 *
 * 16 Eintraege a 0x80 Byte, 2-KiB-ausgerichtet. Jeder Eintrag merkt sich den
 * Index in x0 und springt in einen gemeinsamen Handler, der virt_trap()
 * aufruft (gibt ESR/ELR/FAR aus und haelt an). Genuegt, um beim Bring-up des
 * virtio-Treibers Faults sichtbar zu machen statt still zu haengen.
 */
.macro VEC idx
    .balign 0x80
    mov     x0, #\idx
    b       virt_trap_common
.endm

.section ".text"
.balign 0x800
.global virt_vectors
virt_vectors:
    VEC 0      /* Curr EL SP0: Sync  */
    VEC 1      /* Curr EL SP0: IRQ   */
    VEC 2      /* Curr EL SP0: FIQ   */
    VEC 3      /* Curr EL SP0: SError*/
    VEC 4      /* Curr EL SPx: Sync  */
    VEC 5      /* Curr EL SPx: IRQ   */
    VEC 6      /* Curr EL SPx: FIQ   */
    VEC 7      /* Curr EL SPx: SError*/
    VEC 8      /* Lower EL 64: Sync  */
    VEC 9      /* Lower EL 64: IRQ   */
    VEC 10     /* Lower EL 64: FIQ   */
    VEC 11     /* Lower EL 64: SError*/
    VEC 12     /* Lower EL 32: Sync  */
    VEC 13     /* Lower EL 32: IRQ   */
    VEC 14     /* Lower EL 32: FIQ   */
    VEC 15     /* Lower EL 32: SError*/

virt_trap_common:
    bl      virt_trap
1:  wfe
    b       1b
