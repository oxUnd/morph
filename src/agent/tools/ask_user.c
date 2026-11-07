#include "ask_user.h"
#include "agent/tool_runtime.h"
#include "util/log.h"
#include "util/json.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/error.h"
#include "util/cancel.h"

struct ask_user_context {
	ask_user_callback_fn cb;
	void *user_data;
};

int ask_user_options_to_json(struct json *data, const struct ask_user_options *options)
{
	struct ask_user_options defaults = {0};
	if (!options)
		options = &defaults;
	if (!json_is_object_value(data) || options->recommended_count < 0 ||
	    options->recommended_count > ASK_USER_CHOICES_MAX)
		return -EINVAL;
	struct json *indices = json_array();
	if (!indices)
		return -ENOMEM;
	for (int i = 0; i < options->recommended_count; i++) {
		struct json *index = json_number(options->recommended_indices[i]);
		if (!index) {
			json_free(indices);
			return -ENOMEM;
		}
		json_array_add(indices, index);
	}
	json_object_add(data, "recommended_indices", indices);
	if (!json_add_string(data, "response_policy",
			     options->optional ? "optional" : "required") ||
	    !json_add_number(data, "timeout_seconds", options->timeout_seconds))
		return -ENOMEM;
	return 0;
}

static int parse_ask_user_options(const struct json *root, const char *const *choices,
				  int choices_count, int min, int max,
				  struct ask_user_options *options)
{
	struct json *policy = json_member(root, "response_policy");
	struct json *timeout = json_member(root, "timeout_seconds");
	struct json *indices = json_member(root, "recommended_indices");
	if (policy) {
		if (!json_is_string_value(policy) ||
		    (strcmp(json_string_value(policy), "optional") != 0 &&
		     strcmp(json_string_value(policy), "required") != 0))
			return -EINVAL;
		options->optional = strcmp(json_string_value(policy), "optional") == 0;
	}
	options->timeout_seconds = options->optional ? ASK_USER_TIMEOUT_DEFAULT : 0;
	if (timeout) {
		if (!json_is_number_value(timeout) || json_number_value(timeout) < 1 ||
		    json_number_value(timeout) > 86400 ||
		    json_number_value(timeout) != (double)json_int_value(timeout))
			return -EINVAL;
		if (options->optional)
			options->timeout_seconds = json_int_value(timeout);
	}
	if (!indices)
		return 0;
	struct json *items = json_member(root, "choices");
	if (items && (!json_is_array_value(items) || json_count(items) != choices_count))
		return -EINVAL;
	if (!json_is_array_value(indices) || json_count(indices) > ASK_USER_CHOICES_MAX)
		return -EINVAL;
	int count = json_count(indices);
	if (count && (count < min || (max > 0 && count > max)))
		return -EINVAL;
	for (int i = 0; i < count; i++) {
		struct json *index = json_at(indices, i);
		if (!json_is_number_value(index) || json_number_value(index) < 0 ||
		    json_number_value(index) >= choices_count ||
		    json_number_value(index) != (double)json_int_value(index))
			return -EINVAL;
		if (!choices[json_int_value(index)][0])
			return -EINVAL;
		for (int j = 0; j < i; j++) {
			if (options->recommended_indices[j] == json_int_value(index) ||
			    strcmp(choices[options->recommended_indices[j]],
				   choices[json_int_value(index)]) == 0)
				return -EINVAL;
		}
		options->recommended_indices[i] = json_int_value(index);
	}
	options->recommended_count = count;
	return 0;
}

static void ask_user_context_destroy(void *user_data)
{
	free(user_data);
}

