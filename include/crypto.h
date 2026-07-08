/*
 * include/crypto.h  --  SHA-256 / HMAC-SHA256 / PBKDF2 + Entropie
 *
 * Fuer die Benutzerverwaltung (Passwort-Hashing). Klartext wird nie gespeichert.
 */
#ifndef RPI_RTOS_CRYPTO_H
#define RPI_RTOS_CRYPTO_H

#include <stdint.h>

void sha256(const void *data, uint32_t len, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, uint32_t keylen,
                 const uint8_t *msg, uint32_t msglen, uint8_t out[32]);
void pbkdf2_sha256(const uint8_t *pw, uint32_t pwlen,
                   const uint8_t *salt, uint32_t saltlen,
                   uint32_t iters, uint8_t *out, uint32_t outlen);

/* Fuellt buf mit Zufallsbytes (CNTPCT-geseedeter SHA-256-CSPRNG).
 * >>HW<< : Auf echter Pi-4 zusaetzlich aus dem RNG200 (0xFE104000) nachseeden
 * (in QEMU nicht emuliert -> External-Abort). */
void rng_fill(uint8_t *buf, uint32_t len);

/* Selbsttest gegen bekannte Vektoren (SHA-256, PBKDF2). 0 = ok. */
int crypto_selftest(void);

#endif /* RPI_RTOS_CRYPTO_H */
