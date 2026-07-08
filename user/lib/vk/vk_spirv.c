/*
 * user/lib/vk/vk_spirv.c  --  SPIR-V-Interpreter  MIT FP kompiliert.
 */
#include "vk_spirv.h"

void *memset(void *dst, int c, unsigned long n);

/* --- Freestanding fp32-Transzendenten (kein libm; V2.4 GLSL.std.450). Naeherungen mit fuer einen
 *     Software-Rasterizer ausreichender Genauigkeit; __builtin_sqrtf ist ein echter Compiler-Builtin. --- */
static float spvf_floor(float x) { float t = (float)(long)x; return (t > x) ? t - 1.0f : t; }
static float spvf_trunc(float x) { return (float)(long)x; }
static float spvf_exp(float x)
{
    if (x > 88.0f)  { return 3.0e38f; }
    if (x < -88.0f) { return 0.0f; }
    float t = x * 1.44269504f;                       /* x/ln2 */
    int k = (int)(t >= 0.0f ? t + 0.5f : t - 0.5f);
    float f = x - (float)k * 0.69314718f;            /* Rest in [-ln2/2, ln2/2] */
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * (0.041666667f + f * 0.008333333f))));
    union { float f; unsigned u; } bt; bt.u = (unsigned)((k + 127) << 23);   /* 2^k */
    return p * bt.f;
}
static float spvf_log(float x)
{
    if (x <= 0.0f) { return -3.0e38f; }
    union { float f; unsigned u; } bt; bt.f = x;
    int e = (int)((bt.u >> 23) & 0xFFu) - 127;
    bt.u = (bt.u & 0x007FFFFFu) | 0x3F800000u;       /* Mantisse in [1,2) */
    float s = (bt.f - 1.0f) / (bt.f + 1.0f), s2 = s * s;
    return 2.0f * s * (1.0f + s2 * (0.33333333f + s2 * (0.2f + s2 * 0.14285714f))) + (float)e * 0.69314718f;
}
static float spvf_pow(float b, float e) { return (b <= 0.0f) ? (e == 0.0f ? 1.0f : 0.0f) : spvf_exp(e * spvf_log(b)); }
static float spvf_sin(float x)
{
    float k = spvf_floor(x * 0.15915494f + 0.5f);    /* auf [-pi,pi] reduzieren */
    x -= k * 6.28318531f;
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333f + x2 * (-0.00019841f + x2 * 0.0000027557f))));
}
static float spvf_cos(float x) { return spvf_sin(x + 1.57079633f); }
static float spvf_atan(float x)
{
    int neg = x < 0.0f; if (neg) { x = -x; }
    int inv = x > 1.0f; if (inv) { x = 1.0f / x; }
    float x2 = x * x;
    float r = x * (0.9998660f + x2 * (-0.3302995f + x2 * (0.1801410f + x2 * (-0.0851330f + x2 * 0.0208351f))));
    if (inv) { r = 1.57079633f - r; }
    return neg ? -r : r;
}
static float spvf_atan2(float y, float x)
{
    if (x > 0.0f) { return spvf_atan(y / x); }
    if (x < 0.0f) { return spvf_atan(y / x) + (y >= 0.0f ? 3.14159265f : -3.14159265f); }
    return y > 0.0f ? 1.57079633f : (y < 0.0f ? -1.57079633f : 0.0f);
}
static float spvf_asin(float x)
{
    if (x >= 1.0f) { return 1.57079633f; }
    if (x <= -1.0f) { return -1.57079633f; }
    return spvf_atan(x / __builtin_sqrtf(1.0f - x * x));
}

/* --- Opcodes / Enums (SPIR-V Unified1) --- */
#define OP_EXTINSTIMPORT 11
#define OP_EXTINST       12
#define OP_MEMORYMODEL   14
#define OP_ENTRYPOINT    15
#define OP_EXECUTIONMODE 16
#define OP_CAPABILITY    17
#define OP_TYPEVOID      19
#define OP_TYPEBOOL      20
#define OP_TYPEINT       21
#define OP_TYPEFLOAT     22
#define OP_TYPEVECTOR    23
#define OP_TYPEMATRIX    24
#define OP_TYPEIMAGE     25
#define OP_TYPESAMPLER   26
#define OP_TYPESAMPLEDIMG 27
#define OP_TYPEARRAY     28     /* V1.7 */
#define OP_TYPERUNTIMEARR 29    /* V1.7 */
#define OP_TYPESTRUCT    30
#define OP_EXECUTIONMODE 16     /* V1.7: LocalSize */
#define OP_IMAGESAMPLEIMPL 87
#define OP_IMAGESAMPLEEXPL 88
#define OP_IMAGEFETCH      95     /* V2.6: texelFetch */
#define OP_IMAGEGATHER     96     /* V2.6: textureGather */
#define OP_IMAGEREAD       98     /* V2.6: imageLoad (Storage-Image) */
#define OP_IMAGEWRITE      99     /* V2.6: imageStore (Storage-Image) */
#define OP_IMAGE          100     /* V2.6: Image aus Sampled-Image */
#define OP_IMAGEQUERYSIZELOD 103  /* V2.6: textureSize(.,lod) */
#define OP_IMAGEQUERYSIZE 104     /* V2.6: textureSize (ohne LOD) */
#define OP_TYPEPOINTER   32
#define OP_TYPEFUNCTION  33
#define OP_CONSTANT      43
#define OP_CONSTANTCOMP  44
#define OP_FUNCTION      54
#define OP_FUNCTIONPARAM 55     /* V2.3 */
#define OP_FUNCTIONEND   56
#define OP_FUNCTIONCALL  57     /* V2.3: Aufruf (Inlining via Call-Stack) */
#define OP_VARIABLE      59
#define OP_LOAD          61
#define OP_STORE         62
#define OP_ACCESSCHAIN   65
#define OP_INBOUNDSAC    66
/* V2.6: Bitfeld-Ops (SPIR-V-Spez-Opcodes, integer-exakt) */
#define OP_BITFIELDINSERT   201
#define OP_BITFIELDSEXTRACT 202
#define OP_BITFIELDUEXTRACT 203
#define OP_BITREVERSE       204
#define OP_BITCOUNT         205
/* V2.6: Atomics (SPIR-V-Spez-Opcodes) */
#define OP_ATOMICLOAD    227
#define OP_ATOMICSTORE   228
#define OP_ATOMICEXCHANGE 229
#define OP_ATOMICIINC    232
#define OP_ATOMICIDEC    233
#define OP_ATOMICIADD    234
#define OP_ATOMICISUB    235
#define OP_ATOMICSMIN    236
#define OP_ATOMICUMIN    237
#define OP_ATOMICSMAX    238
#define OP_ATOMICUMAX    239
#define OP_ATOMICAND     240
#define OP_ATOMICOR      241
#define OP_ATOMICXOR     242
/* V3b: Subgroup / GroupNonUniform-Ops. Der Interpreter fuehrt EINE Invokation zugleich aus ->
 * die Subgroup ist genau 1 Lane breit (gl_SubgroupSize=1). Damit sind alle Reduktionen/Broadcasts
 * die Identitaet (Reduce/InclusiveScan = Wert, ExclusiveScan = neutrales Element), Elect = true,
 * All/Any = das Praedikat selbst -- korrekte Vulkan-Semantik fuer subgroupSize 1. */
#define OP_GROUP_ELECT          333
#define OP_GROUP_ALL            334
#define OP_GROUP_ANY            335
#define OP_GROUP_BROADCAST      337
#define OP_GROUP_BROADCASTFIRST 338
#define OP_GROUP_IADD           349
#define OP_GROUP_FADD           350
#define OP_GROUP_IMUL           351
#define OP_GROUP_SMIN           353
#define OP_GROUP_UMIN           354
#define OP_GROUP_SMAX           356
#define OP_GROUP_UMAX           357
#define OP_DECORATE      71
#define OP_MEMBERDECORATE 72
#define OP_VECEXTRACTDYN 77     /* V2.6: OpVectorExtractDynamic */
#define OP_VECINSERTDYN  78     /* V2.6: OpVectorInsertDynamic */
#define OP_VECTORSHUFFLE 79
#define OP_COMPOSITECONS 80
#define OP_COMPOSITEEXTR 81
#define OP_COMPOSITEINS  82     /* V2.5: OpCompositeInsert */
#define OP_CONVERTFTOU  109
#define OP_CONVERTFTOS  110
#define OP_CONVERTSTOF  111
#define OP_CONVERTUTOF  112
#define OP_BITCAST      124
#define OP_IADD         128
#define OP_ISUB         130
#define OP_IMUL         132
#define OP_FNEGATE      127
#define OP_SNEGATE      126
#define OP_FADD         129
#define OP_FSUB         131
#define OP_FMUL         133
#define OP_FDIV         136
#define OP_UDIV         134     /* V2.2 */
#define OP_SDIV         135
#define OP_UMOD         137
#define OP_SREM         138
#define OP_SMOD         139
#define OP_FREM         140
#define OP_FMOD         141
#define OP_VECTIMESSCAL 142
#define OP_MATTIMESSCAL 143     /* V2.2 */
#define OP_VECTIMESMAT  144
#define OP_MATTIMESVEC  145
#define OP_MATTIMESMAT  146
#define OP_OUTERPROD    147
#define OP_TRANSPOSE     84
#define OP_DOT          148
/* --- V2.2: Integer-/Logik-Vergleiche, Shift, Bitwise --- */
#define OP_LOGICALEQ    164
#define OP_LOGICALNEQ   165
#define OP_LOGICALOR    166
#define OP_LOGICALAND   167
#define OP_LOGICALNOT   168
#define OP_IEQUAL       170
#define OP_INOTEQUAL    171
#define OP_UGT          172
#define OP_SGT          173
#define OP_UGE          174
#define OP_SGE          175
#define OP_ULT          176
#define OP_SLT          177
#define OP_ULE          178
#define OP_SLE          179
#define OP_SHRLOG       194
#define OP_SHRARITH     195
#define OP_SHL          196
#define OP_BITOR        197
#define OP_BITXOR       198
#define OP_BITAND       199
#define OP_NOT          200
#define OP_SELECT       169
#define OP_FORDEQ       180
#define OP_FORDNEQ      182
#define OP_FORDLT       184
#define OP_FORDGT       186
#define OP_FORDLE       188
#define OP_FORDGE       190
#define OP_PHI          245
#define OP_LOOPMERGE    246
#define OP_SELECTMERGE  247
#define OP_LABEL        248
#define OP_BRANCH       249
#define OP_BRANCHCOND   250
#define OP_SWITCH       251     /* V2.3 */
#define OP_KILL         252     /* V2.3: Fragment-Discard */
#define OP_RETURN       253
#define OP_RETURNVALUE  254     /* V2.3 */
#define OP_UNREACHABLE  255     /* V2.3 */
#define OP_TERMINATE   4416     /* SPV_KHR_terminate_invocation (1.3) */
#define OP_NAME           5
#define OP_MEMBERNAME     6
#define OP_SOURCE         3
#define OP_SOURCEEXT      4
#define OP_STRING         7

#define SC_UNIFORMCONST 0
#define SC_INPUT  1
#define SC_UNIFORM 2
#define SC_OUTPUT 3
#define SC_PRIVATE 6
#define SC_FUNCTION 7
#define SC_PUSHCONST 9
#define SC_STORAGEBUFFER 12

#define DEC_BUILTIN  11
#define DEC_BINDING  33
#define DEC_DSET     34
#define DEC_LOCATION 30
#define DEC_OFFSET   35

#define BI_POSITION  0
#define BI_FRAGCOORD 15
#define BI_VERTEXINDEX   42
#define BI_INSTANCEINDEX 43
#define BI_GLOBALINVOCATION 28   /* V1.7: gl_GlobalInvocationID (uvec3) */
#define BI_SUBGROUP_SIZE       36   /* V3b: gl_SubgroupSize (=1 im 1-Lane-Modell) */
#define BI_SUBGROUP_INVOCATION 41   /* V3b: gl_SubgroupInvocationID (=0) */
/* Konstante Rueckgaben fuer die Subgroup-Builtins (Load-only; 1-Lane-Subgroup). */
static const spv_w g_bi_subgroup_size = { .u = 1 };
static const spv_w g_bi_subgroup_inv  = { .u = 0 };