static struct json *ask_user_data_to_json(const char *question, const char *const *choices,
					  int choices_count, const char *selection_mode,
					  int min_choices, int max_choices, char *const *answers,
					  int answers_count, int no_input)
{
	struct json *root = json_object();
	if (!root)
		return NULL;

	struct json *choices_json = json_array();
	struct json *answers_json = NULL;
	if (!choices_json)
		goto fail;
	answers_json = json_array();
	if (!answers_json)
		goto fail;

	if (!json_add_string(root, "kind", "ask_user_response") ||
	    !json_add_string(root, "question", question ? question : "") ||
	    !json_add_string(root, "selection_mode", selection_mode ? selection_mode : "single") ||
	    !json_add_number(root, "min_choices", min_choices) ||
	    !json_add_number(root, "max_choices", max_choices) ||
	    !json_add_bool(root, "no_input", no_input))
		goto fail;

	for (int i = 0; i < choices_count; i++) {
		struct json *choice = json_string(choices[i] ? choices[i] : "");
		if (!choice)
			goto fail;
		json_array_add(choices_json, choice);
	}

	for (int i = 0; i < answers_count; i++) {
		struct json *answer = json_string(answers[i] ? answers[i] : "");
		if (!answer)
			goto fail;
		json_array_add(answers_json, answer);
	}

	json_object_add(root, "choices", choices_json);
	json_object_add(root, "answers", answers_json);
	return root;

fail:
	json_free(answers_json);
	json_free(choices_json);
	json_free(root);
	return NULL;
}

static struct json *ask_user_ui_to_json(struct json *data)
{
	struct json *ui = json_object();
	if (!ui)
		return NULL;

	if (!json_add_string(ui, "component", "ask_user") || !json_add_string(ui, "version", "1"))
		goto fail;

	struct json *copy = json_clone(data, 1);
	if (!copy)
		goto fail;
	json_object_add(ui, "data", copy);
	return ui;

fail:
	json_free(ui);
	return NULL;
}

static int attach_ask_user_result(struct tool_result *result,
				  const char *question,
				  const char *const *choices,
				  int choices_count,
				  const char *selection_mode,
				  int min_choices,
				  int max_choices,
				  char *const *answers,
				  int answers_count,
				  int no_input,
				  const struct ask_user_options *options,
				  const char *resolution)
{
	struct json *data =
		ask_user_data_to_json(question, choices, choices_count, selection_mode, min_choices,
				      max_choices, answers, answers_count, no_input);
	if (!data)
		return -ENOMEM;
	if (ask_user_options_to_json(data, options) != 0 ||
	    !json_add_string(data, "resolution", resolution)) {
		json_free(data);
		return -ENOMEM;
	}

	struct json *ui = ask_user_ui_to_json(data);
	if (!ui) {
		json_free(data);
		return -ENOMEM;
	}

	tool_result_clear(result);
	result->data = data;
	result->ui = ui;
	return tool_result_finalize(result);
}

