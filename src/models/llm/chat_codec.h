#ifndef MORPH_LLM_CHAT_CODEC_H
#define MORPH_LLM_CHAT_CODEC_H

#include "codec.h"
#include <nb/err.h>

/* Inputs are borrowed. *out_body starts NULL; success creates an owner for
 * cJSON_free and fills length. Failure preserves outputs; occupied: NB_ERR_EXISTS. */
int llm_chat_body_create(struct nb_log *logger, const char *model_id, const char *system_prompt,
			 const char **messages, int n, int max_tokens, int stream,
			 const char *extra_body_json, const struct llm_adapter *adapter,
			 int disable_thinking, char **out_body, size_t *out_body_len);
nb_status llm_chat_decode_event(nb_sse *parser, const nb_sse_event *event, void *ud,
			       nb_err *err);
const struct llm_codec *llm_chat_codec_borrow(void);

#endif
