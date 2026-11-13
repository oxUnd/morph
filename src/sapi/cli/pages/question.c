#include "sapi/cli/pages/question.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/adapters/input/state.h"
#include "agent/tools/ask_user.h"
#include "cJSON.h"
#include "sapi/cli/adapters/input/reader.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/adapters/protocol/interaction.h"
#include <morph/tui/widgets/select.h>
#include <morph/tui/terminal.h>
#include "util/child_io.h"
#include "util/cleanup.h"

static int cli_question_set_ask_user_answers(char ***answers, int *answers_count,
					     const char *const *values, int values_count)
{
	if (!answers || !answers_count)
		return -EINVAL;
	*answers = NULL;
	*answers_count = 0;
	if (values_count <= 0)
		return 0;

	char **out = calloc((size_t)values_count, sizeof(char *));
	if (!out)
		return -ENOMEM;
	for (int i = 0; i < values_count; i++) {
		out[i] = strdup(values[i] ? values[i] : "");
		if (!out[i]) {
			for (int j = 0; j < i; j++)
				free(out[j]);
			free(out);
			return -ENOMEM;
		}
	}
	*answers = out;
	*answers_count = values_count;
	return 0;
}

static int cli_question_recommended_answers(const char *const *choices,
					    const struct ask_user_options *options, char ***answers,
					    int *answers_count)
{
	const char *values[ASK_USER_CHOICES_MAX];
	for (int i = 0; i < options->recommended_count; i++)
		values[i] = choices[options->recommended_indices[i]];
	return cli_question_set_ask_user_answers(answers, answers_count, values,
						 options->recommended_count);
}