/* Typklassen */
enum { T_NONE = 0, T_VOID, T_BOOL, T_INT, T_FLOAT, T_VEC, T_MAT, T_PTR, T_STRUCT, T_FN,
       T_IMAGE, T_SAMPLER, T_SAMPLEDIMG, T_ARRAY };   /* T_ARRAY: V1.7 */

/* GLSL.std.450 */
#define G_FABS      4
#define G_SQRT     31
#define G_INVSQRT  32
#define G_FMIN     37
#define G_FMAX     40
#define G_FCLAMP   43
#define G_FMIX     46
#define G_LENGTH   66
#define G_CROSS    68
#define G_NORMALIZE 69
/* --- V2.4: GLSL.std.450-Vollausbau --- */
#define G_ROUND     1
#define G_ROUNDEVEN 2
#define G_TRUNC     3
#define G_SABS      5
#define G_FSIGN     6
#define G_SSIGN     7
#define G_FLOOR     8
#define G_CEIL      9
#define G_FRACT    10
#define G_RADIANS  11
#define G_DEGREES  12
#define G_SIN      13
#define G_COS      14
#define G_TAN      15
#define G_ASIN     16
#define G_ACOS     17
#define G_ATAN     18
#define G_ATAN2    25
#define G_POW      26
#define G_EXP      27
#define G_LOG      28
#define G_EXP2     29
#define G_LOG2     30
#define G_UMIN     38
#define G_SMIN     39
#define G_UMAX     41
#define G_SMAX     42
#define G_UCLAMP   44
#define G_SCLAMP   45
#define G_STEP     48
#define G_SMOOTHSTEP 49
#define G_FMA      50
#define G_DISTANCE 67
#define G_FACEFWD  70
#define G_REFLECT  71
#define G_REFRACT  72

/* ---------------- Parsen ---------------- */
static int type_words(const spv_mod_t *m, unsigned tid)
{
    const spv_id_t *t = &m->ids[tid];
    switch (t->tclass) {
    case T_BOOL: case T_INT: case T_FLOAT: return 1;
    case T_VEC: return t->rows;
    case T_MAT: return t->cols * t->rows;
    case T_STRUCT: {
        int w = 0;
        for (int i = 0; i < t->nmember; i++) { w += type_words(m, t->mtype[i]); }
        return w;
    }
    case T_ARRAY: {   /* V1.7: elemwords * Laenge (cval[0].u); Runtime-Array (Laenge 0) -> Elementbreite */
        int ew = type_words(m, t->type);
        if (ew < 1) { ew = 1; }
        unsigned len = t->cval[0].u;
        return (len > 0) ? (int)(ew * (int)len) : ew;
    }
    case T_IMAGE: case T_SAMPLER: case T_SAMPLEDIMG: return 1;   /* opak: 1 Wort = Binding-Index */
    default: return 0;
    }
}

int spv_parse(spv_mod_t *m, const unsigned *words, unsigned nwords)
{
    if (!m || !words || nwords < 5 || words[0] != 0x07230203u) { return -1; }
    memset(m, 0, sizeof(*m));
    m->words  = words;
    m->nwords = nwords;
    m->bound  = words[3];
    if (m->bound == 0 || m->bound > SPV_MAX_IDS) { return -2; }
    m->lsx = m->lsy = m->lsz = 1;   /* V1.7: LocalSize-Default, ggf. von OpExecutionMode ueberschrieben */
    for (unsigned i = 0; i < SPV_MAX_IDS; i++) {
        m->ids[i].builtin = 0xFF;
        m->ids[i].location = 0xFF;
        m->ids[i].dset = 0xFF;
        m->ids[i].dbinding = 0xFF;
    }

    unsigned pc = 5;
    unsigned cur_fn = 0;
    while (pc < nwords) {
        unsigned w0 = words[pc];
        unsigned op = w0 & 0xFFFFu, wc = w0 >> 16;
        if (wc == 0 || pc + wc > nwords) { return -3; }
        const unsigned *o = &words[pc + 1];
        #define ID_OK(x) ((x) < m->bound)
        switch (op) {
        case OP_ENTRYPOINT:
            if (m->nentry < SPV_MAX_ENTRY && wc >= 3) {
                m->entry[m->nentry].model = o[0];
                m->entry[m->nentry].fn    = o[1];
                m->nentry++;
            }
            break;
        case OP_EXECUTIONMODE:      /* V1.7: LocalSize (Modus 17) -> lsx/lsy/lsz */
            if (wc >= 6 && o[1] == 17) { m->lsx = o[2]; m->lsy = o[3]; m->lsz = o[4]; }
            break;
        case OP_EXTINSTIMPORT:
            if (!ID_OK(o[0])) { return -4; }
            m->ids[o[0]].kind = 5;
            break;
        case OP_DECORATE:
            if (wc >= 3 && ID_OK(o[0])) {
                if (o[1] == DEC_LOCATION && wc >= 4) { m->ids[o[0]].location = (unsigned char)o[2]; }
                if (o[1] == DEC_BUILTIN  && wc >= 4) { m->ids[o[0]].builtin  = (unsigned char)o[2]; }
                if (o[1] == DEC_DSET     && wc >= 4) { m->ids[o[0]].dset      = (unsigned char)o[2]; }
                if (o[1] == DEC_BINDING  && wc >= 4) { m->ids[o[0]].dbinding  = (unsigned char)o[2]; }
            }
            break;
        case OP_MEMBERDECORATE:
            if (wc >= 4 && ID_OK(o[0]) && o[1] < SPV_MAX_MEMBER && o[2] == DEC_OFFSET && wc >= 5) {
                m->ids[o[0]].moff[o[1]] = o[3];
            }
            break;
        case OP_TYPEVOID:  if (!ID_OK(o[0])) { return -4; } m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_VOID; break;
        case OP_TYPEBOOL:  if (!ID_OK(o[0])) { return -4; } m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_BOOL; break;
        case OP_TYPEINT:   if (!ID_OK(o[0])) { return -4; } m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_INT;  break;
        case OP_TYPEFLOAT: if (!ID_OK(o[0])) { return -4; } m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_FLOAT; break;
        case OP_TYPEVECTOR:
            if (!ID_OK(o[0]) || !ID_OK(o[1]) || o[2] < 2 || o[2] > 4) { return -4; }
            m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_VEC;
            m->ids[o[0]].rows = (unsigned char)o[2]; m->ids[o[0]].type = o[1];
            break;
        case OP_TYPEMATRIX:
            if (!ID_OK(o[0]) || !ID_OK(o[1]) || o[2] < 2 || o[2] > 4) { return -4; }
            m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_MAT;
            m->ids[o[0]].cols = (unsigned char)o[2];
            m->ids[o[0]].rows = m->ids[o[1]].rows;
            m->ids[o[0]].type = o[1];
            break;
        case OP_TYPESTRUCT: {
            if (!ID_OK(o[0]) || wc - 2 > SPV_MAX_MEMBER) { return -4; }
            spv_id_t *t = &m->ids[o[0]];
            t->kind = 1; t->tclass = T_STRUCT; t->nmember = (unsigned char)(wc - 2);
            for (unsigned i = 0; i + 2 < wc; i++) { t->mtype[i] = o[1 + i]; }
            break;
        }
        case OP_TYPEARRAY:      /* V1.7: %arr = OpTypeArray %elem %lenConst */
            if (!ID_OK(o[0]) || !ID_OK(o[1]) || !ID_OK(o[2])) { return -4; }
            m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_ARRAY;
            m->ids[o[0]].type = o[1];                       /* Elementtyp */
            m->ids[o[0]].cval[0].u = m->ids[o[2]].cval[0].u;/* Laenge aus der Konstante */
            break;
        case OP_TYPERUNTIMEARR: /* V1.7: %rtarr = OpTypeRuntimeArray %elem (unbegrenzt) */
            if (!ID_OK(o[0]) || !ID_OK(o[1])) { return -4; }
            m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_ARRAY;
            m->ids[o[0]].type = o[1];
            m->ids[o[0]].cval[0].u = 0;                     /* 0 = Runtime-Laenge */
            break;
        case OP_TYPEPOINTER:
            if (!ID_OK(o[0]) || !ID_OK(o[2])) { return -4; }
            m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_PTR;
            m->ids[o[0]].storage = o[1]; m->ids[o[0]].type = o[2];
            break;
        case OP_TYPEFUNCTION:
            if (!ID_OK(o[0])) { return -4; }
            m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_FN;
            break;
        case OP_TYPEIMAGE:      if (!ID_OK(o[0])) { return -4; } m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_IMAGE; break;
        case OP_TYPESAMPLER:    if (!ID_OK(o[0])) { return -4; } m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_SAMPLER; break;
        case OP_TYPESAMPLEDIMG: if (!ID_OK(o[0])) { return -4; } m->ids[o[0]].kind = 1; m->ids[o[0]].tclass = T_SAMPLEDIMG; break;
        case OP_CONSTANT: {
            if (!ID_OK(o[0]) || !ID_OK(o[1]) || wc < 4) { return -4; }
            spv_id_t *c = &m->ids[o[1]];
            c->kind = 2; c->type = o[0];
            c->cval[0].u = o[2];
            break;
        }
        case OP_CONSTANTCOMP: {
            if (!ID_OK(o[0]) || !ID_OK(o[1])) { return -4; }
            spv_id_t *c = &m->ids[o[1]];
            c->kind = 2; c->type = o[0];
            int w = 0;
            for (unsigned i = 2; i + 1 < wc && w < SPV_VAL_WORDS; i++) {
                const spv_id_t *e = &m->ids[o[i]];
                int ew = type_words(m, e->type);
                for (int k = 0; k < ew && w < SPV_VAL_WORDS; k++) { c->cval[w++] = e->cval[k]; }
            }
            break;
        }
        case OP_VARIABLE:
            if (!ID_OK(o[0]) || !ID_OK(o[1])) { return -4; }
            m->ids[o[1]].kind = 3;
            m->ids[o[1]].type = o[0];         /* Pointer-Typ */
            m->ids[o[1]].storage = o[2];
            break;
        case OP_FUNCTION:
            if (!ID_OK(o[1])) { return -4; }
            m->ids[o[1]].kind = 4;
            m->ids[o[1]].word_off = pc;
            cur_fn = o[1];
            break;
        case OP_LABEL:
            if (!ID_OK(o[0])) { return -4; }
            m->ids[o[0]].kind = 6;
            m->ids[o[0]].word_off = pc;
            break;
        case OP_FUNCTIONEND:
            cur_fn = 0;
            break;
        default:
            break;                             /* Rest interessiert das Parsen nicht */
        }
        #undef ID_OK
        (void)cur_fn;
        pc += wc;
    }
    return 0;
}

int spv_find_entry(const spv_mod_t *m, int model, unsigned *fn)
{
    for (int i = 0; i < m->nentry; i++) {
        if ((int)m->entry[i].model == model) { *fn = m->entry[i].fn; return 0; }
    }
    return -1;
}

/* ---------------- Ausfuehrung ---------------- */
typedef struct {
    const spv_mod_t *m;
    spv_io_t *io;
    spv_w    val[SPV_MAX_IDS][SPV_VAL_WORDS];
    unsigned rtype[SPV_MAX_IDS];      /* Result-Typ jedes SSA-Werts (exakte Breiten) */
    spv_w   *pbase[SPV_MAX_IDS];      /* Pointer-Werte: Basis + Wort-Offset */
    unsigned poff[SPV_MAX_IDS];
    unsigned pcap[SPV_MAX_IDS];       /* verbleibende Woerter AB poff (Backing-Kapazitaet) */
    unsigned ptype[SPV_MAX_IDS];      /* Pointee-Typ des Pointer-Werts */
    unsigned bound_done[SPV_MAX_IDS]; /* 1 = Function-Variable bereits gebunden (kein Re-Bind) */
    spv_w    varpool[256];            /* Private/Function-Variablen */
    unsigned varused;
    struct { unsigned pc, rid; } callstk[16];   /* V2.3 OpFunctionCall: Ruecksprung-pc + Ergebnis-Id */
    int      callsp;
} spv_ctx_t;

