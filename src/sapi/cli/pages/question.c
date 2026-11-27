#include "sapi/cli/pages/question.h"
#include <nb/time.h>
#include <nb/err.h>
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
#include "sapi/cli/services/sessions.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/adapters/protocol/interaction.h"
#include "sapi/cli/components/question_form.h"
#include <nothing/tui/models/selection_model.h>
#include <nb/cleanup.h>

static int cli_question_set_ask_user_answers(char ***answers, int *answers_count,
					     const char *const *values, int values_count)
{
	if (!answers || !answers_count)
		return NB_ERR_INVAL;

	*answers = NULL;
	*answers_count = 0;
	if (values_count <= 0)
		return 0;

	char **out = calloc((size_t)values_count, sizeof(char *));
	if (!out)
		return NB_ERR_NOMEM;

	for (int i = 0; i < values_count; i++) {
		out[i] = strdup(values[i] ? values[i] : "");
		if (!out[i]) {
			for (int j = 0; j < i; j++)
				free(out[j]);

			free(out);
			return NB_ERR_NOMEM;
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
		return NB_ERR_PROTOCOL;

	answers = cJSON_GetObjectItemCaseSensitive(response_data, "answers");
	if (!cJSON_IsArray(answers))
		return NB_ERR_PROTOCOL;

	count = cJSON_GetArraySize(answers);
	if (count < validation->min_choices || count > 64 ||
	    (validation->max_choices > 0 && count > validation->max_choices) ||
	    ((!validation->selection_mode || strcmp(validation->selection_mode, "multi") != 0) &&
	     count > 1))
		return NB_ERR_INVAL;

	for (int i = 0; i < count; i++) {
		cJSON *answer = cJSON_GetArrayItem(answers, i);

		if (!cJSON_IsString(answer) || !answer->valuestring || !answer->valuestring[0])
			return NB_ERR_PROTOCOL;

		for (int j = 0; j < i; j++) {
			cJSON *previous = cJSON_GetArrayItem(answers, j);

			if (strcmp(answer->valuestring, previous->valuestring) == 0)
				return NB_ERR_INVAL;
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
		return NB_ERR_NOMEM;
	}

	if (!cJSON_AddStringToObject(request, "question", question ? question : "") ||
	    !cJSON_AddStringToObject(request, "selection_mode",
				     selection_mode ? selection_mode : "single") ||
	    !cJSON_AddNumberToObject(request, "min_choices", min_choices) ||
	    !cJSON_AddNumberToObject(request, "max_choices", max_choices)) {
		cJSON_Delete(choices_json);
		cJSON_Delete(request);
		return NB_ERR_NOMEM;
	}

	for (int i = 0; i < choices_count; i++) {
		cJSON *choice = cJSON_CreateString(choices[i] ? choices[i] : "");

		if (!choice) {
			cJSON_Delete(choices_json);
			cJSON_Delete(request);
			return NB_ERR_NOMEM;
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

static int cli_question_ask_user_on_owner(struct cli_application *ctx, void *opaque)
{
	const struct cli_question_ask_user_call *call = opaque;

	return cli_question_callback(call->question, call->choices, call->choices_count,
				     call->selection_mode, call->min_choices, call->max_choices,
				     call->options, call->answers, call->answers_count, ctx);
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
		return NB_ERR_INVAL;

	*answers = NULL;
	*answers_count = 0;
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return cli_question_json_ask_user(ctx, question, choices, choices_count,
						  selection_mode, min_choices, max_choices, options,
						  answers, answers_count);

	cli_cancel_monitor_clear(ctx);
	cli_presentation_prepare_prompt(ctx);
	cli_output_printf(ctx, CLI_FEED_QUESTION_STYLE CLI_FEED_QUESTION " ");
	(void)cli_output_print_untrusted(ctx, question, MORPH_TERMINAL_TEXT_SINGLE_LINE);
	cli_output_printf(ctx, ANSI_RESET "\n");
	int timeout = options && options->optional ? options->timeout_seconds : 0;
	char hint[160];
	if (timeout)
		snprintf(hint, sizeof(hint), "  Auto-continue after %d s without input%s.", timeout,
			 options->recommended_count ? " using recommended choices" : "");
	else
		snprintf(hint, sizeof(hint), "  Waiting for your answer · Esc cancel");


	char prompt[128];
	cli_output_printf(ctx, "%s\n", hint);
	if (choices && choices_count > 0) {
		for (int i = 0; i < choices_count; i++) {
			cli_output_printf(ctx, "  %d. ", i + 1);
			(void)cli_output_print_untrusted(ctx, choices[i],
							 MORPH_TERMINAL_TEXT_SINGLE_LINE);
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

	{
		char *input = NULL;
		const char *readline_prompt = cli_output_is_structured(ctx) ? "" : prompt;
		FILE *tty = fopen("/dev/tty", "r");
		int64_t previous_deadline = ctx->input->input_deadline_ms;
		int previous_timed_out = ctx->input->input_timed_out;
		ctx->input->input_deadline_ms =
			timeout ? nb_time_monotonic_ms() + (int64_t)timeout * 1000 : 0;
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
			return NB_ERR_TIMEOUT;
		}

		if (!input) {
			cli_output_printf(ctx, "\n");
			return NB_ERR_IO;
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
}

struct cli_question_pending {
	struct cli_question_ask_user_call *call;
	struct cli_event_queue_owner_call *completion;
	struct nt_view *form;
	int result;
};

static void cli_question_pending_cleanup(struct cli_question_pending **owner)
{
	free(*owner);
	*owner = NULL;
}

static void cli_question_form_complete(void *data, struct nt_view *view, int result)
{
	struct cli_question_pending *pending = data;
	struct cli_question_ask_user_call *call = pending->call;
	struct cli_application *ctx = cli_application_from_view(view);
	if (!result) {
		const struct nt_selection_model *model = cli_question_form_selection(view);
		const char *values[ASK_USER_CHOICES_MAX + 1] = {0};
		int count = 0;
		for (int i = 0; i < call->choices_count; i++) {
			if (nt_selection_model_is_selected(model, (size_t)i))
				values[count++] = call->choices[i];
		}

		const char *custom = cli_question_form_custom(view);
		if (custom && nt_selection_model_is_selected(model, (size_t)call->choices_count))
			values[count++] = custom;

		result = cli_question_set_ask_user_answers(call->answers, call->answers_count,
							   values, count);
		for (int i = 0; !result && i < count; i++) {
			cli_output_printf(ctx, "  %s" ANSI_GREEN CLI_FEED_DONE " " ANSI_RESET,
					  i + 1 < count ? "├ " : "└ ");
			(void)cli_output_print_untrusted(ctx, values[i],
							 MORPH_TERMINAL_TEXT_SINGLE_LINE);
			cli_output_printf(ctx, "\n");
		}
	}

	if (result == NB_ERR_CANCELLED || result == NB_ERR_INTERRUPTED)
		cli_request_cancel(ctx);

	if (result == NB_ERR_TIMEOUT)
		cli_output_printf(ctx, "  No response; %s.\n",
				  call->options && call->options->recommended_count
					  ? "continuing with recommended choices"
					  : "continuing without input");

	pending->result = result;
	(void)cli_chat_panel_close(ctx, 0);
}

static struct nt_view *cli_question_build(struct nt_builder *builder, struct nt_view *parent,
					  void *data)
{
	struct cli_question_pending *pending = data;
	struct cli_question_ask_user_call *call = pending->call;
	int multi = call->selection_mode && !strcmp(call->selection_mode, "multi");
	pending->form = cli_question_form_add(
		builder, parent,
		&(struct cli_question_form_options){
			.choices = call->choices,
			.count = call->choices_count,
			.multi = multi,
			.minimum = call->min_choices,
			.maximum = multi ? (call->max_choices ? call->max_choices
							      : call->choices_count)
					 : 1,
			.preferences = call->options,
			.allow_custom = 1,
			.complete = cli_question_form_complete,
			.data = pending});
	return pending->form ? cli_question_form_focus(pending->form) : NULL;
}

static void cli_question_panel_complete(struct cli_application *ctx, void *data, int result)
{
	struct cli_question_pending *pending = data;
	struct cli_event_queue_owner_call *completion = pending->completion;
	int rc = result ? result : pending->result;
	if (rc == NB_ERR_CANCELLED || rc == NB_ERR_INTERRUPTED) {
		if (cli_sessions_active(ctx))
			(void)cli_sessions_interrupt(ctx, 0);
		else
			cli_request_cancel(ctx);

		cli_cancel_state_reset(ctx);
	}

	free(pending);
	cli_event_queue_complete_owner_call(completion, rc);
}

static int cli_question_refresh(void *data, const struct nt_event *event)
{
	(void)event;
	struct cli_question_pending *pending = data;
	return cli_question_form_refresh(pending->form);
}

static int cli_question_start(struct cli_application *ctx, void *data,
			      struct cli_event_queue_owner_call *completion)
{
	struct cli_question_ask_user_call *call = data;
	if (!cli_chat_active(ctx)) {
		int rc = cli_question_ask_user_on_owner(ctx, data);
		cli_event_queue_complete_owner_call(completion, rc);
		return 0;
	}

	if (!call->answers || !call->answers_count || call->choices_count < 0 ||
	    call->choices_count > ASK_USER_CHOICES_MAX || call->min_choices < 0 ||
	    call->max_choices < 0)
		return NB_ERR_INVAL;

	*call->answers = NULL;
	*call->answers_count = 0;
	struct cli_question_pending *pending NB_CLEANUP(cli_question_pending_cleanup) =
		calloc(1, sizeof(*pending));
	if (!pending)
		return NB_ERR_NOMEM;

	*pending = (struct cli_question_pending){
		.call = call, .completion = completion, .result = NB_ERR_CANCELLED};
	cli_cancel_monitor_clear(ctx);
	cli_presentation_prepare_prompt(ctx);
	cli_output_printf(ctx, CLI_FEED_QUESTION_STYLE CLI_FEED_QUESTION " ");
	(void)cli_output_print_untrusted(ctx, call->question, MORPH_TERMINAL_TEXT_SINGLE_LINE);
	cli_output_printf(ctx, ANSI_RESET "\n");
	int rc =
		cli_chat_panel_mount(ctx, cli_question_build, pending, cli_question_panel_complete);
	if (!rc) {
		pending = NULL;
		cli_chat_panel_set_interaction(ctx, CLI_CHAT_INTERACTION_QUESTION);
		cli_chat_panel_set_refresh(ctx, cli_question_refresh);
	}

	return rc;
}

int cli_question_callback(const char *question, const char *const *choices, int choices_count,
			  const char *selection_mode, int min_choices, int max_choices,
			  const struct ask_user_options *options, char ***answers,
			  int *answers_count, void *user_data)
{
	struct cli_application *ctx = user_data;
	if (!ctx)
		return NB_ERR_INVAL;

	if (ctx->events && !cli_event_queue_is_owner(ctx)) {
		struct cli_question_ask_user_call call = {
			.question = question,
			.choices = choices,
			.choices_count = choices_count,
			.selection_mode = selection_mode,
			.min_choices = min_choices,
			.max_choices = max_choices,
			.options = options,
			.answers = answers,
			.answers_count = answers_count,
		};
		return ctx->presentation_mode == CLI_PRESENT_INTERACTIVE
			       ? cli_event_queue_call_owner_deferred(ctx, cli_question_start, &call)
			       : cli_event_queue_call_owner(ctx, cli_question_ask_user_on_owner,
							    &call);
	}

	/* Interactive callbacks wait on a worker; the UI owner only mounts controls. */
	if (cli_chat_active(ctx))
		return NB_ERR_BUSY;

	const struct nb_cancel *previous = ctx->input->input_cancel_token;
	ctx->input->input_cancel_token = options ? options->cancel_token : NULL;
	int rc = cli_question_ask_user_prompt(question, choices, choices_count, selection_mode,
					      min_choices, max_choices, options, answers,
					      answers_count, ctx);
	ctx->input->input_cancel_token = previous;
	return rc;
}