static int cli_question_parse_multi_choice_answer(char *input, const char *const *choices,
						  int choices_count, int min_choices,
						  int max_choices, char ***answers,
						  int *answers_count)
{
	int selected[64] = {0};
	int selected_count = 0;
	char *save = NULL;

	for (char *tok = strtok_r(input, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
		int n = atoi(tok);
		if (n < 1 || n > choices_count)
			continue;
		if (!selected[n - 1]) {
			selected[n - 1] = 1;
			selected_count++;
		}
	}

	if (selected_count < min_choices || (max_choices > 0 && selected_count > max_choices))
		return cli_question_set_ask_user_answers(answers, answers_count, NULL, 0);

	const char *values[64] = {0};
	int count = 0;
	for (int i = 0; i < choices_count; i++) {
		if (selected[i])
			values[count++] = choices[i];
	}
	return cli_question_set_ask_user_answers(answers, answers_count, values, count);
}

struct cli_question_ask_user_validation {
	const char *selection_mode;
	int min_choices;
	int max_choices;
};

static int cli_question_validate_ask_user_response(const cJSON *response_data, void *user_data)
{
	struct cli_question_ask_user_validation *validation = user_data;
	cJSON *answers;
	int count;

	if (!validation || !cJSON_IsObject(response_data))
		return MORPH_ERR_PROTOCOL;
	answers = cJSON_GetObjectItemCaseSensitive(response_data, "answers");
	if (!cJSON_IsArray(answers))
		return MORPH_ERR_PROTOCOL;
	count = cJSON_GetArraySize(answers);
	if (count < validation->min_choices || count > 64 ||
	    (validation->max_choices > 0 && count > validation->max_choices) ||
	    ((!validation->selection_mode || strcmp(validation->selection_mode, "multi") != 0) &&
	     count > 1))
		return -EINVAL;
	for (int i = 0; i < count; i++) {
		cJSON *answer = cJSON_GetArrayItem(answers, i);

		if (!cJSON_IsString(answer) || !answer->valuestring || !answer->valuestring[0])
			return MORPH_ERR_PROTOCOL;
		for (int j = 0; j < i; j++) {
			cJSON *previous = cJSON_GetArrayItem(answers, j);

			if (strcmp(answer->valuestring, previous->valuestring) == 0)
				return -EINVAL;
		}
	}
	return 0;
}

static int cli_question_json_ask_user(struct cli_application *ctx, const char *question,
				      const char *const *choices, int choices_count,
				      const char *selection_mode, int min_choices, int max_choices,
				      const struct ask_user_options *options, char ***answers,
				      int *answers_count)
{
	struct cli_question_ask_user_validation validation;
	cJSON *request;
	cJSON *choices_json;
	cJSON *response = NULL;
	cJSON *items;
	const char *values[64] = {0};
	int count;
	int rc;

	request = cJSON_CreateObject();
	choices_json = cJSON_CreateArray();
	if (!request || !choices_json) {
		cJSON_Delete(request);
		cJSON_Delete(choices_json);
		return -ENOMEM;
	}
	if (!cJSON_AddStringToObject(request, "question", question ? question : "") ||
	    !cJSON_AddStringToObject(request, "selection_mode",
				     selection_mode ? selection_mode : "single") ||
	    !cJSON_AddNumberToObject(request, "min_choices", min_choices) ||
	    !cJSON_AddNumberToObject(request, "max_choices", max_choices)) {
		cJSON_Delete(choices_json);
		cJSON_Delete(request);
		return -ENOMEM;
	}
	for (int i = 0; i < choices_count; i++) {
		cJSON *choice = cJSON_CreateString(choices[i] ? choices[i] : "");

		if (!choice) {
			cJSON_Delete(choices_json);
			cJSON_Delete(request);
			return -ENOMEM;
		}
		cJSON_AddItemToArray(choices_json, choice);
	}
	cJSON_AddItemToObject(request, "choices", choices_json);
	rc = ask_user_options_to_json(request, options);
	if (rc) {
		cJSON_Delete(request);
		return rc;
	}
	validation = (struct cli_question_ask_user_validation){
		.selection_mode = selection_mode,
		.min_choices = min_choices,
		.max_choices = max_choices,
	};
	rc = cli_interaction_request(ctx, "ask_user", request,
				     cli_question_validate_ask_user_response, &validation,
				     &response);
	cJSON_Delete(request);
	if (rc != 0)
		return rc;
	items = cJSON_GetObjectItemCaseSensitive(response, "answers");
	count = cJSON_GetArraySize(items);
	for (int i = 0; i < count; i++)
		values[i] = cJSON_GetStringValue(cJSON_GetArrayItem(items, i));
	rc = cli_question_set_ask_user_answers(answers, answers_count, values, count);
	cJSON_Delete(response);
	return rc;
}

struct cli_question_ask_user_call {
	struct cli_application *ctx;
	const char *question;
	const char *const *choices;
	int choices_count;
	const char *selection_mode;
	int min_choices;
	int max_choices;
	const struct ask_user_options *options;
	char ***answers;
	int *answers_count;
};

static int cli_question_ask_user_on_owner(void *opaque)
{
	struct cli_question_ask_user_call *call = opaque;
	struct cli_application *ctx = call->ctx;
	(void)ctx;

	return cli_question_callback(call->question, call->choices, call->choices_count,
				     call->selection_mode, call->min_choices, call->max_choices,
				     call->options, call->answers, call->answers_count, call->ctx);
}

static int cli_question_ask_user_prompt(const char *question, const char *const *choices,
					int choices_count, const char *selection_mode,
					int min_choices, int max_choices,
					const struct ask_user_options *options, char ***answers,
					int *answers_count, void *user_data)
{
	struct cli_application *ctx = user_data;
	int multi = selection_mode && strcmp(selection_mode, "multi") == 0;
	if (!ctx || !answers || !answers_count || choices_count < 0 || choices_count > 64 ||
	    min_choices < 0 || max_choices < 0)
		return -EINVAL;
	*answers = NULL;
	*answers_count = 0;
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return cli_question_json_ask_user(ctx, question, choices, choices_count,
						  selection_mode, min_choices, max_choices, options,
						  answers, answers_count);

	cli_cancel_monitor_clear(ctx);
	cli_presentation_prepare_prompt(ctx);
	cli_output_printf(ctx, CLI_FEED_QUESTION_STYLE CLI_FEED_QUESTION " ");
	(void)cli_output_print_untrusted(ctx, question, UTF8_TERMINAL_TEXT_SINGLE_LINE);
	cli_output_printf(ctx, ANSI_RESET "\n");
	int timeout = options && options->optional ? options->timeout_seconds : 0;
	char hint[160];
	if (timeout)
		snprintf(hint, sizeof(hint), "  Auto-continue after %d s without input%s.", timeout,
			 options->recommended_count ? " using recommended choices" : "");
	else
		snprintf(hint, sizeof(hint), "  Waiting for your answer · Esc cancel");

	int use_selector = choices_count > 0 || timeout;
#ifndef HAVE_READLINE
	/* Canonical terminal input can discard long lines before fgets sees them. */
	use_selector = 1;
#endif
	if (use_selector && isatty(STDOUT_FILENO)) {
		FILE *tty = NULL;
		FILE *input = stdin;

		if (!isatty(STDIN_FILENO)) {
			/* macOS poll rejects the /dev/tty alias. Open the actual
			 * terminal device when standard input is redirected. */
			const char *path = ttyname(STDOUT_FILENO);

			tty = path ? fopen(path, "r") : NULL;
			input = tty;
		}
		if (morph_tui_terminal_available(input, stdout)) {
			struct morph_tui_widget_select state = {0};
			char *labels[ASK_USER_CHOICES_MAX] = {0};
			const char *items[ASK_USER_CHOICES_MAX] = {0};
			int label_rc = 0;
			for (int i = 0; i < choices_count; i++) {
				int recommended = 0;
				for (int j = 0; options && j < options->recommended_count; j++)
					recommended |= options->recommended_indices[j] == i;
				morph_buf_t label = {0};
				label_rc = morph_buf_init(&label, 128);
				if (!label_rc)
					label_rc = morph_buf_printf(&label, "%s%s", choices[i],
								    recommended ? " (Recommended)"
										: "");
				if (!label_rc)
					labels[i] = morph_buf_detach(&label);
				morph_buf_cleanup(&label);
				items[i] = labels[i];
				if (label_rc)
					break;
			}
			const struct morph_tui_widget_select_options select_options = {
				.items = items,
				.interrupted = &ctx->interrupted,
				.color = cli_output_color_enabled(ctx),
				.allow_custom = 1,
				.custom_max_bytes = ASK_USER_ANSWER_MAX_BYTES,
				.timeout_seconds = timeout,
				.hint = hint,
				.cancel_token = options ? options->cancel_token : NULL,
			};
			char *custom = NULL;
			int rc = label_rc ? label_rc
					  : morph_tui_widget_select_init(
						    &state, (size_t)choices_count + 1, multi,
						    (size_t)min_choices,
						    multi ? (size_t)(max_choices ? max_choices
										 : choices_count)
							  : 1);

			if (!rc && options && options->recommended_count) {
				if (multi) {
					for (int i = 0; i < options->recommended_count; i++) {
						state.menu.list.index =
							(size_t)options->recommended_indices[i];
						(void)morph_tui_widget_select_update(&state, ' ');
					}
				}
				state.menu.list.index = (size_t)options->recommended_indices[0];
			}
			if (!rc)
				rc = cli_chat_flush(ctx);
			if (!rc)
				rc = morph_tui_widget_select_run(&state, &select_options, input,
								 stdout, &custom);
			if (rc == -ECANCELED || rc == -EINTR)
				cli_request_cancel(ctx);
			if (!rc) {
				const char *values[64];
				int count = 0;

				for (int i = 0; i < choices_count && count < 64; i++) {
					if (morph_tui_widget_select_selected(&state, (size_t)i))
						values[count++] = choices[i];
				}
				if (custom && count < 64)
					values[count++] = custom;
				rc = cli_question_set_ask_user_answers(answers, answers_count,
								       values, count);
				if (!rc) {
					for (int i = 0; i < count; i++) {
						cli_output_printf(ctx, "  %s" ANSI_GREEN CLI_FEED_DONE
						       " " ANSI_RESET, i + 1 < count ? "├ " : "└ ");
						(void)cli_output_print_untrusted(
							ctx, values[i],
							UTF8_TERMINAL_TEXT_SINGLE_LINE);
						cli_output_printf(ctx, "\n");
					}
				}
			}
			morph_tui_widget_select_cleanup(&state);
			for (int i = 0; i < choices_count; i++)
				free(labels[i]);
			free(custom);
			if (tty)
				fclose(tty);
			if (rc == -ETIMEDOUT)
				cli_output_printf(ctx, "  No response; %s.\n",
				       options->recommended_count
					       ? "continuing with recommended choices"
					       : "continuing without input");
			return rc;
		}
		if (tty)
			fclose(tty);
	}

	char prompt[128];
	cli_output_printf(ctx, "%s\n", hint);
	if (choices && choices_count > 0) {
		for (int i = 0; i < choices_count; i++) {
			cli_output_printf(ctx, "  %d. ", i + 1);
			(void)cli_output_print_untrusted(ctx, choices[i],
							 UTF8_TERMINAL_TEXT_SINGLE_LINE);
			for (int j = 0; options && j < options->recommended_count; j++) {
				if (options->recommended_indices[j] == i)
					cli_output_printf(ctx, " (Recommended)");
			}
			cli_output_printf(ctx, "\n");
		}
		if (multi && max_choices > 0) {
			snprintf(prompt, sizeof(prompt),
				 "  [" CLI_RL_IGNORE_START ANSI_GREEN CLI_RL_IGNORE_END
				 "1-%d, comma separated; %d-%d choices" CLI_RL_IGNORE_START
					 ANSI_RESET CLI_RL_IGNORE_END "]: ",
				 choices_count, min_choices, max_choices);
		} else if (multi) {
			snprintf(prompt, sizeof(prompt),
				 "  [" CLI_RL_IGNORE_START ANSI_GREEN CLI_RL_IGNORE_END
				 "1-%d, comma separated; min %d" CLI_RL_IGNORE_START
					 ANSI_RESET CLI_RL_IGNORE_END "]: ",
				 choices_count, min_choices);
		} else {
			snprintf(prompt, sizeof(prompt),
				 "  [" CLI_RL_IGNORE_START ANSI_GREEN CLI_RL_IGNORE_END
				 "1-%d" CLI_RL_IGNORE_START ANSI_RESET CLI_RL_IGNORE_END "]: ",
				 choices_count);
		}
	} else {
		snprintf(prompt, sizeof(prompt), "  > ");
	}
#ifdef HAVE_READLINE
	{
		char *input = NULL;
		const char *readline_prompt = cli_output_is_structured(ctx) ? "" : prompt;
		FILE *tty = fopen("/dev/tty", "r");
		int64_t previous_deadline = ctx->input->input_deadline_ms;
		int previous_timed_out = ctx->input->input_timed_out;
		ctx->input->input_deadline_ms =
			timeout ? morph_io_now_ms() + (int64_t)timeout * 1000 : 0;
		ctx->input->input_timed_out = 0;
		if (tty) {
			input = cli_reader_prompt_stream_copy(ctx, readline_prompt, tty);
			fclose(tty);
		} else {
			input = cli_readline_prompt_copy(ctx, readline_prompt);
		}
		int timed_out = ctx->input->input_timed_out;
		ctx->input->input_deadline_ms = previous_deadline;
		ctx->input->input_timed_out = previous_timed_out;
		if (timed_out) {
			free(input);
			return -ETIMEDOUT;
		}
		if (!input) {
			cli_output_printf(ctx, "\n");
			return -EIO;
		}
		if (!input[0] && options && options->recommended_count) {
			free(input);
			return cli_question_recommended_answers(choices, options, answers,
								answers_count);
		}
		cli_reader_remember(input);
		if (choices && choices_count > 0) {
			if (multi) {
				int rc = cli_question_parse_multi_choice_answer(
					input, choices, choices_count, min_choices, max_choices,
					answers, answers_count);
				free(input);
				return rc;
			}
			int n = atoi(input);
			if (n >= 1 && n <= choices_count) {
				const char *values[] = {choices[n - 1]};
				free(input);
				return cli_question_set_ask_user_answers(answers, answers_count,
									 values, 1);
			}
		}
		const char *values[] = {input};
		int rc = cli_question_set_ask_user_answers(answers, answers_count, values, 1);
		free(input);
		return rc;
	}
#else
	fflush(stdout);
	morph_buf_t line MORPH_CLEANUP(morph_buf_cleanup) = {0};
	char chunk[BUFSIZ];
	int line_rc = morph_buf_init(&line, BUFSIZ);
	if (line_rc != 0)
		return line_rc;
	FILE *tty = fopen("/dev/tty", "r");
	if (!tty) {
		cli_output_printf(ctx, "\n");
		return -ENOTTY;
	}
	int64_t deadline = timeout ? morph_io_now_ms() + (int64_t)timeout * 1000 : 0;
	for (;;) {
		if (!cli_input_fgets_until(ctx, chunk, sizeof(chunk), tty, &deadline)) {
			int read_error = errno;
			fclose(tty);
			cli_output_printf(ctx, "\n");
			return read_error ? morph_error_from_errno(read_error) : -EIO;
		}
		size_t length = strlen(chunk);
		int complete = length && chunk[length - 1] == '\n';
		if (complete)
			chunk[--length] = '\0';
		if (length > ASK_USER_ANSWER_MAX_BYTES - line.len) {
			fclose(tty);
			return -E2BIG;
		}
		line_rc = morph_buf_append(&line, chunk, length);
		if (line_rc != 0) {
			fclose(tty);
			return line_rc;
		}
		if (complete)
			break;
	}
	fclose(tty);
	char *buf = line.data;

	if (line.len && buf[line.len - 1] == '\r')
		buf[--line.len] = '\0';
	if (!buf[0] && options && options->recommended_count)
		return cli_question_recommended_answers(choices, options, answers, answers_count);

	if (choices && choices_count > 0) {
		if (multi)
			return cli_question_parse_multi_choice_answer(buf, choices, choices_count,
								      min_choices, max_choices,
								      answers, answers_count);
		int n = atoi(buf);
		if (n >= 1 && n <= choices_count) {
			const char *values[] = {choices[n - 1]};
			return cli_question_set_ask_user_answers(answers, answers_count, values, 1);
		}
	}

	const char *values[] = {buf};
	return cli_question_set_ask_user_answers(answers, answers_count, values, 1);
#endif
}

int cli_question_callback(const char *question, const char *const *choices, int choices_count,
			  const char *selection_mode, int min_choices, int max_choices,
			  const struct ask_user_options *options, char ***answers,
			  int *answers_count, void *user_data)
{
	struct cli_application *ctx = user_data;
	if (!ctx)
		return -EINVAL;
	if (ctx->events && !cli_event_queue_is_owner(ctx)) {
		struct cli_question_ask_user_call call = {
			ctx,	     question,	  choices, choices_count, selection_mode,
			min_choices, max_choices, options, answers,	  answers_count,
		};
		return cli_event_queue_call_owner(ctx, cli_question_ask_user_on_owner, &call);
	}
	const struct morph_cancel_token *previous = ctx->input->input_cancel_token;
	ctx->input->input_cancel_token = options ? options->cancel_token : NULL;
	int rc = cli_question_ask_user_prompt(question, choices, choices_count, selection_mode,
					      min_choices, max_choices, options, answers,
					      answers_count, ctx);
	ctx->input->input_cancel_token = previous;
	return rc;
}