static int ask_user_exec(const char *args_json, struct tool_result *result,
			 void *user_data)
{
	struct ask_user_context *ctx = user_data;
	ask_user_callback_fn callback = result && result->runtime && result->runtime->ask_user_fn
						? result->runtime->ask_user_fn
					: ctx ? ctx->cb
					      : NULL;
	void *callback_data = result && result->runtime && result->runtime->ask_user_fn
				      ? result->runtime->ask_user_user_data
			      : ctx ? ctx->user_data
				    : NULL;

	if (!result)
		return -EINVAL;
	if (!callback) {
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"ask_user callback not configured\"}"));
		return MORPH_ERR_NOT_CONFIGURED;
	}

	char question[1024] = {0};
	char selection_mode[16] = "single";
	int min_choices = 0;
	int max_choices = 0;
	const char *choices[64] = {NULL};
	int choices_count = 0;
	char *choice_copies[64] = {NULL};
	struct ask_user_options options = {0};
	int options_rc = 0;

	if (args_json && *args_json) {
		struct json *root = json_parse_prefix(args_json, NULL, NULL);
		if (root) {
			struct json *q = json_member_ci(root, "question");
			if (json_is_string_value(q) && json_string_value(q))
				strncpy(question, json_string_value(q), sizeof(question) - 1);

			struct json *mode = json_member_ci(root, "selection_mode");
			if (json_is_string_value(mode) && json_string_value(mode) &&
			    strcmp(json_string_value(mode), "multi") == 0)
				strncpy(selection_mode, "multi",
					sizeof(selection_mode) - 1);

			struct json *min = json_member_ci(root, "min_choices");
			if (json_is_number_value(min) && json_int_value(min) > 0)
				min_choices = json_int_value(min);

			struct json *max = json_member_ci(root, "max_choices");
			if (json_is_number_value(max) && json_int_value(max) > 0)
				max_choices = json_int_value(max);

			struct json *ch = json_member_ci(root, "choices");
			if (json_is_array_value(ch)) {
				int n = json_count(ch);
				if (n > 64)
					n = 64;
				for (int i = 0; i < n; i++) {
					struct json *item = json_at(ch, i);
					if (json_is_string_value(item) && json_string_value(item)) {
						choice_copies[choices_count] =
							strdup(json_string_value(item));
						if (choice_copies[choices_count]) {
							choices[choices_count] =
								choice_copies[choices_count];
							choices_count++;
						}
					}
				}
			}
			if (strcmp(selection_mode, "multi") == 0) {
				if (min_choices <= 0)
					min_choices = choices_count > 0 ? 1 : 0;
				if (min_choices > choices_count)
					min_choices = choices_count;
				if (max_choices > choices_count)
					max_choices = choices_count;
				if (max_choices > 0 && max_choices < min_choices)
					max_choices = min_choices;
			} else {
				min_choices = choices_count > 0 ? 1 : 0;
				max_choices = choices_count > 0 ? 1 : 0;
			}
			options_rc = parse_ask_user_options(root, choices, choices_count,
				min_choices, max_choices, &options);
			json_free(root);
		}
	}

	if (options_rc != 0) {
		for (int i = 0; i < choices_count; i++)
			free(choice_copies[i]);
		(void)tool_result_error(result, "invalid_arguments",
			"Invalid ask_user policy, timeout, or recommended indices.");
		return options_rc;
	}

	if (question[0] == '\0') {
		for (int i = 0; i < choices_count; i++)
			free(choice_copies[i]);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing or empty 'question' parameter. "
			"Usage: ask_user({\\\"question\\\": \\\"...\\\"})\"}"));
		return -EINVAL;
	}

	char **answers = NULL;
	int answers_count = 0;
	options.cancel_token = result->runtime ? result->runtime->http.cancel_token : NULL;
	int rc = callback(question, choices, choices_count, selection_mode, min_choices,
			  max_choices, &options, &answers, &answers_count, callback_data);
	if (morph_cancel_token_is_cancelled(options.cancel_token))
		rc = -ECANCELED;
	const char *resolution = "user";
	if (rc == -ETIMEDOUT && options.optional) {
		/* A timeout never impersonates an explicit answer or cancellation. */
		if (answers) {
			for (int i = 0; i < answers_count; i++)
				free(answers[i]);
			free(answers);
		}
		answers = NULL;
		answers_count = 0;
		resolution = "timeout_no_input";
		rc = 0;
		if (options.recommended_count) {
			answers = calloc((size_t)options.recommended_count, sizeof(*answers));
			if (!answers)
				rc = -ENOMEM;
			for (int i = 0; !rc && i < options.recommended_count; i++) {
				answers[i] = strdup(choices[options.recommended_indices[i]]);
				if (!answers[i])
					rc = -ENOMEM;
				else
					answers_count++;
			}
			resolution = "timeout_recommended";
		}
	}

	if (rc < 0) {
		if (answers) {
			for (int i = 0; i < answers_count; i++)
				free(answers[i]);
			free(answers);
		}
		for (int i = 0; i < choices_count; i++)
			free(choice_copies[i]);
		(void)tool_result_errorf(result, "ask_user_failed",
			"Failed to get user response: %s", morph_strerror(rc));
		return rc;
	}

	if (answers) {
		int write = 0;
		for (int read = 0; read < answers_count; read++) {
			if (answers[read] && answers[read][0]) {
				answers[write++] = answers[read];
			} else {
				free(answers[read]);
			}
		}
		answers_count = write;
	}

	if (!answers || answers_count <= 0) {
		if (answers)
			free(answers);
		if (strcmp(resolution, "user") == 0)
			resolution = "no_input";
		int result_rc = attach_ask_user_result(result, question, choices,
					     choices_count, selection_mode,
					     min_choices, max_choices,
					     NULL, 0, 1, &options, resolution);
		for (int i = 0; i < choices_count; i++)
			free(choice_copies[i]);
		return result_rc;
	}

	int result_rc = attach_ask_user_result(result, question, choices, choices_count,
				     selection_mode, min_choices, max_choices,
				     answers, answers_count, 0, &options, resolution);
	log_info(result && result->runtime ? result->runtime->http.logger : NULL,
		 "ask_user: question='%s' answers_count=%d", question, answers_count);
	for (int i = 0; i < answers_count; i++)
		free(answers[i]);
	free(answers);
	for (int i = 0; i < choices_count; i++)
		free(choice_copies[i]);
	return result_rc;
}

