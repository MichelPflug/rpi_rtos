/*
 * user/lib/vk/v3d_qpu.h  --  V3D-QPU-Instruktions-Encoder (Vulkan V5, Compiler-Ausgabestufe).
 */
#ifndef RPI_RTOS_V3D_QPU_H
#define RPI_RTOS_V3D_QPU_H

/* QPU-ALU-Ops (Teilmenge; Codes representativ, HW-RE-validierungsbeduerftig). */
#define QPU_A_NOP   0
#define QPU_A_FADD  1
#define QPU_A_FSUB  2
#define QPU_A_ADD   3
#define QPU_A_SUB   4
#define QPU_A_OR    5
#define QPU_A_AND   6
#define QPU_M_NOP   0
#define QPU_M_FMUL  1
#define QPU_M_MUL24 2
#define QPU_M_MOV   3

/* Signal-Bits (Teilmenge). */
#define QPU_SIG_NONE   0
#define QPU_SIG_LDUNIF 1
#define QPU_SIG_THREND 3
#define QPU_SIG_LDTMU  10

typedef struct {
    unsigned sig;          /* 4 bit  */
    unsigned cond;         /* 3 bit: Ausfuehrungsbedingung */
    unsigned add_op;       /* 8 bit  */
    unsigned mul_op;       /* 5 bit  */
    unsigned waddr_add;    /* 6 bit: Ziel des add-ALU */
    unsigned waddr_mul;    /* 6 bit: Ziel des mul-ALU */
    unsigned raddr_a;      /* 6 bit: Quelle A */
    unsigned raddr_b;      /* 6 bit: Quelle B */
} v3d_qpu_instr_t;

/* Logische Felder -> 64-bit-QPU-Wort packen (Feld-Layout s. .c; HW-RE-zu-validieren). */
unsigned long long v3d_qpu_pack(const v3d_qpu_instr_t *in);

/* 64-bit-QPU-Wort -> Felder entpacken. pack/unpack sind bijektiv (Round-Trip-testbar). */
void               v3d_qpu_unpack(unsigned long long word, v3d_qpu_instr_t *out);

/* Bequemer Encoder eines add-ALU-Ops (mul = NOP): waddr = add_op(raddr_a, raddr_b). */
unsigned long long v3d_qpu_alu_add(unsigned add_op, unsigned waddr, unsigned raddr_a, unsigned raddr_b);

/* Eine QPU-NOP (thread-end-Signal optional) -- Programm-Terminator. */
unsigned long long v3d_qpu_nop(int thread_end);

/* Selbsttest: beweist die Encoder-INFRASTRUKTUR (Round-Trip + Feld-Isolation, kein Bit-Bleed) UND
 * den Control-List-Builder (byte-exakte Record-Emission). Rueckgabe 0 = ok, <0 = Bug. */
int                v3d_qpu_selftest(void);

/* --- V5.2: V3D-Control-List (CLE) Builder -- Tile-Based-Deferred-Renderer-Kommandoliste.
 * Emittiert Records (Opcode-Byte + Little-Endian-Operanden) in einen Puffer. Die Opcodes sind
 * representativ (mesa/v3d-nah) und HW-RE-zu-validieren; die Byte-Emission selbst ist verifizierbar. */
#define V3D_CLE_HALT                 0
#define V3D_CLE_NOP                  1
#define V3D_CLE_FLUSH                4
#define V3D_CLE_FLUSH_ALL_STATE      5   /* V5.2a: Flush + State-Cache-Invalidate */
#define V3D_CLE_START_TILE_BINNING   6
#define V3D_CLE_INCR_SEMAPHORE       7
#define V3D_CLE_WAIT_ON_SEMAPHORE    8   /* V5.2a: CLE blockiert bis Semaphore>0 (Bin<->Render-Sync) */
#define V3D_CLE_BRANCH              16
#define V3D_CLE_BRANCH_TO_SUB_LIST  17   /* V5.2a: Sub-List aufrufen (Ruecksprungadresse in RA0) */
#define V3D_CLE_RETURN_FROM_SUB_LIST 18  /* V5.2a: aus Sub-List zurueck */

typedef struct { unsigned char *buf; unsigned cap, len; } v3d_cle_t;

void v3d_cle_init(v3d_cle_t *c, unsigned char *buf, unsigned cap);
void v3d_cle_u8(v3d_cle_t *c, unsigned v);
void v3d_cle_u16(v3d_cle_t *c, unsigned v);
void v3d_cle_u32(v3d_cle_t *c, unsigned v);
void v3d_cle_op(v3d_cle_t *c, unsigned opcode);      /* nur das Opcode-Byte */
void v3d_cle_branch(v3d_cle_t *c, unsigned addr);    /* BRANCH-Opcode + 32-bit-Zieladresse */
/* V5.2a: weitere Control-List-Records (Opcode-Byte + LE-Operanden, byte-exakt). */
void v3d_cle_branch_sub(v3d_cle_t *c, unsigned addr);/* BRANCH_TO_SUB_LIST + 32-bit-Adresse */
void v3d_cle_return(v3d_cle_t *c);                   /* RETURN_FROM_SUB_LIST (1 Byte) */
void v3d_cle_wait_sem(v3d_cle_t *c);                 /* WAIT_ON_SEMAPHORE (1 Byte) */
void v3d_cle_flush_all(v3d_cle_t *c);                /* FLUSH_ALL_STATE (1 Byte) */
/* Generischer Paket-Emitter: Opcode-Byte + n Nutzlast-Bytes (fuer Multi-Byte-Records wie
 * TILE_BINNING_MODE_CFG; die konkreten Feld-Layouts sind HW-RE-zu-validieren, die Byte-Emission
 * selbst ist verifizierbar). Atomar: passt das ganze Paket nicht, wird NICHTS geschrieben. */
void v3d_cle_packet(v3d_cle_t *c, unsigned opcode, const unsigned char *payload, unsigned n);

#endif /* RPI_RTOS_V3D_QPU_H */
