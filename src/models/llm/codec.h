#ifndef MORPH_LLM_CODEC_H
#define MORPH_LLM_CODEC_H

#include "models/llm.h"
#include "adapter.h"
#include "stream.h"
#include "http/sse.h"
#include "cJSON.h"

struct llm_codec {
	const char *endpoint;
	const char *input_key;
	const char *system_key;
	cJSON *(*input)(const struct model *model, const char *system_prompt,
			const struct chat_message *messages, int count);
	cJSON *(*tools)(const struct tool_desc *tools, int count);
	void (*configure)(const struct model *model, cJSON *request, int tool_count);
	sse_event_cb event;
};

const struct llm_codec *llm_codec_resolve(const struct model *model);
cJSON *llm_codec_schema(cJSON *params);

int llm_codec_add_options(struct morph_log *logger, cJSON *root, const char *json);

#endif
