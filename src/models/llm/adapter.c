#include "adapter.h"
#include <string.h>

static int compatible_supports_tool_choice(const struct model *model)
{
	(void)model;
	return 1;
}

static const struct llm_adapter compatible_adapter = {
	.name = "openai-chat-compatible",
	.assistant_tool_content_nullable = 1,
	.supports_tool_choice = compatible_supports_tool_choice,
};

static const struct llm_adapter responses_adapter = {
	.name = "openai-responses",
	.responses_api = 1,
	.supports_tool_choice = compatible_supports_tool_choice,
};

const char *llm_adapter_infer(const char *provider)
{
	if (provider && strcmp(provider, "deepseek") == 0)
		return "deepseek";
	return "openai-chat-compatible";
}

const struct llm_adapter *llm_adapter_resolve(const struct model *model)
{
	const char *name = model && model->adapter[0] ? model->adapter :
		llm_adapter_infer(model ? model->provider : NULL);

	if (strcmp(name, "deepseek") == 0)
		return llm_deepseek_adapter();
	if (strcmp(name, "openai-responses") == 0)
		return &responses_adapter;
	return &compatible_adapter;
}
