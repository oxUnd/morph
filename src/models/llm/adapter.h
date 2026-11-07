#ifndef LLM_ADAPTER_H
#define LLM_ADAPTER_H

#include "models/llm.h"
#include "util/json.h"

struct llm_adapter {
	const char *name;
	int responses_api;
	int capture_reasoning;
	int assistant_tool_content_nullable;
	int (*supports_tool_choice)(const struct model *model);
	int (*disable_thinking)(struct json *request);
	int (*add_message_fields)(const struct model *model, struct json *message,
				  const struct chat_message *chat_message);
};

const struct llm_adapter *llm_adapter_resolve(const struct model *model);
const char *llm_adapter_infer(const char *provider);
const struct llm_adapter *llm_deepseek_adapter(void);

#endif
