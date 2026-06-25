#ifndef MORPH_CANCEL_H
#define MORPH_CANCEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <signal.h>

struct morph_cancel_token {
	volatile sig_atomic_t cancelled;
};

void morph_cancel_token_reset(struct morph_cancel_token *token);
void morph_cancel_token_cancel(struct morph_cancel_token *token);
int morph_cancel_token_is_cancelled(const struct morph_cancel_token *token);

#ifdef __cplusplus
}
#endif

#endif