static int ask_user_register(struct tool_registry *reg, ask_user_callback_fn cb, void *user_data)
{
	if (!reg)
		return -EINVAL;

	struct ask_user_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->cb = cb;
	ctx->user_data = user_data;

	int rc = tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "ask_user",
		.description = "Ask the user a question. For optional preferences use "
		"response_policy=optional, with a 60-second idle timeout by default. "
		"Use required only for critical missing information; required has no "
		"automatic answer. Put preferred options in recommended_indices (zero-based). "
		"Optional timeout adopts recommendations or returns no input. "
		"Never treat timeout_recommended as the user choosing an answer. "
		"For no_input or timeout_no_input on optional questions, proceed with "
		"best judgment and state assumptions; do not repeat the same question. "
		"For required questions, no input does not supply missing information. "
		"Permissions must use the approval flow. "
		"Use when: the request is ambiguous with multiple valid "
		"interpretations; you need a decision between mutually "
		"exclusive approaches; you lack critical information. "
		"Do NOT use when: you can make a reasonable default choice "
		"(prefer action over inaction); the question is trivial; "
		"you already have enough context. If unsure, proceed with "
		"your best judgment and state your assumption. "
		"When presenting options, use 'choices' for structured "
		"selection. Use selection_mode='multi' only when the user may "
		"select more than one choice. Users may also provide their own "
		"answer instead of the suggested choices.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
		"\"question\":{\"type\":\"string\","
		"\"description\":\"The question to ask the user\"},"
		"\"choices\":{\"type\":\"array\","
		"\"items\":{\"type\":\"string\"},"
		"\"description\":\"Optional list of choices for the user "
		"to pick from\"},"
		"\"selection_mode\":{\"type\":\"string\","
		"\"enum\":[\"single\",\"multi\"],"
		"\"description\":\"Whether the user should pick one choice "
		"or multiple choices. Defaults to single.\"},"
		"\"min_choices\":{\"type\":\"integer\","
		"\"description\":\"Minimum number of choices required for "
		"multi-select.\"},"
		"\"max_choices\":{\"type\":\"integer\","
		"\"description\":\"Maximum number of choices allowed for "
		"multi-select.\"},"
		"\"recommended_indices\":{\"type\":\"array\",\"uniqueItems\":true,"
		"\"items\":{\"type\":\"integer\",\"minimum\":0},"
		"\"description\":\"Zero-based recommended choice indices; "
		"must satisfy selection limits.\"},"
		"\"response_policy\":{\"type\":\"string\",\"enum\":[\"optional\",\"required\"],"
		"\"description\":\"Use optional for preferences; required for "
		"critical missing input. Defaults to required.\"},"
		"\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":86400,"
		"\"description\":\"Idle timeout for optional questions. "
		"Defaults to 60; ignored for required.\"}"
		"},\"required\":[\"question\"]}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = ask_user_exec,
		.user_data = ctx,
		.user_data_destroy = ask_user_context_destroy,
	});
	if (rc != 0)
		free(ctx);
	return rc;
}

int ask_user_init(struct tool_registry *reg, ask_user_callback_fn callback, void *user_data)
{
	if (!callback)
		return -EINVAL;
	return ask_user_register(reg, callback, user_data);
}

int ask_user_init_deferred(struct tool_registry *reg)
{
	return ask_user_register(reg, NULL, NULL);
}
