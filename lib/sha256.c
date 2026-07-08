/*
 * lib/sha256.c  --  SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256, SHA-CSPRNG
 *
 * Standardimplementierung (FIPS 180-4 / RFC 2104 / RFC 8018). Selbsttest gegen
 * bekannte Vektoren in crypto_selftest().
 */
#include <stdint.h>
#include "crypto.h"
#include "kmem.h"
#include "aarch64.h"

typedef struct {
    uint32_t h[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t idx;
} sha_ctx;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* Nicht wegoptimierbares Nullen sensibler Puffer (volatile). */
static void secure_zero(void *p, uint32_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}

static void sha_block(sha_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha_init(sha_ctx *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(c->h, iv, sizeof(iv));
    c->bitlen = 0;
    c->idx = 0;
}

static void sha_update(sha_ctx *c, const uint8_t *p, uint32_t n)
{
    c->bitlen += (uint64_t)n * 8;

    /* Teilpuffer auffuellen. */
    if (c->idx) {
        while (n && c->idx < 64) {
            c->buf[c->idx++] = *p++;
            n--;
        }
        if (c->idx == 64) {
            sha_block(c, c->buf);
            c->idx = 0;
        }
    }
    /* Volle Bloecke direkt aus der Eingabe (kein byteweises Puffern). */
    while (n >= 64) {
        sha_block(c, p);
        p += 64;
        n -= 64;
    }
    /* Rest puffern. */
    while (n) {
        c->buf[c->idx++] = *p++;
        n--;
    }
}

static void sha_final(sha_ctx *c, uint8_t out[32])
{
    uint64_t bl = c->bitlen;           /* Laenge VOR dem Padding sichern */
    uint8_t one = 0x80, zero = 0;
    sha_update(c, &one, 1);
    while (c->idx != 56) {
        sha_update(c, &zero, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bl >> (56 - 8 * i));
    }
    sha_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

void sha256(const void *data, uint32_t len, uint8_t out[32])
{
    sha_ctx c;
    sha_init(&c);
    sha_update(&c, (const uint8_t *)data, len);
    sha_final(&c, out);
}

void hmac_sha256(const uint8_t *key, uint32_t keylen,
                 const uint8_t *msg, uint32_t msglen, uint8_t out[32])
{
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (keylen > 64) {
        sha256(key, keylen, k);        /* lange Schluessel zuerst hashen */
    } else {
        memcpy(k, key, keylen);
    }
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) {
        ip[i] = k[i] ^ 0x36;
        op[i] = k[i] ^ 0x5c;
    }
    uint8_t inner[32];
    sha_ctx c;
    sha_init(&c);
    sha_update(&c, ip, 64);
    sha_update(&c, msg, msglen);
    sha_final(&c, inner);

    sha_init(&c);
    sha_update(&c, op, 64);
    sha_update(&c, inner, 32);
    sha_final(&c, out);

    secure_zero(k, sizeof(k));
    secure_zero(ip, sizeof(ip));
    secure_zero(op, sizeof(op));
    secure_zero(inner, sizeof(inner));
}

void pbkdf2_sha256(const uint8_t *pw, uint32_t pwlen,
                   const uint8_t *salt, uint32_t saltlen,
                   uint32_t iters, uint8_t *out, uint32_t outlen)
{
    if (saltlen > 64 || iters == 0) {
        return;
    }
    uint32_t blocks = (outlen + 31) / 32;
    uint8_t sb[68];                    /* salt || INT32BE(block) */
    uint8_t U[32], T[32];

    for (uint32_t b = 1; b <= blocks; b++) {
        memcpy(sb, salt, saltlen);
        sb[saltlen]     = (uint8_t)(b >> 24);
        sb[saltlen + 1] = (uint8_t)(b >> 16);
        sb[saltlen + 2] = (uint8_t)(b >> 8);
        sb[saltlen + 3] = (uint8_t)b;

        hmac_sha256(pw, pwlen, sb, saltlen + 4, U);
        memcpy(T, U, 32);
        for (uint32_t j = 1; j < iters; j++) {
            hmac_sha256(pw, pwlen, U, 32, U);
            for (int i = 0; i < 32; i++) {
                T[i] ^= U[i];
            }
        }
        uint32_t off = (b - 1) * 32;
        uint32_t n = (outlen - off < 32) ? (outlen - off) : 32;
        memcpy(out + off, T, n);
    }
    secure_zero(U, sizeof(U));
    secure_zero(T, sizeof(T));
    secure_zero(sb, sizeof(sb));
}

/* --- SHA-CSPRNG (timer-geseedet) --- */
static uint8_t  s_state[32];
static uint64_t s_ctr;
static int      s_seeded;

static void rng_seed(void)
{
    uint64_t s[8];
    for (int i = 0; i < 8; i++) {
        s[i] = READ_SYSREG(cntpct_el0) + (uint64_t)i * 0x9E3779B97F4A7C15ull;
    }
    sha256(s, sizeof(s), s_state);
    s_seeded = 1;
}

void rng_fill(uint8_t *buf, uint32_t len)
{
    if (!s_seeded) {
        rng_seed();
    }
    while (len) {
        uint8_t blk[40];
        memcpy(blk, s_state, 32);
        uint64_t c = (++s_ctr) ^ READ_SYSREG(cntpct_el0);
        memcpy(blk + 32, &c, 8);
        uint8_t outb[32];
        sha256(blk, sizeof(blk), outb);

        uint32_t n = (len < 32) ? len : 32;
        memcpy(buf, outb, n);
        buf += n;
        len -= n;

        sha256(s_state, 32, s_state);   /* State weiterdrehen */
        secure_zero(blk, sizeof(blk));  /* s_state NICHT loeschen (persistent) */
        secure_zero(outb, sizeof(outb));
    }
}

int crypto_selftest(void)
{
    static const uint8_t sha_abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    static const uint8_t pbkdf_vec[32] = {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c, 0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48, 0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b,
    };

    uint8_t h[32];
    sha256("abc", 3, h);
    if (memcmp(h, sha_abc, 32) != 0) {
        return -1;
    }

    uint8_t dk[32];
    pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, dk, 32);
    if (memcmp(dk, pbkdf_vec, 32) != 0) {
        return -1;
    }
    return 0;
}
