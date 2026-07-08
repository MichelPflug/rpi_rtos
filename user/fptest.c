/*
 * user/fptest.c  --  FPTEST.ELF: EL0-Guardian fuer die FP/SIMD-Kontextsicherung
 *
 * Diese Datei wird MIT FP kompiliert (ohne -mgeneral-regs-only, wie gui_ttf.c).
 */
#include "abi.h"
#include "ulib.h"

#define ROUNDS   40
#define SLEEP_MS 15

/* --- Ausgabe-Helfer (eine Zeile je SYS_WRITE -> cross-core atomar) --- */
static char lbuf[160];
static int  lpos;
static void lflush(void) { if (lpos > 0) { sys3(SYS_WRITE, 1, (long)lbuf, lpos); lpos = 0; } }
static void uwrite(const char *s)
{
    for (; *s; ++s) { if (lpos < (int)sizeof(lbuf)) { lbuf[lpos++] = *s; } if (*s == '\n') { lflush(); } }
}
static void fmt_u(char *buf, unsigned long v)
{
    char tmp[24]; int i = 0, p = 0;
    if (v == 0) { tmp[i++] = '0'; }
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i > 0) { buf[p++] = tmp[--i]; }
    buf[p] = 0;
}

/* --- alle 32 V-Register als 512-B-Block speichern/laden --- */
static void fp_store_all(unsigned char *dst)
{
    __asm__ volatile(
        "stp q0,  q1,  [%0, #16*0]\n\t"
        "stp q2,  q3,  [%0, #16*2]\n\t"
        "stp q4,  q5,  [%0, #16*4]\n\t"
        "stp q6,  q7,  [%0, #16*6]\n\t"
        "stp q8,  q9,  [%0, #16*8]\n\t"
        "stp q10, q11, [%0, #16*10]\n\t"
        "stp q12, q13, [%0, #16*12]\n\t"
        "stp q14, q15, [%0, #16*14]\n\t"
        "stp q16, q17, [%0, #16*16]\n\t"
        "stp q18, q19, [%0, #16*18]\n\t"
        "stp q20, q21, [%0, #16*20]\n\t"
        "stp q22, q23, [%0, #16*22]\n\t"
        "stp q24, q25, [%0, #16*24]\n\t"
        "stp q26, q27, [%0, #16*26]\n\t"
        "stp q28, q29, [%0, #16*28]\n\t"
        "stp q30, q31, [%0, #16*30]\n\t"
        :: "r"(dst) : "memory");
}
static void fp_load_all(const unsigned char *src)
{
    __asm__ volatile(
        "ldp q0,  q1,  [%0, #16*0]\n\t"
        "ldp q2,  q3,  [%0, #16*2]\n\t"
        "ldp q4,  q5,  [%0, #16*4]\n\t"
        "ldp q6,  q7,  [%0, #16*6]\n\t"
        "ldp q8,  q9,  [%0, #16*8]\n\t"
        "ldp q10, q11, [%0, #16*10]\n\t"
        "ldp q12, q13, [%0, #16*12]\n\t"
        "ldp q14, q15, [%0, #16*14]\n\t"
        "ldp q16, q17, [%0, #16*16]\n\t"
        "ldp q18, q19, [%0, #16*18]\n\t"
        "ldp q20, q21, [%0, #16*20]\n\t"
        "ldp q22, q23, [%0, #16*22]\n\t"
        "ldp q24, q25, [%0, #16*24]\n\t"
        "ldp q26, q27, [%0, #16*26]\n\t"
        "ldp q28, q29, [%0, #16*28]\n\t"
        "ldp q30, q31, [%0, #16*30]\n\t"
        :: "r"(src)
        : "memory",
          "v0","v1","v2","v3","v4","v5","v6","v7",
          "v8","v9","v10","v11","v12","v13","v14","v15",
          "v16","v17","v18","v19","v20","v21","v22","v23",
          "v24","v25","v26","v27","v28","v29","v30","v31");
}
static unsigned long fpcr_get(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}
static void fpcr_set(unsigned long v)
{
    __asm__ volatile("msr fpcr, %0" :: "r"(v));
}
static unsigned long fpsr_get(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, fpsr" : "=r"(v));
    return v;
}
static void fpsr_set(unsigned long v)
{
    __asm__ volatile("msr fpsr, %0" :: "r"(v));
}

static unsigned char g_pat[512];
static unsigned char g_chk[512];

/* pid-abgeleitetes, rundenvariiertes Muster (deterministisch rekonstruierbar). */
static void make_pattern(unsigned long pid, unsigned round)
{
    unsigned x = (unsigned)pid * 0x9E3779B9u + round * 0x85EBCA6Bu + 1u;
    for (int i = 0; i < 512; i++) {
        x = x * 1664525u + 1013904223u;              /* LCG */
        g_pat[i] = (unsigned char)(x >> 24);
    }
}

void _start(void)
{
    char nb[24];

    /* (1) Start-Zustand: V-Register + FPCR muessen exakt 0 sein (genullter fpctx).
     * Die beiden Instanzen haben AUFEINANDERFOLGENDE PIDs (kmain spawnt sie direkt
     * nacheinander) -> genau eine ist ungerade. Die UNGERADE probt erst nach 60 ms
     * Schlaf: bis dahin hat der Zwilling seine Muster in die physischen Register
     * geladen -- OHNE fpctx_restore saehe die Probe hier dessen Werte (Leak),
     * MIT Restore den eigenen (genullten) Kontext. Das macht die Leak-Probe
     * deterministisch (die Sofort-Probe des Zwillings prueft die Zero-Init).
     * Zwischen getpid/sleep und der Probe laeuft KEIN Code, der V-Register
     * beruehren koennte (nur Syscall-Wrapper). */
    unsigned long pid = (unsigned long)sys0(SYS_GETPID);
    if (pid & 1ul) { sys1(SYS_SLEEP_MS, 60); }
    fp_store_all(g_chk);
    unsigned long fpcr0 = fpcr_get();
    unsigned long fpsr0 = fpsr_get();
    int clean = (fpcr0 == 0) && (fpsr0 == 0);
    for (int i = 0; i < 512; i++) { if (g_chk[i] != 0) { clean = 0; break; } }

    uwrite("[fptest] pid=");
    fmt_u(nb, pid); uwrite(nb);
    uwrite(clean ? " start-zustand: sauber (V-Register+FPCR+FPSR genullt, kein Leak)\n"
                 : " start-zustand: VERSCHMUTZT (fremde FP-Werte sichtbar!)\n");

    /* (2) Hammer: eigenes FPCR-RMode (pid-abhaengig RP=0b01 / RM=0b10) + eigene STICKY
     * FPSR-Flags (IOC bzw. DZC -- werden von reinen ldp/stp-Transfers NIE veraendert)
     * + V-Muster ueber Schlaf-Praeemptionen hinweg -- der Zwilling auf demselben Kern
     * klobbert derweil die physischen Register mit SEINEN Werten. */
    unsigned long my_rmode = ((pid & 1ul) ? 1ul : 2ul) << 22;   /* FPCR.RMode */
    unsigned long my_fpsr  = (pid & 1ul) ? 0x01ul : 0x02ul;     /* FPSR.IOC bzw. .DZC */
    int bad_rounds = 0;
    for (unsigned r = 0; r < ROUNDS; r++) {
        make_pattern(pid, r);
        fpcr_set(my_rmode);
        fp_load_all(g_pat);
        fpsr_set(my_fpsr);
        sys1(SYS_SLEEP_MS, SLEEP_MS);                 /* Praeemption / Zwilling laeuft */
        fp_store_all(g_chk);
        int ok = (fpcr_get() == my_rmode) && (fpsr_get() == my_fpsr);
        if (ok) {
            for (int i = 0; i < 512; i++) { if (g_chk[i] != g_pat[i]) { ok = 0; break; } }
        }
        if (!ok) { bad_rounds++; }
    }

    uwrite("[fptest] pid=");
    fmt_u(nb, pid); uwrite(nb);
    if (bad_rounds == 0) {
        uwrite(" v-register+fpcr+fpsr ueber ");
        fmt_u(nb, ROUNDS); uwrite(nb);
        uwrite(" schlaf-praeemptionen: ok\n");
    } else {
        uwrite(" v-register+fpcr+fpsr: FEHLER (");
        fmt_u(nb, (unsigned long)bad_rounds); uwrite(nb);
        uwrite(" von ");
        fmt_u(nb, ROUNDS); uwrite(nb);
        uwrite(" runden korrupt)\n");
    }

    sys3(SYS_EXIT, bad_rounds == 0 && clean ? 0 : 1, 0, 0);
    for (;;) { }
}
