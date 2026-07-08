/*
 * user/lib/vk/vk_spirv.h  --  SPIR-V-Interpreter
 */
#ifndef RPI_RTOS_VK_SPIRV_H
#define RPI_RTOS_VK_SPIRV_H

#define SPV_MAX_IDS    224
#define SPV_VAL_WORDS  16      /* groesster Wert: mat4 = 16 Woerter */
#define SPV_MAX_LOC    8
#define SPV_MAX_ENTRY  4
#define SPV_MAX_MEMBER 8
#define SPV_MAX_DESCRIPTOR 8   /* Bindings je Set 0 (Uniform/Storage-Buffer, V1.3) */

#define SPV_MODEL_VERTEX    0
#define SPV_MODEL_FRAGMENT  4
#define SPV_MODEL_GLCOMPUTE 5      /* V1.7: Compute-Shader */

typedef union { float f; unsigned u; int i; } spv_w;

/* V1.4: gebundene Textur (Combined-Image-Sampler). */
typedef struct {
    const void *pixels;                /* B8G8R8A8_UNORM, 0 = nicht gebunden */
    unsigned    w, h, pitch_px;
    int         filter;                /* 0 = nearest, 1 = linear */
    int         wrap;                  /* 0 = repeat, 1 = clamp-to-edge */
} spv_tex_t;

typedef struct {
    unsigned char kind;        /* 0=frei 1=Typ 2=Konst 3=Var 4=Funktion 5=ExtSet 6=Label */
    unsigned char tclass;      /* SPV_T_* (bei Typen) */
    unsigned char cols, rows;  /* vec: rows; mat: cols x rows */
    unsigned      type;        /* Typ-Id (Konst/Var/Ptr-Pointee) */
    unsigned      storage;     /* StorageClass (Var/Ptr) */
    unsigned char builtin;     /* 0xFF = keiner */
    unsigned char location;    /* 0xFF = keine */
    unsigned char dset;        /* DescriptorSet (0xFF = keiner) */
    unsigned char dbinding;    /* Binding (0xFF = keins) */
    unsigned char nmember;
    unsigned      moff[SPV_MAX_MEMBER];    /* Byte-Offsets (OpMemberDecorate Offset) */
    unsigned      mtype[SPV_MAX_MEMBER];
    unsigned      word_off;    /* Label/Funktion: Instruktions-Offset im Wortstrom */
    spv_w         cval[SPV_VAL_WORDS];     /* Konstanten */
} spv_id_t;

typedef struct {
    const unsigned *words;
    unsigned        nwords;
    spv_id_t        ids[SPV_MAX_IDS];
    unsigned        bound;
    struct { unsigned model, fn; } entry[SPV_MAX_ENTRY];
    int             nentry;
    unsigned        lsx, lsy, lsz;         /* V1.7: LocalSize (OpExecutionMode), Default 1 */
} spv_mod_t;

/* Ein-/Ausgaben einer Invokation (Vertex ODER Fragment). */
typedef struct {
    float       in_loc[SPV_MAX_LOC][4];    /* Eingabe-Attribute je Location   */
    float       out_loc[SPV_MAX_LOC][4];   /* Ausgaben je Location            */
    float       builtin_pos[4];            /* Vertex-Ausgabe gl_Position      */
    float       frag_coord[4];             /* Fragment-Eingabe gl_FragCoord   */
    int         vertex_index;              /* Vertex-Eingabe gl_VertexIndex   */
    int         instance_index;            /* Vertex-Eingabe gl_InstanceIndex */
    unsigned    global_id[3];              /* V1.7: Compute gl_GlobalInvocationID */
    const void *push;                      /* Push-Constant-Block (Bytes)     */
    unsigned    push_bytes;
    /* V1.3: gebundene Descriptor (Set 0), je Binding ein Uniform/Storage-Buffer. */
    const void *ubo[SPV_MAX_DESCRIPTOR];
    unsigned    ubo_bytes[SPV_MAX_DESCRIPTOR];
    /* V1.4: gebundene Texturen (Combined-Image-Sampler) je Binding. */
    spv_tex_t   tex[SPV_MAX_DESCRIPTOR];
    /* V2.3: von OpKill/OpTerminateInvocation gesetzt -> der FS-Executor verwirft das Fragment
     * (kein Farb-/Tiefen-Write). Vor jeder Invokation auf 0 setzen. */
    int         discarded;
} spv_io_t;

/* Modul parsen (validiert Magic/Bound/Grenzen). 0 = ok, <0 = Fehler. */
int spv_parse(spv_mod_t *m, const unsigned *words, unsigned nwords);

/* Entry-Point eines Execution-Models suchen. 0 = ok (Funktions-Id in *fn). */
int spv_find_entry(const spv_mod_t *m, int model, unsigned *fn);

/* Entry-Point ausfuehren (single-threaded; nutzt einen statischen Scratch).
 * 0 = ok, <0 = Fehler (unbekannte Op, Grenzverletzung, Schrittlimit). */
int spv_exec(const spv_mod_t *m, unsigned fn, spv_io_t *io);

#endif /* RPI_RTOS_VK_SPIRV_H */
