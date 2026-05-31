#ifndef BPE_H
#define BPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

enum bpe_encoding {
	BPE_CL100K_BASE,
	BPE_O200K_BASE
};

struct bpe_encoder;

struct bpe_encoder *bpe_encoder_create(enum bpe_encoding encoding,
					const char *vocab_dir);
void bpe_encoder_destroy(struct bpe_encoder *enc);
int bpe_count_tokens(struct bpe_encoder *enc, const char *text);
int bpe_count_tokens_n(struct bpe_encoder *enc, const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif
