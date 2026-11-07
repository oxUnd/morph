#ifndef MORPH_LLM_CHAT_CODEC_H
#define MORPH_LLM_CHAT_CODEC_H

#include "codec.h"

int llm_chat_encode_body(struct morph_log *logger, struct arena *arena, const char *model_id,
			 const char *system_prompt, const char **messages, int n, int max_tokens,
			 int stream, const char *extra_body_json, const struct llm_adapter *adapter,
			 int disable_thinking, char **out_body, size_t *out_body_len);
int llm_chat_decode_event(const char *event, const char *data, void *ud);
const struct llm_codec *llm_chat_codec(void);

#endif
