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

	for (int i = 0; i < choices_count; i++)
		free(choice_copies[i]);

	if (rc < 0) {
		free(answer);
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
		return 0;
	}

	(void)tool_result_take_text(result, answer);
	log_info("ask_user: question='%s' answer='%s'", question, answer);
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
