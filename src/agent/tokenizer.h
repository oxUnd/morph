#ifndef TOKENIZER_H
#define TOKENIZER_H

#ifdef __cplusplus
extern "C" {
#endif

struct tokenizer *tokenizer_create(const char *model_name, int context_limit);
void tokenizer_destroy(struct tokenizer *tok);
int tokenizer_count(struct tokenizer *tok, const char *text);
int tokenizer_estimate_tokens(const char *text);

#ifdef __cplusplus
}
#endif

#endif
