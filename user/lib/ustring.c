/*
 * user/lib/ustring.c  --  memset/memcpy/memmove fuer EL0-Apps (freestanding).
 *
 * Clang darf in freestanding Code Aufrufe von memset/memcpy/memmove SYNTHETISIEREN
 * (grosse Struct-Kopien, {0}-Initialisierer). Diese Referenz-Implementierungen
 * (byteweise, integer-only) machen solche Apps linkbar. Bewusst simpel: Korrektheit
 * vor Geschwindigkeit; heisse Pfade kopieren ohnehin wortweise selbst.
 */
typedef unsigned long usz_t;

void *memset(void *dst, int c, usz_t n)
{
    unsigned char *d = (unsigned char *)dst;
    for (usz_t i = 0; i < n; i++) { d[i] = (unsigned char)c; }
    return dst;
}

void *memcpy(void *dst, const void *src, usz_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (usz_t i = 0; i < n; i++) { d[i] = s[i]; }
    return dst;
}

void *memmove(void *dst, const void *src, usz_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (usz_t i = 0; i < n; i++) { d[i] = s[i]; }
    } else {
        for (usz_t i = n; i > 0; i--) { d[i - 1] = s[i - 1]; }
    }
    return dst;
}
