/*
 * user/lib/vk/v3d_qpu.c  --  V3D-QPU-Instruktions-Encoder.
 */
#include "v3d_qpu.h"

/* --- Feld-Layout des 64-bit-QPU-Wortes (representativ, HW-RE-zu-validieren) ---
 * Untere 32 Bit = "unteres" Signal-/ALU-Feld, obere 32 Bit = Register-/Ziel-Feld. Die Breiten
 * folgen der QPU (sig 4, cond 3, add_op 8, mul_op 5, je 6 fuer waddr_add/mul/raddr_a/raddr_b). */
#define F_SIG_SHIFT    0
#define F_SIG_BITS     4
#define F_COND_SHIFT   4
#define F_COND_BITS    3
#define F_ADDOP_SHIFT  7
#define F_ADDOP_BITS   8
#define F_MULOP_SHIFT  15
#define F_MULOP_BITS   5
/* obere Haelfte (Bit 32+) */
#define F_WADDA_SHIFT  32
#define F_WADDA_BITS   6
#define F_WADDM_SHIFT  38
#define F_WADDM_BITS   6
#define F_RADDA_SHIFT  44
#define F_RADDA_BITS   6
#define F_RADDB_SHIFT  50
#define F_RADDB_BITS   6

static unsigned long long put_field(unsigned long long w, unsigned val, int shift, int bits)
{
    unsigned long long mask = ((1ull << bits) - 1ull);
    return w | (((unsigned long long)val & mask) << shift);
}
static unsigned get_field(unsigned long long w, int shift, int bits)
{
    unsigned long long mask = ((1ull << bits) - 1ull);
    return (unsigned)((w >> shift) & mask);
}

unsigned long long v3d_qpu_pack(const v3d_qpu_instr_t *in)
{
    unsigned long long w = 0;
    w = put_field(w, in->sig,       F_SIG_SHIFT,   F_SIG_BITS);
    w = put_field(w, in->cond,      F_COND_SHIFT,  F_COND_BITS);
    w = put_field(w, in->add_op,    F_ADDOP_SHIFT, F_ADDOP_BITS);
    w = put_field(w, in->mul_op,    F_MULOP_SHIFT, F_MULOP_BITS);
    w = put_field(w, in->waddr_add, F_WADDA_SHIFT, F_WADDA_BITS);
    w = put_field(w, in->waddr_mul, F_WADDM_SHIFT, F_WADDM_BITS);
    w = put_field(w, in->raddr_a,   F_RADDA_SHIFT, F_RADDA_BITS);
    w = put_field(w, in->raddr_b,   F_RADDB_SHIFT, F_RADDB_BITS);
    return w;
}

void v3d_qpu_unpack(unsigned long long word, v3d_qpu_instr_t *out)
{
    out->sig       = get_field(word, F_SIG_SHIFT,   F_SIG_BITS);
    out->cond      = get_field(word, F_COND_SHIFT,  F_COND_BITS);
    out->add_op    = get_field(word, F_ADDOP_SHIFT, F_ADDOP_BITS);
    out->mul_op    = get_field(word, F_MULOP_SHIFT, F_MULOP_BITS);
    out->waddr_add = get_field(word, F_WADDA_SHIFT, F_WADDA_BITS);
    out->waddr_mul = get_field(word, F_WADDM_SHIFT, F_WADDM_BITS);
    out->raddr_a   = get_field(word, F_RADDA_SHIFT, F_RADDA_BITS);
    out->raddr_b   = get_field(word, F_RADDB_SHIFT, F_RADDB_BITS);
}

unsigned long long v3d_qpu_alu_add(unsigned add_op, unsigned waddr, unsigned raddr_a, unsigned raddr_b)
{
    v3d_qpu_instr_t i = { QPU_SIG_NONE, 0, add_op, QPU_M_NOP, waddr, 0, raddr_a, raddr_b };
    return v3d_qpu_pack(&i);
}

unsigned long long v3d_qpu_nop(int thread_end)
{
    v3d_qpu_instr_t i = { thread_end ? QPU_SIG_THREND : QPU_SIG_NONE, 0,
                          QPU_A_NOP, QPU_M_NOP, 0, 0, 0, 0 };
    return v3d_qpu_pack(&i);
}

