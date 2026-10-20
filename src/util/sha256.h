#ifndef MORPH_SHA256_H
#define MORPH_SHA256_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32
struct sha256_ctx {
	uint32_t state[8];
	uint64_t bitlen;
	unsigned char data[SHA256_BLOCK_SIZE];
	size_t datalen;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const unsigned char *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, unsigned char hash[32]);
#ifdef __cplusplus
}
#endif
#endif
