#include "adapter.h"
#include "util/error.h"
#include <errno.h>
#include <string.h>

static int deepseek_thinking_enabled(const struct model *model)
{
	cJSON *root;
	cJSON *thinking;
	cJSON *type;
	int enabled;

	if (!model)
		return 0;
	enabled = strstr(model->model_id, "reasoner") != NULL ||
		strstr(model->model_id, "deepseek-v4") != NULL;
	if (!model->extra_body_json[0])
		return enabled;
	root = cJSON_ParseWithOpts(model->extra_body_json, NULL, 1);
	if (!root)
		return enabled;
	thinking = cJSON_GetObjectItem(root, "thinking");
	type = cJSON_IsObject(thinking) ?
		cJSON_GetObjectItem(thinking, "type") : NULL;
	if (cJSON_IsString(type) && type->valuestring) {
		if (strcmp(type->valuestring, "disabled") == 0)
			enabled = 0;
		else if (strcmp(type->valuestring, "enabled") == 0)
			enabled = 1;
	}
	cJSON_Delete(root);
	return enabled;
}

static int deepseek_supports_tool_choice(const struct model *model)
{
	return !deepseek_thinking_enabled(model);
}

static int deepseek_disable_thinking(cJSON *request)
{
	cJSON *thinking = cJSON_CreateObject();
	if (!thinking)
		MORPH_RETURN(-ENOMEM);
	if (!cJSON_AddStringToObject(thinking, "type", "disabled")) {
		cJSON_Delete(thinking);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON_DeleteItemFromObjectCaseSensitive(request, "thinking");
	if (!cJSON_AddItemToObject(request, "thinking", thinking)) {
		cJSON_Delete(thinking);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON *kwargs = cJSON_GetObjectItemCaseSensitive(request, "chat_template_kwargs");
	if (cJSON_IsObject(kwargs)) {
		cJSON_DeleteItemFromObjectCaseSensitive(kwargs, "enable_thinking");
		cJSON_DeleteItemFromObjectCaseSensitive(kwargs, "reasoning_effort");
	}
	cJSON_DeleteItemFromObjectCaseSensitive(request, "reasoning_effort");
	return 0;
}

static int deepseek_add_message_fields(const struct model *model, cJSON *message,
				       const struct chat_message *chat_message)
{
	if (!message || !chat_message)
		return 0;
	if (strcmp(chat_message->role, "assistant") != 0)
		return 0;
	if (deepseek_thinking_enabled(model)) {
		/* A checkpoint is context, not a model answer with a reasoning trace. */
		if (chat_message->is_context_summary) {
			cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");

			return cJSON_SetValuestring(role, "user") ? 0 : -ENOMEM;
		}
	} else if (!chat_message->reasoning_content) {
		return 0;
	}
	if (!cJSON_AddStringToObject(message, "reasoning_content",
				     chat_message->reasoning_content ?
				     chat_message->reasoning_content : ""))
		return -ENOMEM;
	return 0;
}

const struct llm_adapter *llm_deepseek_adapter(void)
{
	static const struct llm_adapter adapter = {
		.name = "deepseek",
		.capture_reasoning = 1,
		.supports_tool_choice = deepseek_supports_tool_choice,
		.disable_thinking = deepseek_disable_thinking,
		.add_message_fields = deepseek_add_message_fields,
	};

	return &adapter;
}