int v3d_qpu_selftest(void)
{
    /* 1) Round-Trip: eine reich belegte Instruktion pack->unpack->vergleichen (alle Felder erhalten). */
    v3d_qpu_instr_t a = { QPU_SIG_LDUNIF, 5, QPU_A_FADD, QPU_M_FMUL, 33, 34, 6, 7 };
    v3d_qpu_instr_t b;
    v3d_qpu_unpack(v3d_qpu_pack(&a), &b);
    if (a.sig != b.sig || a.cond != b.cond || a.add_op != b.add_op || a.mul_op != b.mul_op ||
        a.waddr_add != b.waddr_add || a.waddr_mul != b.waddr_mul ||
        a.raddr_a != b.raddr_a || a.raddr_b != b.raddr_b) { return -1; }

    /* 2) Feld-Isolation: nur EIN Feld setzen darf KEINE anderen Bits beruehren (kein Bit-Bleed). */
    v3d_qpu_instr_t z = { 0, 0, 0, 0, 0, 0, 0, 0 };
    z.raddr_a = 0x3F;                                  /* volles 6-bit-Feld */
    unsigned long long w = v3d_qpu_pack(&z);
    if (w != (0x3Full << F_RADDA_SHIFT)) { return -2; }  /* exakt dort, nirgends sonst */
    z.raddr_a = 0; z.add_op = 0xFF;
    w = v3d_qpu_pack(&z);
    if (w != (0xFFull << F_ADDOP_SHIFT)) { return -3; }

    /* 3) Ueberbreite Werte werden auf die Feldbreite maskiert (kein Ueberlauf in Nachbarfelder). */
    z.add_op = 0; z.mul_op = 0xFF;                     /* 5-bit-Feld -> nur 0x1F bleibt */
    v3d_qpu_unpack(v3d_qpu_pack(&z), &b);
    if (b.mul_op != 0x1F) { return -4; }

    /* 4) NOP mit thread-end traegt das THREND-Signal, sonst leer. */
    v3d_qpu_unpack(v3d_qpu_nop(1), &b);
    if (b.sig != QPU_SIG_THREND || b.add_op != QPU_A_NOP || b.mul_op != QPU_M_NOP) { return -5; }

    /* 5) alu_add-Helfer kodiert genau das ALU-Add mit mul=NOP. */
    v3d_qpu_unpack(v3d_qpu_alu_add(QPU_A_ADD, 40, 8, 9), &b);
    if (b.add_op != QPU_A_ADD || b.mul_op != QPU_M_NOP || b.waddr_add != 40 ||
        b.raddr_a != 8 || b.raddr_b != 9) { return -6; }

    /* 6) Control-List-Builder: eine kleine Bin-Control-List byte-exakt aufbauen + pruefen. */
    unsigned char cbuf[16];
    v3d_cle_t cl;
    v3d_cle_init(&cl, cbuf, sizeof(cbuf));
    v3d_cle_op(&cl, V3D_CLE_START_TILE_BINNING);   /* 1 Byte: 0x06 */
    v3d_cle_op(&cl, V3D_CLE_FLUSH);                 /* 1 Byte: 0x04 */
    v3d_cle_branch(&cl, 0xDEADBEEFu);               /* 1 Byte 0x10 + LE-u32 */
    v3d_cle_op(&cl, V3D_CLE_HALT);                  /* 1 Byte: 0x00 */
    if (cl.len != 8) { return -7; }
    if (cbuf[0] != V3D_CLE_START_TILE_BINNING || cbuf[1] != V3D_CLE_FLUSH ||
        cbuf[2] != V3D_CLE_BRANCH) { return -8; }
    if (cbuf[3] != 0xEF || cbuf[4] != 0xBE || cbuf[5] != 0xAD || cbuf[6] != 0xDE) { return -9; } /* LE */
    if (cbuf[7] != V3D_CLE_HALT) { return -10; }

    /* 7) Overflow-Schutz: emittieren ueber die Kapazitaet hinaus schreibt NICHTS mehr (kein OOB). */
    v3d_cle_t sm;
    unsigned char one[1];
    v3d_cle_init(&sm, one, 1);
    v3d_cle_op(&sm, V3D_CLE_NOP);                    /* fuellt das 1 Byte */
    v3d_cle_u32(&sm, 0x11223344u);                   /* passt nicht mehr -> ignoriert */
    if (sm.len != 1 || one[0] != V3D_CLE_NOP) { return -11; }

    /* 8) V5.2a: erweiterte CLE-Records byte-exakt (Flush-All/Wait-Sem/Branch-Sub/Return). */
    unsigned char rb[24];
    v3d_cle_t rl;
    v3d_cle_init(&rl, rb, sizeof(rb));
    v3d_cle_flush_all(&rl);                          /* 0x05 */
    v3d_cle_wait_sem(&rl);                           /* 0x08 */
    v3d_cle_branch_sub(&rl, 0x11223344u);            /* 0x11 + LE-u32 */
    v3d_cle_return(&rl);                             /* 0x12 */
    if (rl.len != 8) { return -12; }                 /* 1+1+5+1 */
    if (rb[0] != V3D_CLE_FLUSH_ALL_STATE || rb[1] != V3D_CLE_WAIT_ON_SEMAPHORE ||
        rb[2] != V3D_CLE_BRANCH_TO_SUB_LIST) { return -13; }
    if (rb[3] != 0x44 || rb[4] != 0x33 || rb[5] != 0x22 || rb[6] != 0x11) { return -14; }  /* LE */
    if (rb[7] != V3D_CLE_RETURN_FROM_SUB_LIST) { return -15; }

    /* 9) generisches Multi-Byte-Paket byte-exakt + ATOMARER Overflow (Rest zu klein -> nichts). */
    unsigned char pb[8];
    v3d_cle_t pl;
    v3d_cle_init(&pl, pb, sizeof(pb));
    unsigned char pay[3] = { 0xAA, 0xBB, 0xCC };
    v3d_cle_packet(&pl, 0x38, pay, 3);               /* 0x38 AA BB CC */
    if (pl.len != 4 || pb[0] != 0x38 || pb[1] != 0xAA || pb[2] != 0xBB || pb[3] != 0xCC) { return -16; }
    unsigned char big[8] = { 0 };
    v3d_cle_packet(&pl, 0x39, big, 8);               /* 9 Bytes in 4 freie -> atomar verworfen */
    if (pl.len != 4) { return -17; }

    return 0;
}

