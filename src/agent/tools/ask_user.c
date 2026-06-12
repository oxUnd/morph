#include "ask_user.h"
#include "util/log.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/error.h"

struct ask_user_context {
	ask_user_callback_fn cb;
	void *user_data;
};

static void ask_user_context_destroy(void *user_data)
{
	free(user_data);
}

static cJSON *ask_user_data_to_json(const char *question,
				    const char *const *choices,
				    int choices_count,
				    const char *answer,
				    int no_input)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	cJSON *choices_json = cJSON_CreateArray();
	if (!choices_json)
		goto fail;

	if (!cJSON_AddStringToObject(root, "kind", "ask_user_response") ||
	    !cJSON_AddStringToObject(root, "question",
				     question ? question : "") ||
	    !cJSON_AddStringToObject(root, "answer", answer ? answer : "") ||
	    !cJSON_AddBoolToObject(root, "no_input", no_input))
		goto fail;

	for (int i = 0; i < choices_count; i++) {
		cJSON *choice = cJSON_CreateString(choices[i] ? choices[i] : "");
		if (!choice)
			goto fail;
		cJSON_AddItemToArray(choices_json, choice);
	}

	cJSON_AddItemToObject(root, "choices", choices_json);
	return root;

fail:
	cJSON_Delete(choices_json);
	cJSON_Delete(root);
	return NULL;
}

static cJSON *ask_user_ui_to_json(cJSON *data)
{
	cJSON *ui = cJSON_CreateObject();
	if (!ui)
		return NULL;

	if (!cJSON_AddStringToObject(ui, "component", "ask_user") ||
	    !cJSON_AddStringToObject(ui, "version", "1"))
		goto fail;

	cJSON *copy = cJSON_Duplicate(data, 1);
	if (!copy)
		goto fail;
	cJSON_AddItemToObject(ui, "data", copy);
	return ui;

fail:
	cJSON_Delete(ui);
	return NULL;
}

static int attach_ask_user_result(struct tool_result *result,
				  const char *question,
				  const char *const *choices,
				  int choices_count,
				  const char *answer,
				  int no_input)
{
	cJSON *data = ask_user_data_to_json(question, choices, choices_count,
					    answer, no_input);
	if (!data)
		return -ENOMEM;

	cJSON *ui = ask_user_ui_to_json(data);
	if (!ui) {
		cJSON_Delete(data);
		return -ENOMEM;
	}

	int rc = tool_result_take_data(result, data);
	if (rc != 0) {
		cJSON_Delete(ui);
		return rc;
	}

	rc = tool_result_take_ui(result, ui);
	if (rc != 0)
		return rc;
	return 0;
}

static int ask_user_exec(const char *args_json, struct tool_result *result,
			 void *user_data)
{
	struct ask_user_context *ctx = user_data;

	if (!result)
		return -EINVAL;
	if (!ctx || !ctx->cb) {
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"ask_user callback not configured\"}"));
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}

	char question[1024] = {0};
	const char *choices[64] = {NULL};
	int choices_count = 0;
	char *choice_copies[64] = {NULL};

	if (args_json && *args_json) {
		cJSON *root = cJSON_Parse(args_json);
		if (root) {
			cJSON *q = cJSON_GetObjectItem(root, "question");
			if (cJSON_IsString(q) && q->valuestring)
				strncpy(question, q->valuestring,
					sizeof(question) - 1);

			cJSON *ch = cJSON_GetObjectItem(root, "choices");
			if (cJSON_IsArray(ch)) {
				int n = cJSON_GetArraySize(ch);
				if (n > 64)
					n = 64;
				for (int i = 0; i < n; i++) {
					cJSON *item = cJSON_GetArrayItem(ch, i);
					if (cJSON_IsString(item)
					    && item->valuestring) {
						choice_copies[choices_count] =
							strdup(item->valuestring);
						if (choice_copies[choices_count]) {
							choices[choices_count] =
								choice_copies[choices_count];
							choices_count++;
						}
					}
				}
			}
			cJSON_Delete(root);
		}
	}

	if (question[0] == '\0') {
		for (int i = 0; i < choices_count; i++)
			free(choice_copies[i]);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing or empty 'question' parameter. "
			"Usage: ask_user({\\\"question\\\": \\\"...\\\"})\"}"));
		return -EINVAL;
	}

	char *answer = NULL;
	int rc = ctx->cb(question, choices, choices_count, &answer,
			 ctx->user_data);

	if (rc < 0) {
		free(answer);
		for (int i = 0; i < choices_count; i++)
			free(choice_copies[i]);
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"failed to get user response (code %d)\"}",
			 rc);
		(void)tool_result_take_text(result, strdup(err));
		return rc;
	}

	if (!answer || !answer[0]) {
		free(answer);
		(void)tool_result_take_text(result, strdup(
			"User provided no input. Proceed with your best judgment."));
		(void)attach_ask_user_result(result, question, choices,
					     choices_count, "", 1);
		for (int i = 0; i < choices_count; i++)
			free(choice_copies[i]);
		return 0;
	}

	(void)tool_result_take_text(result, answer);
	(void)attach_ask_user_result(result, question, choices, choices_count,
				     result->text.data, 0);
	log_info("ask_user: question='%s' answer='%s'", question, answer);
	for (int i = 0; i < choices_count; i++)
		free(choice_copies[i]);
	return 0;
}

int ask_user_init(struct tool_registry *reg, ask_user_callback_fn cb,
		  void *user_data)
{
	if (!reg || !cb)
		return -EINVAL;

	struct ask_user_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->cb = cb;
	ctx->user_data = user_data;

	int rc = tool_register(reg, "ask_user",
		"Ask the user a question and wait for their response. "
		"Use when: the request is ambiguous with multiple valid "
		"interpretations; you need a decision between mutually "
		"exclusive approaches; an action is irreversible or "
		"destructive; you lack critical information. "
		"Do NOT use when: you can make a reasonable default choice "
		"(prefer action over inaction); the question is trivial; "
		"you already have enough context. If unsure, proceed with "
		"your best judgment and state your assumption. "
		"When presenting options, use 'choices' for structured "
		"selection.",
		"{\"type\":\"object\",\"properties\":{"
		"\"question\":{\"type\":\"string\","
		"\"description\":\"The question to ask the user\"},"
		"\"choices\":{\"type\":\"array\","
		"\"items\":{\"type\":\"string\"},"
		"\"description\":\"Optional list of choices for the user "
		"to pick from\"}"
		"},\"required\":[\"question\"]}",
		ask_user_exec, ctx, ask_user_context_destroy);
	if (rc != 0)
		free(ctx);
	return rc;
}