static spv_ctx_t g_ctx;              /* single-threaded (eine Invokation zur Zeit) */

/* SICHERHEIT: .spv kommt von hdd1 (user-schreibbar) -> UNTRUSTED. Jede <id> aus dem Wortstrom
 * MUSS gegen bound geprueft werden, BEVOR sie ein Array (val/pbase/rtype/ids/...) indiziert --
 * sonst OOB-Read/Write im EL0-Prozess. value_of/width_of klemmen Lesezugriffe; fuer Schreib-
 * Result-Ids und Typ-/Pointer-Operanden erzwingt dieses Makro im Fehlerfall einen sauberen
 * Abbruch (fail-loud) statt eines wilden Zugriffs. */
#define IDOK(x, id)  ((id) < (x)->m->bound)

/* Opcodes mit Layout (o[0]=Ergebnis-Typ, o[1]=Ergebnis-<id>): fuer sie erzwingt spv_exec
 * VOR dem Handler wc>=4 + o[0]/o[1] < bound + Ergebnisbreite <= SPV_VAL_WORDS. Damit koennen
 * die Handler val[o[1]]/rtype[o[1]] und type_words(o[0]) ohne eigene Checks sicher indizieren. */
static int op_is_typed_result(unsigned op)
{
    switch (op) {
    case OP_COMPOSITEEXTR: case OP_COMPOSITEINS: case OP_COMPOSITECONS: case OP_VECTORSHUFFLE:
    case OP_VECEXTRACTDYN: case OP_VECINSERTDYN:
    case OP_FNEGATE: case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV:
    case OP_VECTIMESSCAL: case OP_MATTIMESVEC: case OP_DOT:
    case OP_FORDEQ: case OP_FORDNEQ: case OP_FORDLT: case OP_FORDGT:
    case OP_FORDLE: case OP_FORDGE: case OP_SELECT: case OP_EXTINST:
    case OP_CONVERTFTOU: case OP_CONVERTFTOS: case OP_CONVERTSTOF:
    case OP_CONVERTUTOF: case OP_BITCAST: case OP_IADD: case OP_ISUB: case OP_IMUL:
    case OP_IMAGESAMPLEIMPL: case OP_IMAGESAMPLEEXPL:
    case OP_IMAGEFETCH: case OP_IMAGEGATHER: case OP_IMAGEREAD: case OP_IMAGE: case OP_IMAGEQUERYSIZELOD: case OP_IMAGEQUERYSIZE:
    /* V2.2 */
    case OP_SNEGATE: case OP_UDIV: case OP_SDIV: case OP_UMOD: case OP_SREM: case OP_SMOD:
    case OP_FREM: case OP_FMOD: case OP_MATTIMESSCAL: case OP_VECTIMESMAT: case OP_MATTIMESMAT:
    case OP_OUTERPROD: case OP_TRANSPOSE:
    case OP_LOGICALEQ: case OP_LOGICALNEQ: case OP_LOGICALOR: case OP_LOGICALAND: case OP_LOGICALNOT:
    case OP_IEQUAL: case OP_INOTEQUAL: case OP_UGT: case OP_SGT: case OP_UGE: case OP_SGE:
    case OP_ULT: case OP_SLT: case OP_ULE: case OP_SLE:
    case OP_SHRLOG: case OP_SHRARITH: case OP_SHL: case OP_BITOR: case OP_BITXOR: case OP_BITAND: case OP_NOT:
    /* V2.6: Bitfeld-Ops */
    case OP_BITFIELDINSERT: case OP_BITFIELDSEXTRACT: case OP_BITFIELDUEXTRACT:
    case OP_BITREVERSE: case OP_BITCOUNT:
    /* V2.6: Atomics mit Ergebnis (AtomicStore hat keins) */
    case OP_ATOMICLOAD: case OP_ATOMICEXCHANGE: case OP_ATOMICIINC: case OP_ATOMICIDEC:
    case OP_ATOMICIADD: case OP_ATOMICISUB: case OP_ATOMICSMIN: case OP_ATOMICUMIN:
    case OP_ATOMICSMAX: case OP_ATOMICUMAX: case OP_ATOMICAND: case OP_ATOMICOR: case OP_ATOMICXOR:
    /* V3b: Subgroup/GroupNonUniform-Ops (typisiertes Ergebnis) */
    case OP_GROUP_ELECT: case OP_GROUP_ALL: case OP_GROUP_ANY:
    case OP_GROUP_BROADCAST: case OP_GROUP_BROADCASTFIRST:
    case OP_GROUP_IADD: case OP_GROUP_FADD: case OP_GROUP_IMUL:
    case OP_GROUP_SMIN: case OP_GROUP_UMIN: case OP_GROUP_SMAX: case OP_GROUP_UMAX:
        return 1;
    default:
        return 0;
    }
}

static const spv_w *value_of(spv_ctx_t *x, unsigned id)
{
    if (id >= x->m->bound) { return x->val[0]; }     /* Id 0 ist nie belegt -> Nullen */
    const spv_id_t *d = &x->m->ids[id];
    if (d->kind == 2) { return d->cval; }
    return x->val[id];
}

/* Breite (in Woertern) eines Werts: Konstante -> deklarierter Typ, SSA -> Result-Typ. */
static int width_of(spv_ctx_t *x, unsigned id)
{
    if (id >= x->m->bound) { return 1; }
    const spv_id_t *d = &x->m->ids[id];
    unsigned t = (d->kind == 2) ? d->type : x->rtype[id];
    int w = type_words(x->m, t);
    return (w > 0 && w <= SPV_VAL_WORDS) ? w : 1;
}

/* Typ-Id eines Werts (Konstante -> deklarierter Typ, SSA -> Result-Typ). 0 = unbekannt. */
static unsigned type_of(spv_ctx_t *x, unsigned id)
{
    if (id >= x->m->bound) { return 0; }
    const spv_id_t *d = &x->m->ids[id];
    return (d->kind == 2) ? d->type : x->rtype[id];
}

/* V2.5: Composite-Indizes (LITERALE aus dem Wortstrom) durch den Typbaum navigieren.
 * Liefert den Flat-Wort-Offset des adressierten Elements (im spaltweisen val-Layout) und
 * dessen Typ-Id in *out_type. Jeder Index wird gegen die reale Elementzahl GEKLEMMT ->
 * nie OOB (auch bei untrusted .spv). Rueckgabe <0 = Typfehler (Index in Skalar o.ae.). */
static int comp_offset(const spv_mod_t *m, unsigned tid, const unsigned *idx, int nidx,
                       unsigned *out_type)
{
    unsigned off = 0;
    for (int k = 0; k < nidx; k++) {
        if (tid == 0 || tid >= m->bound) { return -1; }
        const spv_id_t *t = &m->ids[tid];
        unsigned i = idx[k];
        if (t->tclass == T_VEC) {
            if (t->rows && i >= t->rows) { i = t->rows - 1; }
            off += i;                                   /* Komponente = 1 Wort */
            tid = t->type;
        } else if (t->tclass == T_MAT) {
            if (t->cols && i >= t->cols) { i = t->cols - 1; }
            int colw = type_words(m, t->type);          /* Spaltenvektor-Breite */
            if (colw < 1) { colw = 1; }
            off += i * (unsigned)colw;                  /* column-major */
            tid = t->type;
        } else if (t->tclass == T_ARRAY) {
            int ew = type_words(m, t->type);
            if (ew < 1) { ew = 1; }
            unsigned len = t->cval[0].u;
            if (len && i >= len) { i = len - 1; }
            off += i * (unsigned)ew;
            tid = t->type;
        } else if (t->tclass == T_STRUCT) {
            if (t->nmember && i >= t->nmember) { i = (unsigned)t->nmember - 1; }
            for (unsigned j = 0; j < i && j < SPV_MAX_MEMBER; j++) {
                int mw = type_words(m, t->mtype[j]);
                if (mw < 1) { mw = 1; }
                off += (unsigned)mw;
            }
            tid = t->mtype[i];
        } else {
            return -1;                                  /* Index in einen Skalar */
        }
    }
    if (off >= (unsigned)SPV_VAL_WORDS) { off = SPV_VAL_WORDS - 1; }   /* harte Sicherheits-Klemme */
    *out_type = tid;
    return (int)off;
}

/* Variable an ihren Speicher binden (beim Exec-Start bzw. bei OpVariable im Body).
 * Setzt pbase + pcap (verfuegbare Woerter der Backing-Region) -> Load/Store/AccessChain
 * koennen den Offset gegen pcap pruefen und so NIE ueber die reale Region hinausschreiben. */
static int bind_var(spv_ctx_t *x, unsigned id)
{
    if (!IDOK(x, id)) { return -14; }
    const spv_id_t *v = &x->m->ids[id];
    if (!IDOK(x, v->type)) { return -14; }
    const spv_id_t *pt = &x->m->ids[v->type];        /* Pointer-Typ */
    unsigned pointee = pt->type;
    if (!IDOK(x, pointee)) { return -14; }
    int w = type_words(x->m, pointee);
    if (w <= 0) { w = 1; }
    unsigned cap = (unsigned)w;
    switch (v->storage) {
    case SC_INPUT:
        if (v->builtin == BI_FRAGCOORD) { x->pbase[id] = (spv_w *)(void *)x->io->frag_coord; cap = 4; }
        else if (v->builtin == BI_VERTEXINDEX) { x->pbase[id] = (spv_w *)(void *)&x->io->vertex_index; cap = 1; }
        else if (v->builtin == BI_INSTANCEINDEX) { x->pbase[id] = (spv_w *)(void *)&x->io->instance_index; cap = 1; }
        else if (v->builtin == BI_GLOBALINVOCATION) { x->pbase[id] = (spv_w *)(void *)x->io->global_id; cap = 3; } /* V1.7 */
        else if (v->builtin == BI_SUBGROUP_SIZE)       { x->pbase[id] = (spv_w *)(void *)&g_bi_subgroup_size; cap = 1; } /* V3b: =1 */
        else if (v->builtin == BI_SUBGROUP_INVOCATION) { x->pbase[id] = (spv_w *)(void *)&g_bi_subgroup_inv;  cap = 1; } /* V3b: =0 */
        else if (v->location < SPV_MAX_LOC) { x->pbase[id] = (spv_w *)(void *)x->io->in_loc[v->location]; cap = 4; }
        else { return -10; }
        break;
    case SC_OUTPUT:
        if (v->builtin == BI_POSITION) { x->pbase[id] = (spv_w *)(void *)x->io->builtin_pos; cap = 4; }
        else if (v->location < SPV_MAX_LOC) { x->pbase[id] = (spv_w *)(void *)x->io->out_loc[v->location]; cap = 4; }
        else { return -10; }
        break;
    case SC_PUSHCONST:
        if (!x->io->push) { return -11; }
        /* const weg-casten: der Interpreter SCHREIBT nie in PushConstant (Load-only). */
        x->pbase[id] = (spv_w *)(unsigned long)(unsigned long)x->io->push;
        cap = x->io->push_bytes / 4u;                /* Push-Block ist Load-only + laengenbegrenzt */
        break;
    case SC_UNIFORM:
    case SC_STORAGEBUFFER: {
        /* V1.3: Descriptor-gestuetzt (Set 0, Binding) an die gebundene Puffer-Memory. Uniform =
         * Load-only; StorageBuffer ist ausserdem beschreibbar (V1.7 Compute: OpStore prueft pcap
         * -> kein OOB). Subset: nur Set 0. */
        if (v->dset != 0 || v->dbinding >= SPV_MAX_DESCRIPTOR || !x->io->ubo[v->dbinding]) { return -15; }
        x->pbase[id] = (spv_w *)(unsigned long)(unsigned long)x->io->ubo[v->dbinding];
        cap = x->io->ubo_bytes[v->dbinding] / 4u;
        break;
    }
    case SC_UNIFORMCONST:
        /* V1.4: Sampler/Bild/Combined -- OPAK (keine Memory). OpLoad liefert den Binding-Index
         * als "Handle"; OpImageSample* schlaegt darueber io->tex[binding] nach. */
        x->pbase[id] = 0; x->pcap[id] = 0; x->ptype[id] = pointee;
        return 0;
    case SC_PRIVATE:
    case SC_FUNCTION:
        if (x->varused + (unsigned)w > sizeof(x->varpool) / sizeof(x->varpool[0])) { return -12; }
        x->pbase[id] = &x->varpool[x->varused];
        x->varused += (unsigned)w;
        cap = (unsigned)w;
        break;
    default:
        return -13;
    }
    x->poff[id]  = 0;
    x->pcap[id]  = cap;
    x->ptype[id] = pointee;
    return 0;
}

