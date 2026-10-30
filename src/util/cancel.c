#include "cancel.h"
#include "signals.h"

void morph_cancel_token_reset(struct morph_cancel_token *token)
{
	if (token)
		morph_signal_flag_set(&token->cancelled, 0);
}

void morph_cancel_token_cancel(struct morph_cancel_token *token)
{
	if (token)
		morph_signal_flag_set(&token->cancelled, 1);
}

int morph_cancel_token_is_cancelled(const struct morph_cancel_token *token)
{
	return token && morph_signal_flag_get(&token->cancelled);
}
