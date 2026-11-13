#ifndef NB_HASH_H
#define NB_HASH_H

#include "nb/base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NB_SHA256_DIGEST_SIZE 32u
#define NB_SHA256_BLOCK_SIZE  64u

/*
 * Standard hash primitives:
 *   - CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320)
 *   - SHA-256 (FIPS 180-4)
 *   - HMAC-SHA-256 (RFC 2104, test vectors in RFC 4231)
 *
 * These are low-level primitives, not password storage functions. Use
 * Argon2, bcrypt or scrypt for passwords; MD5 and SHA-1 are intentionally not
 * provided because they are no longer collision resistant.
 */

/* Computes CRC-32 of data. Empty input returns 0. */
NB_EXPORT uint32_t nb_crc32(const void *data, size_t len);

typedef struct nb_sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    unsigned char block[NB_SHA256_BLOCK_SIZE];
    size_t block_len;
} nb_sha256_ctx;

NB_EXPORT void nb_sha256_init(nb_sha256_ctx *ctx);
NB_EXPORT void nb_sha256_update(nb_sha256_ctx *ctx, const void *data, size_t len);
NB_EXPORT void nb_sha256_final(nb_sha256_ctx *ctx,
                               unsigned char out[NB_SHA256_DIGEST_SIZE]);
NB_EXPORT void nb_sha256(const void *data, size_t len,
                         unsigned char out[NB_SHA256_DIGEST_SIZE]);

/* HMAC-SHA-256. Keys longer than the 64-byte block size are hashed first.
 * out must provide NB_SHA256_DIGEST_SIZE bytes. */
NB_EXPORT void nb_hmac_sha256(const void *key, size_t key_len,
                              const void *data, size_t len,
                              unsigned char out[NB_SHA256_DIGEST_SIZE]);

/* Constant-time comparison of len bytes. Returns true when equal. */
NB_EXPORT bool nb_hash_equal(const void *a, const void *b, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NB_HASH_H */
