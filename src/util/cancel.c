#include "cancel.h"

void morph_cancel_token_reset(struct morph_cancel_token *token)
{
	if (token)
		token->cancelled = 0;
}

void morph_cancel_token_cancel(struct morph_cancel_token *token)
{
	if (token)
		token->cancelled = 1;
}

int morph_cancel_token_is_cancelled(const struct morph_cancel_token *token)
{
	return token && token->cancelled;
}
