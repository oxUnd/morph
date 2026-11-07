#ifndef MORPH_HISTORY_INTERNAL_H
#define MORPH_HISTORY_INTERNAL_H

#include "history.h"
#include "react.h"
#include "util/array.h"

/* Internal history contracts; transformations return owned strings. */
char *history_redact_all(struct react_context *ctx, const char *text);
char *history_redact_json(struct react_context *ctx, const char *text);
char *history_user_content(const struct model_history_item *item);
int history_item_tokens(const struct model_history_item *item, struct tokenizer *tokenizer,
			const char *text);
int history_store_turn_outcomes(struct db *db, int64_t session_id, const char *turn_id,
				const char *status, int error_code, struct tokenizer *tokenizer,
				int *changed);
int history_tool_call_input(const struct json *call, enum tool_input_kind *kind,
			    const char **input);

#endif
