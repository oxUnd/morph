#include "adapter.h"
#include "util/error.h"
#include <errno.h>
#include <string.h>

static int deepseek_thinking_enabled(const struct model *model)
{
	struct json *root;
	const struct json *thinking;
	const char *type;
	const char *effort;
	int enabled;

	if (!model)
		return 0;
	enabled = strstr(model->model_id, "reasoner") != NULL ||
		  strstr(model->model_id, "deepseek-v4") != NULL ||
		  strcmp(model->model_id, "deepseek-flash") == 0;
	if (!model->extra_body_json[0])
		return enabled;
	root = json_parse_ex(model->extra_body_json, NULL, 1, NULL);
	if (!root)
		return enabled;
	thinking = json_member_ci(root, "thinking");
	type = json_string_value(json_member_ci(thinking, "type"));
	effort = json_string_value(json_member(root, "reasoning_effort"));
	if (effort)
		enabled = strcmp(effort, "none") != 0;
	if (type) {
		if (strcmp(type, "disabled") == 0)
			enabled = 0;
		else if (strcmp(type, "enabled") == 0)
			enabled = 1;
	}
	json_free(root);
	return enabled;
}

static int deepseek_supports_tool_choice(const struct model *model)
{
	return !deepseek_thinking_enabled(model);
}

static int deepseek_disable_thinking(struct json *request)
{
	struct json *thinking = json_object();
	if (!thinking)
		return -ENOMEM;
	if (!json_add_string(thinking, "type", "disabled")) {
		json_free(thinking);
		return -ENOMEM;
	}
	json_object_remove(request, "thinking");
	if (json_object_add(request, "thinking", thinking) != 0) {
		json_free(thinking);
		return -ENOMEM;
	}
	struct json *kwargs = json_member(request, "chat_template_kwargs");
	if (json_is_object_value(kwargs)) {
		json_object_remove(kwargs, "enable_thinking");
		json_object_remove(kwargs, "reasoning_effort");
	}
	json_object_remove(request, "reasoning_effort");
	return 0;
}

static int deepseek_add_message_fields(const struct model *model, struct json *message,
				       const struct chat_message *chat_message)
{
	if (!message || !chat_message)
		return 0;
	if (strcmp(chat_message->role, "assistant") != 0)
		return 0;
	if (deepseek_thinking_enabled(model)) {
		/* A checkpoint is context, not a model answer with a reasoning trace. */
		if (chat_message->is_context_summary) {
			struct json *role = json_member(message, "role");

			return json_string_update(role, "user") ? 0 : -ENOMEM;
		}
	} else if (!chat_message->reasoning_content) {
		return 0;
	}
	if (!json_add_string(message, "reasoning_content",
			     chat_message->reasoning_content ? chat_message->reasoning_content
							     : ""))
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