/* V1.4: eine Textur (B8G8R8A8_UNORM) an (u,v) in [0,1] samplen -> rgba[4]. Nearest oder
 * bilinear; Wrap repeat/clamp. tx->pixels != 0 vom Aufrufer geprueft. */
static float wrap_coord(float c, int wrap)
{
    if (wrap == 1) { return c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c); }   /* clamp-to-edge */
    c = c - (float)(int)c;                                               /* repeat: frac */
    if (c < 0.0f) { c += 1.0f; }
    return c;
}
/* V2.6: float-Kanal [0,1] -> 8-bit (0.0/1.0 exakt). */
static unsigned f2b(float f)
{
    if (f <= 0.0f) { return 0; }
    if (f >= 1.0f) { return 255; }
    return (unsigned)(f * 255.0f + 0.5f);
}
static void texel_rgba(const void *pix, unsigned pitch_px, int tx, int ty, float *o)
{
    unsigned w = ((const unsigned *)pix)[(unsigned)ty * pitch_px + (unsigned)tx];
    o[0] = (float)((w >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((w >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(w & 0xFF) / 255.0f;
    o[3] = (float)((w >> 24) & 0xFF) / 255.0f;
}
static void sample_tex(const spv_tex_t *t, float u, float v, spv_w *out)
{
    u = wrap_coord(u, t->wrap); v = wrap_coord(v, t->wrap);
    float rgba[4];
    if (t->filter == 1) {                                    /* bilinear */
        float fx = u * (float)t->w - 0.5f, fy = v * (float)t->h - 0.5f;
        int x0 = (int)(fx < 0 ? fx - 1 : fx), y0 = (int)(fy < 0 ? fy - 1 : fy);
        float dx = fx - (float)x0, dy = fy - (float)y0;
        int xs[2] = { x0, x0 + 1 }, ys[2] = { y0, y0 + 1 };
        for (int k = 0; k < 2; k++) {
            if (xs[k] < 0) { xs[k] = (t->wrap == 1) ? 0 : (int)t->w - 1; }
            if (xs[k] >= (int)t->w) { xs[k] = (t->wrap == 1) ? (int)t->w - 1 : 0; }
            if (ys[k] < 0) { ys[k] = (t->wrap == 1) ? 0 : (int)t->h - 1; }
            if (ys[k] >= (int)t->h) { ys[k] = (t->wrap == 1) ? (int)t->h - 1 : 0; }
        }
        float c00[4], c10[4], c01[4], c11[4];
        texel_rgba(t->pixels, t->pitch_px, xs[0], ys[0], c00);
        texel_rgba(t->pixels, t->pitch_px, xs[1], ys[0], c10);
        texel_rgba(t->pixels, t->pitch_px, xs[0], ys[1], c01);
        texel_rgba(t->pixels, t->pitch_px, xs[1], ys[1], c11);
        for (int i = 0; i < 4; i++) {
            float a = c00[i] + (c10[i] - c00[i]) * dx;
            float b = c01[i] + (c11[i] - c01[i]) * dx;
            rgba[i] = a + (b - a) * dy;
        }
    } else {                                                 /* nearest */
        int tx = (int)(u * (float)t->w), ty = (int)(v * (float)t->h);
        if (tx >= (int)t->w) { tx = (int)t->w - 1; } if (tx < 0) { tx = 0; }
        if (ty >= (int)t->h) { ty = (int)t->h - 1; } if (ty < 0) { ty = 0; }
        texel_rgba(t->pixels, t->pitch_px, tx, ty, rgba);
    }
    out[0].f = rgba[0]; out[1].f = rgba[1]; out[2].f = rgba[2]; out[3].f = rgba[3];
}

/* V2.6: textureGather -- sammelt EINE Komponente der 4 Bilinear-Footprint-Texel.
 * Reihenfolge nach Vulkan-Spez 15.9.1: (i0,j1),(i1,j1),(i1,j0),(i0,j0). */
static void gather_tex(const spv_tex_t *t, float u, float v, int comp, spv_w *out)
{
    u = wrap_coord(u, t->wrap); v = wrap_coord(v, t->wrap);
    float fx = u * (float)t->w - 0.5f, fy = v * (float)t->h - 0.5f;
    int x0 = (int)(fx < 0 ? fx - 1 : fx), y0 = (int)(fy < 0 ? fy - 1 : fy);
    int xs[2] = { x0, x0 + 1 }, ys[2] = { y0, y0 + 1 };
    for (int k = 0; k < 2; k++) {
        if (xs[k] < 0) { xs[k] = (t->wrap == 1) ? 0 : (int)t->w - 1; }
        if (xs[k] >= (int)t->w) { xs[k] = (t->wrap == 1) ? (int)t->w - 1 : 0; }
        if (ys[k] < 0) { ys[k] = (t->wrap == 1) ? 0 : (int)t->h - 1; }
        if (ys[k] >= (int)t->h) { ys[k] = (t->wrap == 1) ? (int)t->h - 1 : 0; }
    }
    if (comp < 0 || comp > 3) { comp = 0; }
    float c[4];
    texel_rgba(t->pixels, t->pitch_px, xs[0], ys[1], c); out[0].f = c[comp];
    texel_rgba(t->pixels, t->pitch_px, xs[1], ys[1], c); out[1].f = c[comp];
    texel_rgba(t->pixels, t->pitch_px, xs[1], ys[0], c); out[2].f = c[comp];
    texel_rgba(t->pixels, t->pitch_px, xs[0], ys[0], c); out[3].f = c[comp];
}

int spv_exec(const spv_mod_t *m, unsigned fn, spv_io_t *io)
{
    spv_ctx_t *x = &g_ctx;
    if (!m || fn >= m->bound || m->ids[fn].kind != 4 || !io) { return -20; }
    /* Nur bis 'bound' nullen: spv_exec laeuft im Draw-Pfad JE FRAGMENT -- ein voller
     * 14-KiB-memset waere der heisseste Punkt der ganzen Pipeline. Ids >= bound werden
     * nie gelesen (value_of/width_of klemmen auf bound). */
    memset(x->val, 0, (unsigned long)m->bound * sizeof(x->val[0]));
    memset(x->pbase, 0, (unsigned long)m->bound * sizeof(x->pbase[0]));
    memset(x->rtype, 0, (unsigned long)m->bound * sizeof(x->rtype[0]));
    memset(x->bound_done, 0, (unsigned long)m->bound * sizeof(x->bound_done[0]));
    x->m = m; x->io = io; x->varused = 0; x->callsp = 0;

    /* Globale Variablen binden (alles ausser Function-Storage). */
    for (unsigned i = 1; i < m->bound; i++) {
        if (m->ids[i].kind == 3 && m->ids[i].storage != SC_FUNCTION) {
            int r = bind_var(x, i);
            if (r < 0) { return r; }
        }
    }

    const unsigned *W = m->words;
    unsigned pc = m->ids[fn].word_off;
    pc += (W[pc] >> 16);                             /* hinter OpFunction */
    unsigned prev_label = 0, cur_label = 0;
    int steps = 0;

    while (pc < m->nwords) {
        if (++steps > 100000) { return -21; }
        unsigned w0 = W[pc];
        unsigned op = w0 & 0xFFFFu, wc = w0 >> 16;
        if (wc == 0 || pc + wc > m->nwords) { return -22; }
        const unsigned *o = &W[pc + 1];

        /* SICHERHEIT (untrusted .spv): fuer alle "typed result"-Opcodes zentral pruefen, dass
         * Typ-<id> o[0] und Ergebnis-<id> o[1] gueltig sind und die Ergebnisbreite in val[]
         * passt -> die Handler unten schreiben val[o[1]]/rtype[o[1]] dann garantiert in-bounds. */
        if (op_is_typed_result(op)) {
            if (wc < 4 || !IDOK(x, o[0]) || !IDOK(x, o[1])) { return -40; }
            int rw = type_words(m, o[0]);
            if (rw < 0 || rw > SPV_VAL_WORDS) { return -44; }
        }

        switch (op) {
        case OP_LABEL: {
            prev_label = cur_label;
            cur_label  = o[0];
            /* Simultane Phis am Blockanfang: erst ALLE Quellen lesen, dann zuweisen. */
            unsigned p2 = pc + wc;
            spv_w stage[8][SPV_VAL_WORDS];
            unsigned dest[8]; int nphi = 0;
            while (p2 < m->nwords && ((W[p2] & 0xFFFFu) == OP_PHI) && nphi < 8) {
                unsigned pwc = W[p2] >> 16;
                const unsigned *po = &W[p2 + 1];
                unsigned rtype = po[0], rid = po[1];
                int w = type_words(m, rtype);
                int found = 0;
                for (unsigned k = 2; k + 2 < pwc; k += 2) {   /* Paare in po[2..pwc-2] */
                    if (po[k + 1] == prev_label) {
                        const spv_w *src = value_of(x, po[k]);
                        for (int j = 0; j < w && j < SPV_VAL_WORDS; j++) { stage[nphi][j] = src[j]; }
                        found = 1;
                        break;
                    }
                }
                if (!found) { return -23; }
                x->rtype[rid] = rtype;
                dest[nphi++] = rid;
                p2 += pwc;
            }
            for (int i = 0; i < nphi; i++) {
                for (int j = 0; j < SPV_VAL_WORDS; j++) { x->val[dest[i]][j] = stage[i][j]; }
            }
            pc = p2;
            continue;                                 /* Phis sind konsumiert */
        }
        case OP_PHI:
            return -24;                               /* nur direkt nach OpLabel erlaubt */
        case OP_VARIABLE: {                           /* Function-lokale Variable */
            if (wc < 3 || !IDOK(x, o[1])) { return -40; }
            if (!x->bound_done[o[1]]) {                /* nur EINMAL binden (Schleifen -> kein varpool-Leck) */
                int r = bind_var(x, o[1]);
                if (r < 0) { return r; }
                x->bound_done[o[1]] = 1;
            }
            break;
        }
        case OP_LOAD: {
            if (wc < 4 || !IDOK(x, o[0]) || !IDOK(x, o[1]) || !IDOK(x, o[2])) { return -40; }
            unsigned rid = o[1], ptr = o[2];
            /* V1.4: Load eines Sampler/Combined-Image-Variablen -> Binding-Index als "Handle". */
            if (m->ids[ptr].kind == 3 && m->ids[ptr].storage == SC_UNIFORMCONST) {
                x->val[rid][0].u = m->ids[ptr].dbinding;
                x->rtype[rid] = o[0];
                break;
            }
            if (!x->pbase[ptr]) { return -25; }
            int w = type_words(m, o[0]);
            if (w < 0 || w > SPV_VAL_WORDS || (unsigned)w > x->pcap[ptr]) { return -41; }  /* OOB-Read blocken */
            const spv_w *src = x->pbase[ptr] + x->poff[ptr];
            for (int i = 0; i < w; i++) { x->val[rid][i] = src[i]; }
            x->rtype[rid] = o[0];
            break;
        }
        case OP_STORE: {
            if (wc < 3 || !IDOK(x, o[0]) || !IDOK(x, o[1])) { return -40; }
            unsigned ptr = o[0];
            if (!x->pbase[ptr]) { return -26; }
            int w = type_words(m, x->ptype[ptr]);
            if (w < 0 || w > SPV_VAL_WORDS || (unsigned)w > x->pcap[ptr]) { return -42; }  /* OOB-Write blocken */
            const spv_w *src = value_of(x, o[1]);
            spv_w *dst = x->pbase[ptr] + x->poff[ptr];
            for (int i = 0; i < w; i++) { dst[i] = src[i]; }
            break;
        }
        case OP_ATOMICLOAD: {                          /* V2.6: %r = atomicLoad(*ptr) */
            unsigned ptr = o[2];
            spv_w *d = (ptr < m->bound && x->pbase[ptr] && x->pcap[ptr] >= 1) ? x->pbase[ptr] + x->poff[ptr] : 0;
            x->val[o[1]][0].u = d ? d->u : 0;
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_ATOMICSTORE: {                         /* V2.6: kein Ergebnis: %ptr %scope %sem %value */
            unsigned ptr = o[0];
            spv_w *d = (ptr < m->bound && x->pbase[ptr] && x->pcap[ptr] >= 1) ? x->pbase[ptr] + x->poff[ptr] : 0;
            if (wc >= 4 && d) { d->u = value_of(x, o[3])[0].u; }
            break;
        }
        case OP_ATOMICIINC: case OP_ATOMICIDEC: {      /* V2.6: %r = alt; *ptr += / -= 1 */
            unsigned ptr = o[2];
            spv_w *d = (ptr < m->bound && x->pbase[ptr] && x->pcap[ptr] >= 1) ? x->pbase[ptr] + x->poff[ptr] : 0;
            unsigned old = d ? d->u : 0;
            x->val[o[1]][0].u = old;
            if (d) { d->u = (op == OP_ATOMICIINC) ? old + 1u : old - 1u; }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_ATOMICEXCHANGE: case OP_ATOMICIADD: case OP_ATOMICISUB:
        case OP_ATOMICSMIN: case OP_ATOMICUMIN: case OP_ATOMICSMAX: case OP_ATOMICUMAX:
        case OP_ATOMICAND: case OP_ATOMICOR: case OP_ATOMICXOR: {   /* V2.6: %r=alt; *ptr=f(alt,val) */
            if (wc < 7) { return -46; }                /* op,type,r,ptr,scope,sem,value */
            unsigned ptr = o[2];
            spv_w *d = (ptr < m->bound && x->pbase[ptr] && x->pcap[ptr] >= 1) ? x->pbase[ptr] + x->poff[ptr] : 0;
            unsigned val = value_of(x, o[5])[0].u;
            unsigned old = d ? d->u : 0;
            x->val[o[1]][0].u = old;                   /* Atomics geben den ALTEN Wert zurueck */
            if (d) {
                unsigned nv = old;
                switch (op) {
                case OP_ATOMICEXCHANGE: nv = val; break;
                case OP_ATOMICIADD: nv = old + val; break;
                case OP_ATOMICISUB: nv = old - val; break;
                case OP_ATOMICSMIN: nv = ((int)old < (int)val) ? old : val; break;
                case OP_ATOMICUMIN: nv = (old < val) ? old : val; break;
                case OP_ATOMICSMAX: nv = ((int)old > (int)val) ? old : val; break;
                case OP_ATOMICUMAX: nv = (old > val) ? old : val; break;
                case OP_ATOMICAND: nv = old & val; break;
                case OP_ATOMICOR:  nv = old | val; break;
                default:           nv = old ^ val; break;   /* OP_ATOMICXOR */
                }
                d->u = nv;
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_ACCESSCHAIN:
        case OP_INBOUNDSAC: {
            if (wc < 4 || !IDOK(x, o[0]) || !IDOK(x, o[1]) || !IDOK(x, o[2])) { return -40; }
            unsigned rid = o[1], base = o[2];
            if (!x->pbase[base]) { return -27; }
            spv_w *b = x->pbase[base];
            unsigned off = x->poff[base];
            unsigned cap = x->pcap[base];              /* verbleibende Woerter ab off */
            unsigned t = x->ptype[base];
            /* Operanden sind o[0..wc-2] (wc zaehlt das Opcode-Wort mit!) -> i < wc-1,
             * sonst wuerde das ERSTE WORT DER NAECHSTEN INSTRUKTION als Index-Id gelesen. */
            for (unsigned i = 3; i + 1 < wc; i++) {
                if (!IDOK(x, t) || !IDOK(x, o[i])) { return -40; }
                const spv_id_t *td = &m->ids[t];
                int idx = (int)value_of(x, o[i])[0].u;   /* Konstante ODER Wert */
                unsigned step;                            /* Wort-Vorschub UND Kapazitaets-Abzug */
                if (td->tclass == T_STRUCT) {
                    if (idx < 0) { idx = 0; }
                    if (td->nmember == 0 || idx >= td->nmember) { idx = (int)td->nmember - 1; }
                    if (idx < 0) { return -28; }
                    step = td->moff[idx] / 4u;
                    t = td->mtype[idx];
                } else if (td->tclass == T_VEC) {
                    if (idx < 0) { idx = 0; }
                    if (idx >= td->rows) { idx = td->rows - 1; }
                    step = (unsigned)idx;
                    t = td->type;                        /* float */
                } else if (td->tclass == T_MAT) {
                    if (idx < 0) { idx = 0; }
                    if (idx >= td->cols) { idx = td->cols - 1; }
                    step = (unsigned)idx * td->rows;
                    t = td->type;                        /* Spaltenvektor */
                } else if (td->tclass == T_ARRAY) {      /* V1.7: dyn. Index -> element_words-Schritt */
                    int ew = type_words(m, td->type);
                    if (ew < 1) { ew = 1; }
                    if (idx < 0) { idx = 0; }
                    /* SICHERHEIT: idx GEGEN die Backing-Kapazitaet klemmen VOR der Multiplikation
                     * (untrusted .spv: verhindert unsigned-Overflow von idx*ew, der die step<cap-
                     * Pruefung umgehen wuerde). robustBufferAccess: OOB-Index -> geklemmt. */
                    if ((unsigned)idx >= cap) { idx = (cap > 0) ? (int)(cap - 1) : 0; }
                    step = (unsigned)idx * (unsigned)ew;
                    t = td->type;                        /* Elementtyp */
                } else {
                    return -28;
                }
                /* SICHERHEIT: Wort-Offset NIE ueber die Backing-Kapazitaet hinaus akkumulieren
                 * (auch nicht durch praeparierte OpMemberDecorate-Offsets) -> kein OOB. */
                if (step >= cap) { return -43; }
                off += step;
                cap -= step;
            }
            x->pbase[rid] = b; x->poff[rid] = off; x->pcap[rid] = cap;
            /* Pointee-Typ des ERGEBNIS-Pointers steht im Result-Typ (o[0]). */
            x->ptype[rid] = m->ids[o[0]].type;
            break;
        }
        case OP_COMPOSITEEXTR: {                       /* V2.5: mehrstufige Indizes + Composite-Ergebnis */
            unsigned rid = o[1];
            const spv_w *src = value_of(x, o[2]);
            unsigned lt = 0;
            int off = comp_offset(m, type_of(x, o[2]), &o[3], (int)wc - 4, &lt);
            if (off < 0) { return -29; }
            int rw = type_words(m, o[0]);              /* Breite des extrahierten (Sub-)Typs */
            if (rw < 1) { rw = 1; }
            for (int i = 0; i < rw && off + i < SPV_VAL_WORDS; i++) { x->val[rid][i] = src[off + i]; }
            x->rtype[rid] = o[0];
            break;
        }
        case OP_COMPOSITEINS: {                        /* V2.5: %r = Insert %object in %composite an idx... */
            unsigned rid = o[1];
            const spv_w *obj  = value_of(x, o[2]);
            const spv_w *base = value_of(x, o[3]);
            int rw = type_words(m, o[0]);              /* Ergebnis hat den Composite-Typ */
            if (rw < 1) { rw = 1; } if (rw > SPV_VAL_WORDS) { rw = SPV_VAL_WORDS; }
            for (int i = 0; i < rw; i++) { x->val[rid][i] = base[i]; }   /* Basis kopieren */
            unsigned lt = 0;
            int off = comp_offset(m, o[0], &o[4], (int)wc - 5, &lt);
            if (off < 0) { return -29; }
            int ow = type_words(m, lt);                /* Breite des eingefuegten Elements */
            if (ow < 1) { ow = 1; }
            for (int i = 0; i < ow && off + i < SPV_VAL_WORDS; i++) { x->val[rid][off + i] = obj[i]; }
            x->rtype[rid] = o[0];
            break;
        }
        case OP_COMPOSITECONS: {
            unsigned rid = o[1];
            int w = 0;
            for (unsigned i = 2; i + 1 < wc && w < SPV_VAL_WORDS; i++) {   /* o[0..wc-2]! */
                int ew = width_of(x, o[i]);            /* Skalar ODER Vektor, exakte Breite */
                const spv_w *src = value_of(x, o[i]);
                for (int k = 0; k < ew && w < SPV_VAL_WORDS; k++) { x->val[rid][w++] = src[k]; }
            }
            x->rtype[rid] = o[0];
            break;
        }
        case OP_VECEXTRACTDYN: {                       /* V2.6: %r = vector[index] (dynamischer Index) */
            const spv_w *v = value_of(x, o[2]);
            unsigned n = (unsigned)width_of(x, o[2]);
            unsigned i = value_of(x, o[3])[0].u;
            if (n && i >= n) { i = n - 1; }            /* gegen reale Vektorbreite klemmen */
            if (i >= (unsigned)SPV_VAL_WORDS) { i = SPV_VAL_WORDS - 1; }
            x->val[o[1]][0] = v[i];
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_VECINSERTDYN: {                        /* V2.6: %r = vector mit vector[index]=component */
            const spv_w *v = value_of(x, o[2]);
            const spv_w *comp = value_of(x, o[3]);
            unsigned i = value_of(x, o[4])[0].u;
            int w = type_words(m, o[0]);
            if (w < 1) { w = 1; } if (w > SPV_VAL_WORDS) { w = SPV_VAL_WORDS; }
            for (int k = 0; k < w; k++) { x->val[o[1]][k] = v[k]; }
            if (i < (unsigned)w) { x->val[o[1]][i] = comp[0]; }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_VECTORSHUFFLE: {
            unsigned rid = o[1];
            const spv_w *v1 = value_of(x, o[2]);
            const spv_w *v2 = value_of(x, o[3]);
            unsigned n1 = (unsigned)width_of(x, o[2]); /* exakte Breite des 1. Vektors */
            int w = type_words(m, o[0]);
            if (wc < 5 || (unsigned)w > wc - 5) { return -34; }   /* Komponenten in o[4..wc-2] */
            for (int i = 0; i < w; i++) {
                unsigned c = o[4 + i];
                /* SICHERHEIT: Komponenten-Index gegen die realen Vektorbreiten klemmen
                 * (v1/v2 sind val-Arrays mit SPV_VAL_WORDS Woertern) -> kein OOB-Read. */
                if (c == 0xFFFFFFFFu) { x->val[rid][i].u = 0; }
                else if (c < n1) { x->val[rid][i] = v1[c]; }
                else if (c - n1 < (unsigned)SPV_VAL_WORDS) { x->val[rid][i] = v2[c - n1]; }
                else { x->val[rid][i].u = 0; }
            }
            x->rtype[rid] = o[0];
            break;
        }
        case OP_FNEGATE: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            for (int i = 0; i < w; i++) { x->val[o[1]][i].f = -a[i].f; }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_CONVERTSTOF: case OP_CONVERTUTOF:
        case OP_CONVERTFTOS: case OP_CONVERTFTOU: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            for (int i = 0; i < w; i++) {
                if (op == OP_CONVERTSTOF)      { x->val[o[1]][i].f = (float)a[i].i; }
                else if (op == OP_CONVERTUTOF) { x->val[o[1]][i].f = (float)a[i].u; }
                else if (op == OP_CONVERTFTOS) { x->val[o[1]][i].i = (int)a[i].f; }
                else                           { x->val[o[1]][i].u = (unsigned)a[i].f; }
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_BITCAST: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            for (int i = 0; i < w; i++) { x->val[o[1]][i] = a[i]; }   /* rohe Bits */
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_IADD: case OP_ISUB: case OP_IMUL: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            const spv_w *b = value_of(x, o[3]);
            for (int i = 0; i < w; i++) {
                if (op == OP_IADD)      { x->val[o[1]][i].i = a[i].i + b[i].i; }
                else if (op == OP_ISUB) { x->val[o[1]][i].i = a[i].i - b[i].i; }
                else                    { x->val[o[1]][i].i = a[i].i * b[i].i; }
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        /* V3b: Subgroup/GroupNonUniform-Ops im 1-Lane-Modell (o[2]=Execution-Scope, ignoriert). */
        case OP_GROUP_ELECT:                              /* Elect: einzige Lane -> immer true */
            x->val[o[1]][0].u = 1; x->rtype[o[1]] = o[0]; break;
        case OP_GROUP_ALL: case OP_GROUP_ANY: {           /* All/Any(pred) -> pred (1 Lane) */
            const spv_w *p = value_of(x, o[3]);
            x->val[o[1]][0] = p[0]; x->rtype[o[1]] = o[0]; break;
        }
        case OP_GROUP_BROADCAST: case OP_GROUP_BROADCASTFIRST: {   /* Broadcast(value[,id]) -> value */
            int w = type_words(m, o[0]);
            const spv_w *v = value_of(x, o[3]);
            for (int i = 0; i < w; i++) { x->val[o[1]][i] = v[i]; }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_GROUP_IADD: case OP_GROUP_FADD: case OP_GROUP_IMUL:
        case OP_GROUP_SMIN: case OP_GROUP_UMIN: case OP_GROUP_SMAX: case OP_GROUP_UMAX: {
            /* Arithmetik: o[3]=GroupOperation (0=Reduce,1=InclusiveScan,2=ExclusiveScan), o[4]=Wert.
             * 1 Lane -> Reduce/Inclusive = Wert; Exclusive = neutrales Element (Add=0, Mul=1, sonst=Wert). */
            int w = type_words(m, o[0]);
            unsigned oper = o[3];
            const spv_w *v = value_of(x, o[4]);
            for (int i = 0; i < w; i++) {
                if (oper == 2) {                          /* ExclusiveScan -> neutrales Element */
                    if (op == OP_GROUP_IADD)      { x->val[o[1]][i].u = 0; }
                    else if (op == OP_GROUP_FADD) { x->val[o[1]][i].f = 0.0f; }
                    else if (op == OP_GROUP_IMUL) { x->val[o[1]][i].u = 1; }
                    else                          { x->val[o[1]][i] = v[i]; }   /* Min/Max: Wert */
                } else {                                  /* Reduce / InclusiveScan -> Wert */
                    x->val[o[1]][i] = v[i];
                }
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_SNEGATE: {                                   /* V2.2 */
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            for (int i = 0; i < w; i++) { x->val[o[1]][i].i = -a[i].i; }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_UDIV: case OP_SDIV: case OP_UMOD: case OP_SREM: case OP_SMOD: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            const spv_w *b = value_of(x, o[3]);
            for (int i = 0; i < w; i++) {
                spv_w r; r.u = 0;
                if (op == OP_UDIV)      { r.u = b[i].u ? a[i].u / b[i].u : 0xFFFFFFFFu; }   /* Div/0 -> robuster Wert */
                else if (op == OP_SDIV) { r.i = b[i].i ? a[i].i / b[i].i : -1; }
                else if (op == OP_UMOD) { r.u = b[i].u ? a[i].u % b[i].u : a[i].u; }
                else if (op == OP_SREM) { r.i = b[i].i ? a[i].i % b[i].i : a[i].i; }         /* Rest: Vorzeichen des Dividenden */
                else {                                                                        /* SMOD: Vorzeichen des Divisors */
                    if (b[i].i) { int q = a[i].i % b[i].i; if (q != 0 && ((q < 0) != (b[i].i < 0))) { q += b[i].i; } r.i = q; }
                    else { r.i = a[i].i; }
                }
                x->val[o[1]][i] = r;
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_FREM: case OP_FMOD: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            const spv_w *b = value_of(x, o[3]);
            for (int i = 0; i < w; i++) {
                float x0 = a[i].f, y0 = b[i].f, r = 0.0f;
                if (y0 != 0.0f) {
                    int qi = (int)(x0 / y0);                 /* Richtung Null (FRem) */
                    r = x0 - (float)qi * y0;
                    if (op == OP_FMOD && r != 0.0f && ((r < 0.0f) != (y0 < 0.0f))) { r += y0; }  /* FMod: Vorzeichen des Divisors */
                }
                x->val[o[1]][i].f = r;
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_MATTIMESSCAL: {
            int w = type_words(m, o[0]);
            const spv_w *M = value_of(x, o[2]);
            float s = value_of(x, o[3])[0].f;
            for (int i = 0; i < w; i++) { x->val[o[1]][i].f = M[i].f * s; }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_VECTIMESMAT: {                               /* Zeilenvektor V (rows) x Matrix -> vec(cols) */
            int cols = m->ids[o[0]].rows;                    /* Ergebnisbreite = Matrix-Spaltenzahl */
            int rows = width_of(x, o[2]);                    /* Vektorbreite = Matrix-Zeilenzahl */
            if (cols < 1 || rows < 1 || cols * rows > SPV_VAL_WORDS) { return -45; }
            const spv_w *V = value_of(x, o[2]);
            const spv_w *M = value_of(x, o[3]);
            for (int c = 0; c < cols; c++) {
                float sacc = 0.0f;
                for (int r = 0; r < rows; r++) { sacc += V[r].f * M[c * rows + r].f; }
                x->val[o[1]][c].f = sacc;
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_MATTIMESMAT: {                               /* A (Arows x Acols) * B (Acols x Bcols) */
            unsigned ta = x->rtype[o[2]], tb = x->rtype[o[3]];
            int Arows = m->ids[ta].rows, Acols = m->ids[ta].cols, Bcols = m->ids[tb].cols;
            if (Arows < 1 || Acols < 1 || Bcols < 1 ||
                Arows * Bcols > SPV_VAL_WORDS || Arows * Acols > SPV_VAL_WORDS) { return -45; }
            const spv_w *A = value_of(x, o[2]);
            const spv_w *B = value_of(x, o[3]);
            for (int c = 0; c < Bcols; c++) {
                for (int r = 0; r < Arows; r++) {
                    float sacc = 0.0f;
                    for (int k = 0; k < Acols; k++) { sacc += A[k * Arows + r].f * B[c * Acols + k].f; }
                    x->val[o[1]][c * Arows + r].f = sacc;
                }
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_TRANSPOSE: {                                 /* T[tc][tr] = M[tr][tc] (column-major) */
            unsigned tm = x->rtype[o[2]];
            int Mrows = m->ids[tm].rows, Mcols = m->ids[tm].cols;
            if (Mrows < 1 || Mcols < 1 || Mrows * Mcols > SPV_VAL_WORDS) { return -45; }
            const spv_w *M = value_of(x, o[2]);
            for (int tc = 0; tc < Mrows; tc++) {
                for (int tr = 0; tr < Mcols; tr++) { x->val[o[1]][tc * Mcols + tr].f = M[tr * Mrows + tc].f; }
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_OUTERPROD: {                                 /* V1 (n, Zeilen) x V2^T (mm, Spalten) */
            int n = width_of(x, o[2]), mm = width_of(x, o[3]);
            if (n < 1 || mm < 1 || n * mm > SPV_VAL_WORDS) { return -45; }
            const spv_w *V1 = value_of(x, o[2]);
            const spv_w *V2 = value_of(x, o[3]);
            for (int c = 0; c < mm; c++) {
                for (int r = 0; r < n; r++) { x->val[o[1]][c * n + r].f = V1[r].f * V2[c].f; }
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_IEQUAL: case OP_INOTEQUAL: case OP_UGT: case OP_SGT: case OP_UGE:
        case OP_SGE: case OP_ULT: case OP_SLT: case OP_ULE: case OP_SLE: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            const spv_w *b = value_of(x, o[3]);
            for (int i = 0; i < w; i++) {
                int r;
                switch (op) {
                case OP_IEQUAL:    r = (a[i].u == b[i].u); break;
                case OP_INOTEQUAL: r = (a[i].u != b[i].u); break;
                case OP_UGT: r = (a[i].u >  b[i].u); break;
                case OP_UGE: r = (a[i].u >= b[i].u); break;
                case OP_ULT: r = (a[i].u <  b[i].u); break;
                case OP_ULE: r = (a[i].u <= b[i].u); break;
                case OP_SGT: r = (a[i].i >  b[i].i); break;
                case OP_SGE: r = (a[i].i >= b[i].i); break;
                case OP_SLT: r = (a[i].i <  b[i].i); break;
                default:     r = (a[i].i <= b[i].i); break;
                }
                x->val[o[1]][i].u = (unsigned)r;
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_LOGICALEQ: case OP_LOGICALNEQ: case OP_LOGICALOR: case OP_LOGICALAND: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            const spv_w *b = value_of(x, o[3]);
            for (int i = 0; i < w; i++) {
                int av = (a[i].u != 0), bv = (b[i].u != 0), r;
                switch (op) {
                case OP_LOGICALEQ:  r = (av == bv); break;
                case OP_LOGICALNEQ: r = (av != bv); break;
                case OP_LOGICALOR:  r = (av || bv); break;
                default:            r = (av && bv); break;
                }
                x->val[o[1]][i].u = (unsigned)r;
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_LOGICALNOT: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            for (int i = 0; i < w; i++) { x->val[o[1]][i].u = (a[i].u == 0); }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_SHRLOG: case OP_SHRARITH: case OP_SHL:
        case OP_BITOR: case OP_BITXOR: case OP_BITAND: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            const spv_w *b = value_of(x, o[3]);
            for (int i = 0; i < w; i++) {
                unsigned sh = b[i].u & 31u;
                switch (op) {
                case OP_SHRLOG:   x->val[o[1]][i].u = a[i].u >> sh; break;
                case OP_SHRARITH: x->val[o[1]][i].i = a[i].i >> sh; break;
                case OP_SHL:      x->val[o[1]][i].u = a[i].u << sh; break;
                case OP_BITOR:    x->val[o[1]][i].u = a[i].u | b[i].u; break;
                case OP_BITXOR:   x->val[o[1]][i].u = a[i].u ^ b[i].u; break;
                default:          x->val[o[1]][i].u = a[i].u & b[i].u; break;
                }
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_NOT: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            for (int i = 0; i < w; i++) { x->val[o[1]][i].u = ~a[i].u; }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_BITREVERSE: {                          /* V2.6: Bits spiegeln (32-bit) */
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            for (int c = 0; c < w; c++) {
                unsigned v = a[c].u, r = 0;
                for (int k = 0; k < 32; k++) { r |= ((v >> k) & 1u) << (31 - k); }
                x->val[o[1]][c].u = r;
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_BITCOUNT: {                            /* V2.6: gesetzte Bits zaehlen (popcount) */
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            for (int c = 0; c < w; c++) {
                unsigned v = a[c].u, n = 0;
                while (v) { n += v & 1u; v >>= 1; }
                x->val[o[1]][c].u = n;
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_BITFIELDUEXTRACT: case OP_BITFIELDSEXTRACT: {   /* V2.6: (value>>off)&mask, S mit Vorzeichen */
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            unsigned off = value_of(x, o[3])[0].u & 31u;
            unsigned cnt = value_of(x, o[4])[0].u;
            if (cnt > 32) { cnt = 32; } if (off + cnt > 32) { cnt = 32 - off; }
            for (int c = 0; c < w; c++) {
                unsigned v = a[c].u;
                unsigned mask = (cnt == 0) ? 0u : (cnt >= 32 ? 0xFFFFFFFFu : ((1u << cnt) - 1u));
                unsigned res = (v >> off) & mask;
                if (op == OP_BITFIELDSEXTRACT && cnt > 0 && cnt < 32 && (res & (1u << (cnt - 1)))) {
                    res |= ~mask;                       /* Vorzeichen erweitern */
                }
                x->val[o[1]][c].u = res;
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_BITFIELDINSERT: {                      /* V2.6: base mit [off..off+cnt) = insert */
            int w = type_words(m, o[0]);
            const spv_w *base = value_of(x, o[2]);
            const spv_w *ins  = value_of(x, o[3]);
            unsigned off = value_of(x, o[4])[0].u & 31u;
            unsigned cnt = value_of(x, o[5])[0].u;
            if (cnt > 32) { cnt = 32; } if (off + cnt > 32) { cnt = 32 - off; }
            unsigned mask = (cnt == 0) ? 0u : (cnt >= 32 ? 0xFFFFFFFFu : (((1u << cnt) - 1u) << off));
            for (int c = 0; c < w; c++) {
                x->val[o[1]][c].u = (base[c].u & ~mask) | ((ins[c].u << off) & mask);
            }
            x->rtype[o[1]] = o[0]; break;
        }
        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
            int w = type_words(m, o[0]);
            const spv_w *a = value_of(x, o[2]);
            const spv_w *b = value_of(x, o[3]);
            for (int i = 0; i < w; i++) {
                float r;
                if (op == OP_FADD) { r = a[i].f + b[i].f; }
                else if (op == OP_FSUB) { r = a[i].f - b[i].f; }
                else if (op == OP_FMUL) { r = a[i].f * b[i].f; }
                else { r = a[i].f / b[i].f; }
                x->val[o[1]][i].f = r;
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_VECTIMESSCAL: {
            int w = type_words(m, o[0]);
            const spv_w *v = value_of(x, o[2]);
            float s = value_of(x, o[3])[0].f;
            for (int i = 0; i < w; i++) { x->val[o[1]][i].f = v[i].f * s; }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_MATTIMESVEC: {
            const spv_id_t *rt = &m->ids[o[0]];       /* Ergebnis: vecN */
            int rows = rt->rows;
            int cols = width_of(x, o[3]);             /* Vektorbreite = Spaltenzahl */
            /* SICHERHEIT: M ist ein val-Array (SPV_VAL_WORDS); der groesste Index ist
             * (cols-1)*rows+(rows-1) -> cols*rows <= SPV_VAL_WORDS erzwingen. */
            if (rows < 1 || cols < 1 || rows * cols > SPV_VAL_WORDS) { return -45; }
            const spv_w *M = value_of(x, o[2]);
            const spv_w *V = value_of(x, o[3]);
            for (int r = 0; r < rows; r++) {
                float s = 0.0f;
                for (int c = 0; c < cols; c++) { s += M[c * rows + r].f * V[c].f; }
                x->val[o[1]][r].f = s;
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_DOT: {
            int w = width_of(x, o[2]);                /* exakte Vektorbreite */
            const spv_w *a = value_of(x, o[2]);
            const spv_w *b = value_of(x, o[3]);
            float s = 0.0f;
            for (int i = 0; i < w; i++) { s += a[i].f * b[i].f; }
            x->val[o[1]][0].f = s;
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_FORDEQ: case OP_FORDNEQ: case OP_FORDLT:
        case OP_FORDGT: case OP_FORDLE: case OP_FORDGE: {
            float a = value_of(x, o[2])[0].f, b = value_of(x, o[3])[0].f;
            int r;
            switch (op) {
            case OP_FORDEQ:  r = (a == b); break;
            case OP_FORDNEQ: r = (a != b); break;
            case OP_FORDLT:  r = (a <  b); break;
            case OP_FORDGT:  r = (a >  b); break;
            case OP_FORDLE:  r = (a <= b); break;
            default:         r = (a >= b); break;
            }
            x->val[o[1]][0].u = (unsigned)r;
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_SELECT: {
            int w = type_words(m, o[0]);
            unsigned c = value_of(x, o[2])[0].u;
            const spv_w *a = value_of(x, o[3]);
            const spv_w *b = value_of(x, o[4]);
            for (int i = 0; i < w; i++) { x->val[o[1]][i] = c ? a[i] : b[i]; }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_IMAGESAMPLEIMPL: case OP_IMAGESAMPLEEXPL: {
            /* V1.4: o[2]=Combined-Image (Handle=Binding), o[3]=Koordinate (vec2). */
            if (wc < 5) { return -46; }
            unsigned binding = value_of(x, o[2])[0].u;
            const spv_w *co = value_of(x, o[3]);
            spv_w *r = x->val[o[1]];
            r[0].f = r[1].f = r[2].f = 0.0f; r[3].f = 1.0f;
            if (binding < SPV_MAX_DESCRIPTOR && x->io->tex[binding].pixels) {
                sample_tex(&x->io->tex[binding], co[0].f, co[1].f, r);
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_IMAGE: {                                   /* V2.6: Image aus Sampled-Image = Handle durchreichen */
            x->val[o[1]][0] = value_of(x, o[2])[0];
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_IMAGEFETCH: {                              /* V2.6: texelFetch(image, ivec2, lod) -- integer, ungefiltert */
            if (wc < 5) { return -46; }
            unsigned binding = value_of(x, o[2])[0].u;
            const spv_w *co = value_of(x, o[3]);           /* ivec2 (integer!) */
            spv_w *r = x->val[o[1]];
            r[0].f = r[1].f = r[2].f = 0.0f; r[3].f = 1.0f;
            if (binding < SPV_MAX_DESCRIPTOR && x->io->tex[binding].pixels) {
                const spv_tex_t *t = &x->io->tex[binding];
                int tx = co[0].i, ty = co[1].i;            /* gegen Bildgrenzen klemmen (robustBufferAccess) */
                if (tx < 0) { tx = 0; } if (t->w && tx >= (int)t->w) { tx = (int)t->w - 1; }
                if (ty < 0) { ty = 0; } if (t->h && ty >= (int)t->h) { ty = (int)t->h - 1; }
                float rgba[4]; texel_rgba(t->pixels, t->pitch_px, tx, ty, rgba);
                for (int i = 0; i < 4; i++) { r[i].f = rgba[i]; }
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_IMAGEREAD: {                               /* V2.6: imageLoad(image, ivec2) -> vec4 (integer) */
            if (wc < 5) { return -46; }
            unsigned binding = value_of(x, o[2])[0].u;
            const spv_w *co = value_of(x, o[3]);
            spv_w *r = x->val[o[1]];
            r[0].f = r[1].f = r[2].f = 0.0f; r[3].f = 1.0f;
            if (binding < SPV_MAX_DESCRIPTOR && x->io->tex[binding].pixels) {
                const spv_tex_t *t = &x->io->tex[binding];
                int tx = co[0].i, ty = co[1].i;
                if (tx < 0) { tx = 0; } if (t->w && tx >= (int)t->w) { tx = (int)t->w - 1; }
                if (ty < 0) { ty = 0; } if (t->h && ty >= (int)t->h) { ty = (int)t->h - 1; }
                float rgba[4]; texel_rgba(t->pixels, t->pitch_px, tx, ty, rgba);
                for (int i = 0; i < 4; i++) { r[i].f = rgba[i]; }
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_IMAGEWRITE: {                              /* V2.6: imageStore(image, ivec2, vec4) -- kein Ergebnis */
            if (wc < 4) { return -46; }
            unsigned binding = value_of(x, o[0])[0].u;
            const spv_w *co = value_of(x, o[1]);
            const spv_w *px = value_of(x, o[2]);
            if (binding < SPV_MAX_DESCRIPTOR && x->io->tex[binding].pixels) {
                const spv_tex_t *t = &x->io->tex[binding];
                int cx = co[0].i, cy = co[1].i;            /* nur in-bounds schreiben (kein OOB) */
                if (cx >= 0 && cy >= 0 && (unsigned)cx < t->w && (unsigned)cy < t->h) {
                    unsigned word = (f2b(px[3].f) << 24) | (f2b(px[0].f) << 16) |
                                    (f2b(px[1].f) << 8) | f2b(px[2].f);
                    ((unsigned *)(void *)t->pixels)[(unsigned)cy * t->pitch_px + (unsigned)cx] = word;
                }
            }
            break;
        }
        case OP_IMAGEGATHER: {                             /* V2.6: textureGather(sampler, coord, comp) -> vec4 */
            if (wc < 6) { return -46; }
            unsigned binding = value_of(x, o[2])[0].u;
            const spv_w *co = value_of(x, o[3]);
            int comp = value_of(x, o[4])[0].i;             /* Komponente 0..3 (Konstante) */
            spv_w *r = x->val[o[1]];
            r[0].f = r[1].f = r[2].f = r[3].f = 0.0f;
            if (binding < SPV_MAX_DESCRIPTOR && x->io->tex[binding].pixels) {
                gather_tex(&x->io->tex[binding], co[0].f, co[1].f, comp, r);
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_IMAGEQUERYSIZE:
        case OP_IMAGEQUERYSIZELOD: {                        /* V2.6: textureSize -> ivec2 {w,h} */
            unsigned binding = value_of(x, o[2])[0].u;
            spv_w *r = x->val[o[1]];
            r[0].i = r[1].i = 0;
            if (binding < SPV_MAX_DESCRIPTOR) {
                r[0].i = (int)x->io->tex[binding].w;
                r[1].i = (int)x->io->tex[binding].h;
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_EXTINST: {
            if (wc < 6) { return -35; }                /* op,type,rid,set,inst,arg1 */
            int w = type_words(m, o[0]);
            unsigned inst = o[3];
            const spv_w *a = value_of(x, o[4]);
            spv_w *r = x->val[o[1]];
            int need2 = (inst == G_FMIN || inst == G_FMAX || inst == G_CROSS || inst == G_FCLAMP ||
                         inst == G_FMIX || inst == G_POW || inst == G_ATAN2 || inst == G_STEP ||
                         inst == G_DISTANCE || inst == G_REFLECT || inst == G_UMIN || inst == G_SMIN ||
                         inst == G_UMAX || inst == G_SMAX || inst == G_UCLAMP || inst == G_SCLAMP ||
                         inst == G_SMOOTHSTEP || inst == G_FMA || inst == G_FACEFWD || inst == G_REFRACT);
            int need3 = (inst == G_FCLAMP || inst == G_FMIX || inst == G_UCLAMP || inst == G_SCLAMP ||
                         inst == G_SMOOTHSTEP || inst == G_FMA || inst == G_FACEFWD || inst == G_REFRACT);
            if ((need2 && wc < 7) || (need3 && wc < 8)) { return -35; }
            switch (inst) {
            case G_FABS:  for (int i = 0; i < w; i++) { r[i].f = a[i].f < 0 ? -a[i].f : a[i].f; } break;
            case G_SQRT:  for (int i = 0; i < w; i++) { r[i].f = __builtin_sqrtf(a[i].f); } break;
            case G_INVSQRT: for (int i = 0; i < w; i++) { r[i].f = 1.0f / __builtin_sqrtf(a[i].f); } break;
            case G_FMIN: {
                const spv_w *b = value_of(x, o[5]);
                for (int i = 0; i < w; i++) { r[i].f = a[i].f < b[i].f ? a[i].f : b[i].f; }
                break;
            }
            case G_FMAX: {
                const spv_w *b = value_of(x, o[5]);
                for (int i = 0; i < w; i++) { r[i].f = a[i].f > b[i].f ? a[i].f : b[i].f; }
                break;
            }
            case G_FCLAMP: {
                const spv_w *lo = value_of(x, o[5]);
                const spv_w *hi = value_of(x, o[6]);
                for (int i = 0; i < w; i++) {
                    float v = a[i].f;
                    if (v < lo[i].f) { v = lo[i].f; }
                    if (v > hi[i].f) { v = hi[i].f; }
                    r[i].f = v;
                }
                break;
            }
            case G_FMIX: {
                const spv_w *b = value_of(x, o[5]);
                const spv_w *t = value_of(x, o[6]);
                for (int i = 0; i < w; i++) { r[i].f = a[i].f + (b[i].f - a[i].f) * t[i].f; }
                break;
            }
            case G_LENGTH: {
                int aw = width_of(x, o[4]);
                float s = 0.0f;
                for (int i = 0; i < aw; i++) { s += a[i].f * a[i].f; }
                r[0].f = __builtin_sqrtf(s);
                break;
            }
            case G_CROSS: {
                const spv_w *b = value_of(x, o[5]);
                float rx = a[1].f * b[2].f - a[2].f * b[1].f;
                float ry = a[2].f * b[0].f - a[0].f * b[2].f;
                float rz = a[0].f * b[1].f - a[1].f * b[0].f;
                r[0].f = rx; r[1].f = ry; r[2].f = rz;
                break;
            }
            case G_NORMALIZE: {
                float s = 0.0f;
                for (int i = 0; i < w; i++) { s += a[i].f * a[i].f; }
                float inv = 1.0f / __builtin_sqrtf(s);
                for (int i = 0; i < w; i++) { r[i].f = a[i].f * inv; }
                break;
            }
            /* --- V2.4: 1-Argument, komponentenweise --- */
            case G_FLOOR: for (int i = 0; i < w; i++) { r[i].f = spvf_floor(a[i].f); } break;
            case G_CEIL:  for (int i = 0; i < w; i++) { r[i].f = -spvf_floor(-a[i].f); } break;
            case G_TRUNC: for (int i = 0; i < w; i++) { r[i].f = spvf_trunc(a[i].f); } break;
            case G_FRACT: for (int i = 0; i < w; i++) { r[i].f = a[i].f - spvf_floor(a[i].f); } break;
            case G_ROUND: for (int i = 0; i < w; i++) { r[i].f = spvf_floor(a[i].f + 0.5f); } break;
            case G_ROUNDEVEN: for (int i = 0; i < w; i++) {
                                  float fl = spvf_floor(a[i].f), fr = a[i].f - fl;
                                  r[i].f = (fr < 0.5f) ? fl : (fr > 0.5f) ? fl + 1.0f
                                          : (((long)fl & 1L) ? fl + 1.0f : fl); } break;
            case G_SABS:  for (int i = 0; i < w; i++) { r[i].i = a[i].i < 0 ? -a[i].i : a[i].i; } break;
            case G_FSIGN: for (int i = 0; i < w; i++) { r[i].f = (a[i].f > 0.0f) - (a[i].f < 0.0f); } break;
            case G_SSIGN: for (int i = 0; i < w; i++) { r[i].i = (a[i].i > 0) - (a[i].i < 0); } break;
            case G_RADIANS: for (int i = 0; i < w; i++) { r[i].f = a[i].f * 0.017453293f; } break;
            case G_DEGREES: for (int i = 0; i < w; i++) { r[i].f = a[i].f * 57.29578f; } break;
            case G_SIN:  for (int i = 0; i < w; i++) { r[i].f = spvf_sin(a[i].f); } break;
            case G_COS:  for (int i = 0; i < w; i++) { r[i].f = spvf_cos(a[i].f); } break;
            case G_TAN:  for (int i = 0; i < w; i++) { float c = spvf_cos(a[i].f); r[i].f = c != 0.0f ? spvf_sin(a[i].f) / c : 0.0f; } break;
            case G_ASIN: for (int i = 0; i < w; i++) { r[i].f = spvf_asin(a[i].f); } break;
            case G_ACOS: for (int i = 0; i < w; i++) { r[i].f = 1.57079633f - spvf_asin(a[i].f); } break;
            case G_ATAN: for (int i = 0; i < w; i++) { r[i].f = spvf_atan(a[i].f); } break;
            case G_EXP:  for (int i = 0; i < w; i++) { r[i].f = spvf_exp(a[i].f); } break;
            case G_LOG:  for (int i = 0; i < w; i++) { r[i].f = spvf_log(a[i].f); } break;
            case G_EXP2: for (int i = 0; i < w; i++) { r[i].f = spvf_exp(a[i].f * 0.69314718f); } break;
            case G_LOG2: for (int i = 0; i < w; i++) { r[i].f = spvf_log(a[i].f) * 1.44269504f; } break;
            /* --- V2.4: 2-Argument --- */
            case G_POW:   { const spv_w *b = value_of(x, o[5]); for (int i = 0; i < w; i++) { r[i].f = spvf_pow(a[i].f, b[i].f); } break; }
            case G_ATAN2: { const spv_w *b = value_of(x, o[5]); for (int i = 0; i < w; i++) { r[i].f = spvf_atan2(a[i].f, b[i].f); } break; }
            case G_STEP:  { const spv_w *b = value_of(x, o[5]); for (int i = 0; i < w; i++) { r[i].f = (b[i].f < a[i].f) ? 0.0f : 1.0f; } break; }
            case G_UMIN:  { const spv_w *b = value_of(x, o[5]); for (int i = 0; i < w; i++) { r[i].u = a[i].u < b[i].u ? a[i].u : b[i].u; } break; }
            case G_SMIN:  { const spv_w *b = value_of(x, o[5]); for (int i = 0; i < w; i++) { r[i].i = a[i].i < b[i].i ? a[i].i : b[i].i; } break; }
            case G_UMAX:  { const spv_w *b = value_of(x, o[5]); for (int i = 0; i < w; i++) { r[i].u = a[i].u > b[i].u ? a[i].u : b[i].u; } break; }
            case G_SMAX:  { const spv_w *b = value_of(x, o[5]); for (int i = 0; i < w; i++) { r[i].i = a[i].i > b[i].i ? a[i].i : b[i].i; } break; }
            case G_DISTANCE: { int aw = width_of(x, o[4]); const spv_w *b = value_of(x, o[5]);
                               float s = 0.0f; for (int i = 0; i < aw; i++) { float d = a[i].f - b[i].f; s += d * d; }
                               r[0].f = __builtin_sqrtf(s); break; }
            case G_REFLECT: { const spv_w *n = value_of(x, o[5]);   /* I - 2*dot(N,I)*N */
                              float d = 0.0f; for (int i = 0; i < w; i++) { d += n[i].f * a[i].f; }
                              for (int i = 0; i < w; i++) { r[i].f = a[i].f - 2.0f * d * n[i].f; } break; }
            /* --- V2.4: 3-Argument --- */
            case G_UCLAMP: { const spv_w *lo = value_of(x, o[5]), *hi = value_of(x, o[6]);
                             for (int i = 0; i < w; i++) { unsigned v = a[i].u; if (v < lo[i].u) v = lo[i].u; if (v > hi[i].u) v = hi[i].u; r[i].u = v; } break; }
            case G_SCLAMP: { const spv_w *lo = value_of(x, o[5]), *hi = value_of(x, o[6]);
                             for (int i = 0; i < w; i++) { int v = a[i].i; if (v < lo[i].i) v = lo[i].i; if (v > hi[i].i) v = hi[i].i; r[i].i = v; } break; }
            case G_FMA:    { const spv_w *b = value_of(x, o[5]), *c = value_of(x, o[6]);
                             for (int i = 0; i < w; i++) { r[i].f = a[i].f * b[i].f + c[i].f; } break; }
            case G_SMOOTHSTEP: { const spv_w *e0 = value_of(x, o[5]), *e1 = value_of(x, o[6]);   /* a=x, e0,e1 */
                             for (int i = 0; i < w; i++) {
                                 float d = e1[i].f - e0[i].f;
                                 float t = d != 0.0f ? (a[i].f - e0[i].f) / d : 0.0f;
                                 if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
                                 r[i].f = t * t * (3.0f - 2.0f * t);
                             } break; }
            case G_FACEFWD: { const spv_w *I = value_of(x, o[5]), *Nref = value_of(x, o[6]);   /* N=a */
                             float d = 0.0f; for (int i = 0; i < w; i++) { d += Nref[i].f * I[i].f; }
                             for (int i = 0; i < w; i++) { r[i].f = (d < 0.0f) ? a[i].f : -a[i].f; } break; }
            case G_REFRACT: { const spv_w *N = value_of(x, o[5]); float eta = value_of(x, o[6])[0].f;   /* I=a */
                             float d = 0.0f; for (int i = 0; i < w; i++) { d += N[i].f * a[i].f; }
                             float kk = 1.0f - eta * eta * (1.0f - d * d);
                             if (kk < 0.0f) { for (int i = 0; i < w; i++) { r[i].f = 0.0f; } }
                             else { float sq = __builtin_sqrtf(kk);
                                    for (int i = 0; i < w; i++) { r[i].f = eta * a[i].f - (eta * d + sq) * N[i].f; } } break; }
            default:
                return -30;                            /* unbekannte ExtInst: fail-loud */
            }
            x->rtype[o[1]] = o[0];
            break;
        }
        case OP_SELECTMERGE:
        case OP_LOOPMERGE:
            break;                                     /* strukturell -> ignorieren */
        case OP_BRANCH: {
            unsigned lbl = o[0];
            if (lbl >= m->bound || m->ids[lbl].kind != 6) { return -31; }
            prev_label = cur_label;
            pc = m->ids[lbl].word_off;
            continue;
        }
        case OP_BRANCHCOND: {
            unsigned c = value_of(x, o[0])[0].u;
            unsigned lbl = c ? o[1] : o[2];
            if (lbl >= m->bound || m->ids[lbl].kind != 6) { return -31; }
            prev_label = cur_label;
            pc = m->ids[lbl].word_off;
            continue;
        }
        case OP_SWITCH: {                                  /* V2.3: <sel> <default> (literal,label)* */
            if (wc < 3) { return -31; }
            unsigned sel = value_of(x, o[0])[0].u;
            unsigned target = o[1];                        /* Default */
            for (unsigned i = 2; i + 1 <= wc - 2; i += 2) {   /* Paare o[2..wc-2] (32-bit-Literale) */
                if (o[i] == sel) { target = o[i + 1]; break; }
            }
            if (target >= m->bound || m->ids[target].kind != 6) { return -31; }
            prev_label = cur_label;
            pc = m->ids[target].word_off;
            continue;
        }
        case OP_FUNCTIONCALL: {                           /* V2.3: Inlining via Call-Stack */
            if (wc < 4 || !IDOK(x, o[0]) || !IDOK(x, o[1]) || !IDOK(x, o[2])) { return -40; }
            unsigned callee = o[2];
            if (m->ids[callee].kind != 4) { return -46; }    /* keine Funktion */
            unsigned cpc = m->ids[callee].word_off;          /* OpFunction */
            cpc += (W[cpc] >> 16);                            /* hinter OpFunction */
            unsigned argi = 3;                                /* Argumente in o[3..wc-2] */
            while (cpc < m->nwords && (W[cpc] & 0xFFFFu) == OP_FUNCTIONPARAM && argi + 1 < wc) {
                unsigned pwc = W[cpc] >> 16;
                if (pwc < 3 || cpc + pwc > m->nwords) { return -22; }
                const unsigned *po = &W[cpc + 1];             /* po[0]=Typ, po[1]=Param-Id */
                unsigned pid = po[1], arg = o[argi];
                if (!IDOK(x, pid) || !IDOK(x, arg)) { return -40; }
                if (x->pbase[arg]) {                          /* Zeiger-Parameter -> Pointer-Zustand kopieren */
                    x->pbase[pid] = x->pbase[arg]; x->poff[pid] = x->poff[arg];
                    x->pcap[pid] = x->pcap[arg]; x->ptype[pid] = x->ptype[arg];
                } else {                                      /* Wert-Parameter */
                    const spv_w *src = value_of(x, arg);
                    for (int j = 0; j < SPV_VAL_WORDS; j++) { x->val[pid][j] = src[j]; }
                    x->rtype[pid] = x->rtype[arg] ? x->rtype[arg] : po[0];
                }
                cpc += pwc; argi++;
            }
            if (x->callsp >= 16) { return -47; }             /* Tiefen-/Rekursionslimit */
            x->callstk[x->callsp].pc  = pc + wc;             /* Ruecksprung hinter den Call */
            x->callstk[x->callsp].rid = o[1];
            x->callsp++;
            pc = cpc;                                        /* erster Block des Callee (hinter Params) */
            continue;
        }
        case OP_KILL:                                     /* V2.3: Fragment verwerfen */
        case OP_TERMINATE:
            x->io->discarded = 1;
            return 0;
        case OP_RETURN:
        case OP_UNREACHABLE:
            if (x->callsp > 0) { x->callsp--; pc = x->callstk[x->callsp].pc; continue; }
            return 0;
        case OP_RETURNVALUE:                              /* V2.3: Rueckgabewert in die Ergebnis-Id des Calls */
            if (x->callsp > 0) {
                x->callsp--;
                unsigned rid = x->callstk[x->callsp].rid;
                const spv_w *rv = value_of(x, o[0]);
                for (int j = 0; j < SPV_VAL_WORDS; j++) { x->val[rid][j] = rv[j]; }
                x->rtype[rid] = x->rtype[o[0]];
                pc = x->callstk[x->callsp].pc;
                continue;
            }
            return 0;
        case OP_FUNCTIONEND:
            if (x->callsp > 0) { x->callsp--; pc = x->callstk[x->callsp].pc; continue; }
            return 0;
        default:
            return -32;                                /* unbekannte Op: fail-loud */
        }
        pc += wc;
    }
    return -33;
}