/* --- V5.2: V3D-Control-List (CLE) Builder --- */
void v3d_cle_init(v3d_cle_t *c, unsigned char *buf, unsigned cap)
{
    c->buf = buf; c->cap = cap; c->len = 0;
}
void v3d_cle_u8(v3d_cle_t *c, unsigned v)
{
    if (c->len + 1 <= c->cap) { c->buf[c->len++] = (unsigned char)(v & 0xFF); }
}
void v3d_cle_u16(v3d_cle_t *c, unsigned v)
{
    if (c->len + 2 <= c->cap) {                      /* atomar: nur wenn BEIDE Bytes passen */
        c->buf[c->len++] = (unsigned char)(v & 0xFF);
        c->buf[c->len++] = (unsigned char)((v >> 8) & 0xFF);
    }
}
void v3d_cle_u32(v3d_cle_t *c, unsigned v)
{
    if (c->len + 4 <= c->cap) {                      /* atomar */
        c->buf[c->len++] = (unsigned char)(v & 0xFF);
        c->buf[c->len++] = (unsigned char)((v >> 8) & 0xFF);
        c->buf[c->len++] = (unsigned char)((v >> 16) & 0xFF);
        c->buf[c->len++] = (unsigned char)((v >> 24) & 0xFF);
    }
}
void v3d_cle_op(v3d_cle_t *c, unsigned opcode) { v3d_cle_u8(c, opcode); }
void v3d_cle_branch(v3d_cle_t *c, unsigned addr)
{
    if (c->len + 5 <= c->cap) { v3d_cle_u8(c, V3D_CLE_BRANCH); v3d_cle_u32(c, addr); }
}
/* V5.2a: weitere Records. Alle atomar (nur wenn das ganze Paket in die Kapazitaet passt). */
void v3d_cle_branch_sub(v3d_cle_t *c, unsigned addr)
{
    if (c->len + 5 <= c->cap) { v3d_cle_u8(c, V3D_CLE_BRANCH_TO_SUB_LIST); v3d_cle_u32(c, addr); }
}
void v3d_cle_return(v3d_cle_t *c)    { v3d_cle_u8(c, V3D_CLE_RETURN_FROM_SUB_LIST); }
void v3d_cle_wait_sem(v3d_cle_t *c)  { v3d_cle_u8(c, V3D_CLE_WAIT_ON_SEMAPHORE); }
void v3d_cle_flush_all(v3d_cle_t *c) { v3d_cle_u8(c, V3D_CLE_FLUSH_ALL_STATE); }
void v3d_cle_packet(v3d_cle_t *c, unsigned opcode, const unsigned char *payload, unsigned n)
{
    if (c->len + 1u + n <= c->cap) {          /* atomar: Opcode + gesamte Nutzlast muss passen */
        v3d_cle_u8(c, opcode);
        for (unsigned i = 0; i < n; i++) { v3d_cle_u8(c, payload[i]); }
    }
}
