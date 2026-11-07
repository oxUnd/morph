#ifndef MORPH_LLM_CHAT_CODEC_H
#define MORPH_LLM_CHAT_CODEC_H

#include "codec.h"

/* Inputs are borrowed. *out_body starts NULL; success creates an owner for
 * cJSON_free and fills length. Failure preserves outputs; occupied: -EEXIST. */
int llm_chat_body_create(struct morph_log *logger, const char *model_id, const char *system_prompt,
			 const char **messages, int n, int max_tokens, int stream,
			 const char *extra_body_json, const struct llm_adapter *adapter,
			 int disable_thinking, char **out_body, size_t *out_body_len);
int llm_chat_decode_event(const char *event, const char *data, void *ud);
const struct llm_codec *llm_chat_codec_borrow(void);

#endif
