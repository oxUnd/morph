#include "nb/hash.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* CRC-32                                                                    */

#include "crc32_table.h"

uint32_t nb_crc32(const void *data, size_t len)
{
    if (!data && len != 0) {
        return 0;
    }
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    /* Explicit byte order works on either endian and unaligned views.
     * The tables are immutable, so there is no lazy-initialization race. */
    while (len >= 8u) {
        uint32_t first = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        first ^= crc;
        crc = crc32_table[7][first & 0xFFu] ^
              crc32_table[6][(first >> 8) & 0xFFu] ^
              crc32_table[5][(first >> 16) & 0xFFu] ^
              crc32_table[4][first >> 24] ^
              crc32_table[3][p[4]] ^ crc32_table[2][p[5]] ^
              crc32_table[1][p[6]] ^ crc32_table[0][p[7]];
        p += 8u;
        len -= 8u;
    }
    while (len != 0) {
        crc = (crc >> 8) ^ crc32_table[0][(crc ^ *p++) & 0xFFu];
        len--;
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------------- */
/* SHA-256                                                                   */

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void store_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void sha256_compress(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        w[i] = load_be32(block + i * 4u);
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^
                      rotr32(w[i - 15u], 18u) ^
                      (w[i - 15u] >> 3u);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^
                      rotr32(w[i - 2u], 19u) ^
                      (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];

    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void nb_sha256_init(nb_sha256_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bitlen = 0;
    ctx->block_len = 0;
    memset(ctx->block, 0, sizeof(ctx->block));
}

void nb_sha256_update(nb_sha256_ctx *ctx, const void *data, size_t len)
{
    if (!ctx || (!data && len != 0)) {
        return;
    }
    const unsigned char *p = (const unsigned char *)data;
    ctx->bitlen += (uint64_t)len * 8u;
    while (len > 0) {
        size_t space = NB_SHA256_BLOCK_SIZE - ctx->block_len;
        size_t take = len < space ? len : space;
        memcpy(ctx->block + ctx->block_len, p, take);
        ctx->block_len += take;
        p += take;
        len -= take;
        if (ctx->block_len == NB_SHA256_BLOCK_SIZE) {
            sha256_compress(ctx->state, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void nb_sha256_final(nb_sha256_ctx *ctx,
                     unsigned char out[NB_SHA256_DIGEST_SIZE])
{
    if (!ctx || !out) {
        return;
    }
    uint64_t bitlen = ctx->bitlen;
    size_t pos = ctx->block_len;
    ctx->block[pos++] = 0x80u;
    if (pos > 56u) {
        if (pos < NB_SHA256_BLOCK_SIZE) {
            memset(ctx->block + pos, 0, NB_SHA256_BLOCK_SIZE - pos);
        }
        sha256_compress(ctx->state, ctx->block);
        pos = 0;
    }
    memset(ctx->block + pos, 0, 56u - pos);
    for (size_t i = 0; i < 8u; i++) {
        ctx->block[56u + i] = (unsigned char)(bitlen >> (56u - i * 8u));
    }
    sha256_compress(ctx->state, ctx->block);

    for (size_t i = 0; i < 8u; i++) {
        store_be32(out + i * 4u, ctx->state[i]);
    }
    memset(ctx, 0, sizeof(*ctx));
}

void nb_sha256(const void *data, size_t len,
               unsigned char out[NB_SHA256_DIGEST_SIZE])
{
    nb_sha256_ctx ctx;
    nb_sha256_init(&ctx);
    nb_sha256_update(&ctx, data, len);
    nb_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------------- */
/* HMAC-SHA-256                                                              */

void nb_hmac_sha256(const void *key, size_t key_len,
                    const void *data, size_t len,
                    unsigned char out[NB_SHA256_DIGEST_SIZE])
{
    if (!out || (!key && key_len != 0) || (!data && len != 0)) {
        return;
    }

    unsigned char k[NB_SHA256_BLOCK_SIZE];
    unsigned char ipad[NB_SHA256_BLOCK_SIZE];
    unsigned char opad[NB_SHA256_BLOCK_SIZE];
    unsigned char inner[NB_SHA256_DIGEST_SIZE];
    memset(k, 0, sizeof(k));
    if (key_len > NB_SHA256_BLOCK_SIZE) {
        nb_sha256(key, key_len, k);
    } else if (key_len != 0) {
        memcpy(k, key, key_len);
    }
    for (size_t i = 0; i < NB_SHA256_BLOCK_SIZE; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36u);
        opad[i] = (unsigned char)(k[i] ^ 0x5Cu);
    }

    nb_sha256_ctx ctx;
    nb_sha256_init(&ctx);
    nb_sha256_update(&ctx, ipad, sizeof(ipad));
    nb_sha256_update(&ctx, data, len);
    nb_sha256_final(&ctx, inner);

    nb_sha256_init(&ctx);
    nb_sha256_update(&ctx, opad, sizeof(opad));
    nb_sha256_update(&ctx, inner, sizeof(inner));
    nb_sha256_final(&ctx, out);

    memset(k, 0, sizeof(k));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
    memset(inner, 0, sizeof(inner));
}

bool nb_hash_equal(const void *a, const void *b, size_t len)
{
    if (len == 0) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned char)(pa[i] ^ pb[i]);
    }
    return diff == 0;
}
