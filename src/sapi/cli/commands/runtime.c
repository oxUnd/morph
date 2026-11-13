#include "sapi/cli/commands/runtime.h"
#include "runtime/runtime.h"
#include "util/cleanup.h"
#include "ext/ext.h"
#include "credits.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/command_output.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/adapters/clipboard/clipboard.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/pages/model.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/adapters/input/readline.h"

static int cli_runtime_command_quit(struct cli_application *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	ctx->running = 0;
	CMD_OK(ctx, "Goodbye!");
	if (!cli_chat_active(ctx))
		cli_session_history_print_resume_hint(ctx);
	return 0;
}

static int cli_runtime_command_help(struct cli_application *ctx, int argc, char **argv)
{
	(void)ctx;
	const char *topic = cli_cmd_arg_borrow(argc, argv, 1);
	char full[64];
	if (topic) {
		if (topic[0] != '/') {
			snprintf(full, sizeof(full), "/%s", topic);
			topic = full;
		}
		const struct cli_command *e = cli_command_find_borrow(topic);
			if (!e) {
				CMD_ERROR(ctx, "unknown command: %s", topic);
				return -ENOENT;
			}
			{
				struct cli_output_segment segments[] = {
					{.style = CLI_OUTPUT_STYLE_BOLD, .text = e->name},
					{.style = CLI_OUTPUT_STYLE_PLAIN, .text = " — "},
					{.style = CLI_OUTPUT_STYLE_PLAIN, .text = e->desc},
				};

				(void)cli_command_output_segments(ctx, segments,
								  sizeof(segments) / sizeof(segments[0]),
								  2, 1);
			}
			if (e->usage && *e->usage)
				(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_DIM, 2,
							       "usage: %s", e->usage);
			return 0;
	}
	cli_print_help(ctx);
	return 0;
}

static int cli_runtime_command_copy(struct cli_application *ctx, int argc, char **argv)
{
	char *answer = NULL;
	int rc;

	(void)argv;
	if (argc != 1) {
		CMD_ERROR(ctx, "usage: /copy");
		return -EINVAL;
	}
	rc = runtime_session_last_answer_copy(ctx->runtime, &answer);
	if (rc) {
		if (rc == -ENODATA)
			CMD_ERROR(ctx, "no assistant answer to copy in this session");
		else
			CMD_ERROR(ctx, "copy failed: %s", morph_strerror(rc));
		return rc;
	}
	rc = cli_clipboard_copy_text(ctx, answer);
	free(answer);
		if (rc == CLI_CLIPBOARD_COPIED)
			CMD_OK(ctx, "copied last answer to clipboard");
		else if (rc == CLI_CLIPBOARD_REQUESTED) {
			(void)cli_command_output_linef(
				ctx, CLI_OUTPUT_STYLE_DIM, 0,
				"Copy unconfirmed: sent a clipboard request to the terminal.");
			rc = 0;
	} else {
		if (rc == -E2BIG)
			CMD_ERROR(ctx, "copy failed: answer exceeds the terminal "
				       "clipboard payload limit");
		else
			CMD_ERROR(ctx, "copy failed: %s", morph_strerror(rc));
	}
	return rc;
}

static int cli_runtime_command_model(struct cli_application *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg_borrow(argc, argv, 1);
	if (argc > 2) {
		CMD_ERROR(ctx, "usage: /model [normal|ultra|status|name]");
		return -EINVAL;
	}
	if (!name && ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON &&
	    cli_application_tui_borrow(ctx))
		return cli_model_show(ctx);
	if (name && (!strcmp(name, "normal") || !strcmp(name, "ultra"))) {
		int rc = runtime_model_set_ultra(ctx->runtime, !strcmp(name, "ultra"));
		if (rc != 0) {
			CMD_ERROR(ctx, "model mode change failed: %s", morph_strerror(rc));
			return rc;
		}
		cli_readline_sync_mode(ctx, 1);
		CMD_OK(ctx, "model mode: %s", name);
	} else if (name && strcmp(name, "status")) {
		int rc = runtime_session_set_model(ctx->runtime, name);
		if (rc != 0) {
			CMD_ERROR(ctx, "model change failed: %s", morph_strerror(rc));
			return rc;
		}
		CMD_OK(ctx, "model switched to: %s", name);
	} else {
		struct session current = {0};
		int ultra = 0;
		(void)runtime_session_current(ctx->runtime, &current);
		int rc = runtime_model_get_ultra(ctx->runtime, &ultra);
		if (rc != 0)
			return rc;
		if (name)
			(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_PLAIN, 0,
						       "current model: %s · mode: %s",
						       current.model,
						       ultra ? "ultra" : "normal");
		else
			(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_PLAIN, 0,
						       "current model: %s", current.model);
	}
	return 0;
}

static enum cli_output_style cli_runtime_trace_step_style(enum react_step_type type)
{
	switch (type) {
	case REACT_STEP_CONTENT:
	case REACT_STEP_THOUGHT:
	case REACT_STEP_OBSERVATION:
	case REACT_STEP_REASONING:
		return CLI_OUTPUT_STYLE_DIM;
	case REACT_STEP_ACTION:
		return CLI_OUTPUT_STYLE_YELLOW;
	case REACT_STEP_REFLECTION:
		return CLI_OUTPUT_STYLE_CYAN;
	case REACT_STEP_FINAL:
		return CLI_OUTPUT_STYLE_GREEN;
	default:
		return CLI_OUTPUT_STYLE_PLAIN;
	}
}

static void cli_runtime_print_trace_step(struct cli_application *ctx, int number,
					 enum react_step_type type, const char *content,
					 const char *tool_name)
{
	char number_text[32];
	char type_text[64];
	struct cli_output_segment segments[7];
	size_t count = 0;

	snprintf(number_text, sizeof(number_text), "%d. ", number);
	snprintf(type_text, sizeof(type_text), "[%s]", react_step_type_name(type));
	segments[count++] =
		(struct cli_output_segment){.style = CLI_OUTPUT_STYLE_PLAIN, .text = number_text};
	segments[count++] = (struct cli_output_segment){
		.style = cli_runtime_trace_step_style(type), .text = type_text};
	if (content) {
		segments[count++] =
			(struct cli_output_segment){.style = CLI_OUTPUT_STYLE_PLAIN, .text = " "};
		segments[count++] = (struct cli_output_segment){
			.style = CLI_OUTPUT_STYLE_PLAIN,
			.text = content,
			.untrusted = 1,
			.untrusted_mode = UTF8_TERMINAL_TEXT_SINGLE_LINE,
		};
	}
	if (tool_name) {
		segments[count++] = (struct cli_output_segment){
			.style = CLI_OUTPUT_STYLE_DIM, .text = " (tool: "};
		segments[count++] =
			(struct cli_output_segment){.style = CLI_OUTPUT_STYLE_DIM, .text = tool_name};
		segments[count++] =
			(struct cli_output_segment){.style = CLI_OUTPUT_STYLE_DIM, .text = ")"};
	}
	(void)cli_command_output_segments(ctx, segments, count, 2, 1);
}

static void cli_runtime_print_trace_steps(struct cli_application *ctx, struct react_step *steps,
					  int count, const char *state_name)
{
	struct react_step *cur = steps;
	int step = 1;
	while (cur) {
		cli_runtime_print_trace_step(ctx, step, cur->type, cur->content, cur->tool_name);
		cur = cur->next;
		step++;
	}
	(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_DIM, 0, "state: %s, steps: %d",
				       state_name ? state_name : "n/a", count);
}

static void cli_runtime_print_runtime_trace(struct cli_application *ctx,
					    const struct runtime_turn_status *status)
{
	for (int i = 0; status && i < status->step_count; i++) {
		const struct runtime_trace_step *step = &status->steps[i];
		cli_runtime_print_trace_step(ctx, i + 1, step->type, step->content,
					     step->tool_name);
	}
	(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_DIM, 0, "state: %s, steps: %d",
				       status ? react_state_name(status->state) : "n/a",
				       status ? status->step_count : 0);
}

static struct react_step *cli_runtime_json_to_react_steps(const char *json, int *out_count)
{
	if (!json)
		return NULL;
	cJSON *arr = cJSON_Parse(json);
	if (!cJSON_IsArray(arr)) {
		cJSON_Delete(arr);
		return NULL;
	}
	int count = cJSON_GetArraySize(arr);
	struct react_step head = {0};
	struct react_step *tail = &head;
	for (int i = 0; i < count; i++) {
		cJSON *obj = cJSON_GetArrayItem(arr, i);
		if (!cJSON_IsObject(obj))
			continue;
		cJSON *type_item = cJSON_GetObjectItem(obj, "type");
		const char *type_name =
			cJSON_IsString(type_item) ? type_item->valuestring : "Unknown";
		enum react_step_type type = REACT_STEP_THOUGHT;
		if (strcmp(type_name, "Thought") == 0)
			type = REACT_STEP_THOUGHT;
		else if (strcmp(type_name, "Action") == 0)
			type = REACT_STEP_ACTION;
		else if (strcmp(type_name, "Observation") == 0)
			type = REACT_STEP_OBSERVATION;
		else if (strcmp(type_name, "Reflection") == 0)
			type = REACT_STEP_REFLECTION;
		else if (strcmp(type_name, "Final") == 0)
			type = REACT_STEP_FINAL;
		else if (strcmp(type_name, "Reasoning") == 0)
			type = REACT_STEP_REASONING;
		else if (strcmp(type_name, "Content") == 0)
			type = REACT_STEP_CONTENT;
		cJSON *content = cJSON_GetObjectItem(obj, "content");
		cJSON *tool_name = cJSON_GetObjectItem(obj, "tool_name");
		cJSON *tool_args = cJSON_GetObjectItem(obj, "tool_args");
		cJSON *tool_call_id = cJSON_GetObjectItem(obj, "tool_call_id");
		char *args_buf = NULL;
		if (cJSON_IsString(tool_args) && tool_args->valuestring) {
			const char *tn = cJSON_IsString(tool_name) ? tool_name->valuestring : "";
			size_t ab_len = strlen(tn) + strlen(tool_args->valuestring) + 4;
			args_buf = malloc(ab_len);
			if (args_buf)
				snprintf(args_buf, ab_len, "%s(%s)", tn, tool_args->valuestring);
		}
		struct react_step *s = react_step_create(
			type, cJSON_IsString(content) ? content->valuestring : NULL,
			cJSON_IsString(tool_name) ? tool_name->valuestring : NULL,
			args_buf ? args_buf : NULL,
			cJSON_IsString(tool_call_id) ? tool_call_id->valuestring : NULL);
		if (s) {
			tail->next = s;
			tail = s;
		}
		free(args_buf);
	}
	cJSON_Delete(arr);
	if (out_count)
		*out_count = count;
	return head.next;
}

static int cli_runtime_command_trace(struct cli_application *ctx, int argc, char **argv)
{
	int from_db = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--from-db") == 0 || strcmp(argv[i], "--all") == 0)
			from_db = 1;
	}
	if (from_db) {
		int round_no = 0, aborted = 0;
		char *json = runtime_trace_latest_current_copy(ctx->runtime, &round_no, &aborted);
		if (!json) {
			(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_PLAIN, 0,
						       "no traces saved in DB for this session");
			return 0;
		}
		CMD_HEADER(ctx, "ReAct trace (round %d, %s)", round_no,
			   aborted ? "aborted" : "completed");
		int count = 0;
		struct react_step *steps MORPH_CLEANUP(react_steps_clear) =
			cli_runtime_json_to_react_steps(json, &count);
		if (steps) {
			cli_runtime_print_trace_steps(ctx, steps, count,
						      aborted ? "ABORT" : "DONE");
		} else {
			{
				struct cli_output_segment segments[] = {
					{.style = CLI_OUTPUT_STYLE_PLAIN, .text = "(raw) "},
					{.style = CLI_OUTPUT_STYLE_PLAIN,
					 .text = json,
					 .untrusted = 1,
					 .untrusted_mode = UTF8_TERMINAL_TEXT_SINGLE_LINE},
				};

				(void)cli_command_output_segments(
					ctx, segments, sizeof(segments) / sizeof(segments[0]), 2, 1);
			}
		}
		free(json);
		return 0;
	}
	struct runtime_turn_status status MORPH_CLEANUP(runtime_turn_status_cleanup) = {0};
	struct session current;
	if (runtime_turn_status_copy(ctx->runtime, &status) != 0 || status.step_count == 0) {
		(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_PLAIN, 0,
					       "no ReAct trace for current turn");
		return 0;
	}
	(void)runtime_session_current(ctx->runtime, &current);
	CMD_HEADER(ctx, "ReAct trace (%s)", current.name);
	cli_runtime_print_runtime_trace(ctx, &status);
	return 0;
}

static int cli_runtime_command_credits(struct cli_application *ctx, int argc, char **argv)
{
	struct credit_summary today;
	struct credit_summary session;
	morph_buf_t head MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t tail MORPH_CLEANUP(morph_buf_cleanup) = {0};
	const struct config *config;
	int over_limit = 0;
	int rc;

	(void)argc;
	(void)argv;
	config = runtime_config_borrow(ctx->runtime);
	rc = runtime_credit_summary_today_get(ctx->runtime, &today);
	if (rc != 0)
		return rc;
	rc = runtime_credit_summary_current_get(ctx->runtime, &session);
	if (rc != 0)
		return rc;

	rc = morph_buf_init(&head, 128);
	if (rc != 0)
		return rc;
	rc = morph_buf_init(&tail, 160);
	if (rc != 0)
		return rc;
	rc = morph_buf_printf(&head, "credits: %lld", (long long)today.credits);
	if (rc == 0 && config->credits.daily_limit >= 0) {
		rc = morph_buf_printf(&head, " / %d today", config->credits.daily_limit);
		over_limit = today.credits > config->credits.daily_limit;
	} else if (rc == 0) {
		rc = morph_buf_puts(&head, " today");
	}
	if (rc == 0)
		rc = morph_buf_printf(&tail, " | est. cost: %.6f %s | session: %lld credits",
				      today.estimated_cost, config->credits.currency,
				      (long long)session.credits);
	if (rc == 0 && session.event_count == 0)
		rc = morph_buf_puts(&tail, " | no events");
	if (rc != 0)
		return rc;
	{
		struct cli_output_segment segments[3] = {
			{.style = CLI_OUTPUT_STYLE_PLAIN, .text = morph_buf_cstr_borrow(&head)},
			{.style = CLI_OUTPUT_STYLE_YELLOW, .text = " (over limit)"},
			{.style = CLI_OUTPUT_STYLE_PLAIN, .text = morph_buf_cstr_borrow(&tail)},
		};

		(void)cli_command_output_segments(ctx, segments, over_limit ? 3u : 2u, 0, 1);
		if (!over_limit) {
			segments[1] = segments[2];
			(void)cli_command_output_segments(ctx, segments, 2, 0, 1);
		}
	}
	return 0;
}

static int cli_runtime_command_context(struct cli_application *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int active_items = 0;
	int transcript_count = 0;
	int total_tokens = 0;
	int tool_tokens = 0;
	int compactions = 0;
	int limit = 0;
	struct message *transcript =
		runtime_session_messages_current_copy(ctx->runtime, &transcript_count);

	runtime_session_messages_free(transcript);
	(void)runtime_session_model_context_stats(ctx->runtime, &active_items, &total_tokens,
						  &tool_tokens, &compactions, &limit);
	double pct = limit > 0 ? (double)total_tokens / limit * 100.0 : 0.0;
	{
		char tokens[96];
		char detail[192];
		struct cli_output_segment segments[] = {
			{.style = CLI_OUTPUT_STYLE_PLAIN, .text = "context: "},
			{.style = CLI_OUTPUT_STYLE_PLAIN, .text = tokens},
			{.style = CLI_OUTPUT_STYLE_PLAIN, .text = detail},
		};

		snprintf(tokens, sizeof(tokens), "%d / %d tokens (%.1f%%)", total_tokens, limit,
			 pct);
		snprintf(detail, sizeof(detail),
			 " | model items: %d | transcript: %d | tool results: %d tokens"
			 " | compactions: %d",
			 active_items, transcript_count, tool_tokens, compactions);
		segments[1].style = cli_command_output_ratio_style(pct / 100.0, 0.8);
		(void)cli_command_output_segments(ctx, segments,
						  sizeof(segments) / sizeof(segments[0]), 0, 1);
	}
	{
		morph_buf_t kinds_line MORPH_CLEANUP(morph_buf_cleanup) = {0};
		static const char *const kinds[] = {"user_message",	    "assistant_message",
						    "assistant_tool_calls", "tool_result",
						    "compaction_summary",   "background_receipt"};
		int count = 0;
		struct model_history_item *items =
			runtime_session_model_history_current_copy(ctx->runtime, 1, &count);

		if (morph_buf_init(&kinds_line, 128) == 0) {
			(void)morph_buf_puts(&kinds_line, "context by kind:");
		}
		for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
			int kind_count = 0;
			int kind_tokens = 0;

			for (struct model_history_item *item = items; item; item = item->next) {
				if (strcmp(item->kind, kinds[i]) == 0) {
					kind_count++;
					kind_tokens += item->token_count;
				}
			}
			if (kind_count > 0 && !kinds_line.failed)
				(void)morph_buf_printf(&kinds_line, " %s=%d/%dt", kinds[i],
						       kind_count, kind_tokens);
		}
		if (!kinds_line.failed)
			(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_PLAIN, 0, "%s",
						       morph_buf_cstr_borrow(&kinds_line));
		runtime_session_model_history_free(items);
	}
	{
		struct runtime_history_compaction_status status;

		if (runtime_session_compaction_status(ctx->runtime, &status) == 0)
			(void)cli_command_output_linef(
				ctx, CLI_OUTPUT_STYLE_PLAIN, 0, "last compaction: %s/%s %d -> %d tokens%s",
				status.trigger_kind, status.status, status.input_tokens,
				status.output_tokens, status.error_code < 0 ? " (failed)" : "");
	}
	return 0;
}

static int cli_runtime_command_compress(struct cli_application *ctx, int argc, char **argv)
{
	int window_removed = 0;
	int kept = 0;
	int rc;

	(void)argc;
	(void)argv;
	rc = runtime_session_compress(ctx->runtime, NULL, &window_removed, &kept);
	if (rc < 0) {
		CMD_ERROR(ctx, "compression failed: %s", morph_strerror(rc));
		return rc;
	}
	CMD_OK(ctx, "model history compacted atomically: removed %d active items, kept %d",
	       window_removed, kept);
	return 0;
}

static int cli_runtime_command_save(struct cli_application *ctx, int argc, char **argv)
{
	const char *fmt = cli_cmd_arg_borrow(argc, argv, 1);
	if (!fmt)
		fmt = "md";
	int count = 0;
	struct message *msgs = runtime_session_messages_current_copy(ctx->runtime, &count);
	struct session current;
	(void)runtime_session_current(ctx->runtime, &current);
	char filename[PATH_MAX];
	snprintf(filename, sizeof(filename), "%s_%lld.%s", current.name, (long long)time(NULL),
		 fmt);
	CMD_HEADER(ctx, "saving session to %s", filename);
	FILE *f = fopen(filename, "w");
	if (f) {
		fprintf(f, "# Session: %s\n\n", current.name);
		struct message *cur = msgs;
		while (cur) {
			fprintf(f, "**%s**: %s\n\n", cur->role, cur->content ? cur->content : "");
			cur = cur->next;
		}
		fclose(f);
		CMD_OK(ctx, "saved %d messages", count);
	} else {
		CMD_ERROR(ctx, "failed to open file for writing");
	}
	runtime_session_messages_free(msgs);
	return 0;
}

static int cli_runtime_command_config(struct cli_application *ctx, int argc, char **argv)
{
	const struct config *config = runtime_config_borrow(ctx->runtime);
	struct cli_output_field general[] = {
		{.label = "default_session", .text = config->general.default_session},
		{.label = "output_dir", .text = config->general.output_dir},
		{.label = "log_level", .text = config->general.log_level},
		{.label = "log_file", .text = config->general.log_file},
	};
	struct cli_output_field text[] = {
		{.label = "provider", .text = config->models.text.provider},
		{.label = "model", .text = config->models.text.model},
		{.label = "api_base", .text = config->models.text.api_base},
		{.label = "context_limit", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->models.text.context_limit},
		{.label = "max_tokens", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->models.text.max_tokens},
		{.label = "timeout_seconds", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->models.text.timeout_seconds},
	};
	struct cli_output_field vision[] = {
		{.label = "provider", .text = config->models.vision.provider},
		{.label = "model", .text = config->models.vision.model},
		{.label = "api_base", .text = config->models.vision.api_base},
		{.label = "context_limit", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->models.vision.context_limit},
	};
	struct cli_output_field image[] = {
		{.label = "provider", .text = config->models.image.provider},
		{.label = "adapter",
		 .text = config->models.image.adapter[0] ? config->models.image.adapter
							 : "(auto)"},
		{.label = "model", .text = config->models.image.model},
		{.label = "api_base", .text = config->models.image.api_base},
		{.label = "context_limit", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->models.image.context_limit},
	};
	struct cli_output_field video[] = {
		{.label = "provider", .text = config->models.video.provider},
		{.label = "adapter",
		 .text = config->models.video.adapter[0] ? config->models.video.adapter
							 : "(auto)"},
		{.label = "model", .text = config->models.video.model},
		{.label = "api_base", .text = config->models.video.api_base},
		{.label = "context_limit", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->models.video.context_limit},
	};
	struct cli_output_field credits[] = {
		{.label = "daily_limit", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->credits.daily_limit},
		{.label = "currency", .text = config->credits.currency},
		{.label = "cost_to_credit_coef", .kind = CLI_OUTPUT_FIELD_DECIMAL_3,
		 .decimal = config->credits.cost_to_credit_coef},
		{.label = "input_token_credit_coef", .kind = CLI_OUTPUT_FIELD_DECIMAL_6,
		 .decimal = config->credits.input_token_credit_coef},
		{.label = "output_token_credit_coef", .kind = CLI_OUTPUT_FIELD_DECIMAL_6,
		 .decimal = config->credits.output_token_credit_coef},
		{.label = "image_unit_credit_coef", .kind = CLI_OUTPUT_FIELD_DECIMAL_6,
		 .decimal = config->credits.image_unit_credit_coef},
		{.label = "video_second_credit_coef", .kind = CLI_OUTPUT_FIELD_DECIMAL_6,
		 .decimal = config->credits.video_second_credit_coef},
		{.label = "prices", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->credits.price_count},
	};
	struct cli_output_field react[] = {
		{.label = "max_iterations", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->react.max_iterations},
		{.label = "tool_timeout", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->react.tool_timeout_seconds},
		{.label = "tool_max_retries", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->react.tool_max_retries},
		{.label = "guardrail_enabled", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->react.guardrail_enabled},
		{.label = "guardrail_max_retries", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->react.guardrail_max_retries},
		{.label = "guardrail_max_empty_rounds", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->react.guardrail_max_empty_rounds},
		{.label = "guardrail_llm_model", .text = config->react.guardrail_llm_model},
		{.label = "hitl_enabled", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->react.hitl_enabled},
		{.label = "hitl_auto_approve_readonly", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->react.hitl_auto_approve_readonly},
		{.label = "exec.shell", .text = config->exec.shell},
		{.label = "exec.default_timeout_ms", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->exec.default_timeout_ms},
		{.label = "exec.yield_time_ms", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->exec.yield_time_ms},
		{.label = "exec.max_inline_output", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->exec.max_inline_output},
		{.label = "exec.max_session_output", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->exec.max_session_output},
		{.label = "exec.kill_grace_ms", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->exec.kill_grace_ms},
		{.label = "exec.network", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->exec.network},
	};
	struct cli_output_field context[] = {
		{.label = "threshold", .kind = CLI_OUTPUT_FIELD_DECIMAL_1,
		 .decimal = config->context.summarize_threshold_ratio},
		{.label = "target", .kind = CLI_OUTPUT_FIELD_DECIMAL_1,
		 .decimal = config->context.compress_target_ratio},
		{.label = "keep_rounds", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->context.keep_recent_rounds},
	};
	struct cli_output_field memory[] = {
		{.label = "enabled", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->memory.enabled},
		{.label = "capture_input_enabled", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->memory.capture_input_enabled},
		{.label = "save_tasks_enabled", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->memory.save_tasks_enabled},
		{.label = "learn_from_chats_enabled", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->memory.learn_from_chats_enabled},
		{.label = "max_facts", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->memory.max_facts},
		{.label = "max_past_tasks", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->memory.max_past_tasks},
		{.label = "max_tips", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->memory.max_tips},
		{.label = "max_reply_chars", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->memory.max_reply_chars},
	};
	struct cli_output_field render[] = {
		{.label = "prefer_image_protocol", .text = config->render.prefer_image_protocol},
		{.label = "mpv_args", .text = config->render.mpv_args},
	};
	struct cli_output_field ext[] = {
		{.label = "dir", .text = config->ext.dir},
		{.label = "default_max_memory_mb", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->ext.default_max_memory_mb},
		{.label = "default_max_cpu_seconds", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->ext.default_max_cpu_seconds},
	};
	struct cli_output_field prompt[] = {
		{.label = "system_prompt_file", .text = config->prompt.system_prompt_file},
		{.label = "system_prompt_dir", .text = config->prompt.system_prompt_dir},
	};
	struct cli_output_field skill[] = {
		{.label = "dir", .text = config->skill.dir},
	};
	struct cli_output_field mcp[] = {
		{.label = "server_count", .kind = CLI_OUTPUT_FIELD_INT,
		 .integer = config->mcp.server_count},
	};
	struct cli_output_section before_tools[] = {
		{.title = "general", .fields = general,
		 .field_count = sizeof(general) / sizeof(general[0])},
		{.title = "model.text", .fields = text,
		 .field_count = sizeof(text) / sizeof(text[0])},
		{.title = "model.vision", .fields = vision,
		 .field_count = sizeof(vision) / sizeof(vision[0])},
		{.title = "model.image", .fields = image,
		 .field_count = sizeof(image) / sizeof(image[0])},
		{.title = "model.video", .fields = video,
		 .field_count = sizeof(video) / sizeof(video[0])},
		{.title = "credits", .fields = credits,
		 .field_count = sizeof(credits) / sizeof(credits[0])},
		{.title = "react", .fields = react,
		 .field_count = sizeof(react) / sizeof(react[0])},
	};
	struct cli_output_section after_tools[] = {
		{.title = "context", .fields = context,
		 .field_count = sizeof(context) / sizeof(context[0])},
		{.title = "memory", .fields = memory,
		 .field_count = sizeof(memory) / sizeof(memory[0])},
		{.title = "render", .fields = render,
		 .field_count = sizeof(render) / sizeof(render[0])},
		{.title = "ext", .fields = ext,
		 .field_count = sizeof(ext) / sizeof(ext[0])},
		{.title = "prompt", .fields = prompt,
		 .field_count = sizeof(prompt) / sizeof(prompt[0])},
		{.title = "skill", .fields = skill,
		 .field_count = sizeof(skill) / sizeof(skill[0])},
		{.title = "mcp", .fields = mcp, .field_count = sizeof(mcp) / sizeof(mcp[0])},
	};

	(void)argc;
	(void)argv;
	(void)cli_command_output_sections(ctx, before_tools,
					  sizeof(before_tools) / sizeof(before_tools[0]));
	{
		struct cli_output_field fields[2];
		const char *disabled[DISABLED_TOOLS_MAX];
		const char *hitl[HITL_TOOLS_MAX];
		size_t count = 0;

		for (int i = 0; i < config->react.disabled_tools_count; i++)
			disabled[i] = config->react.disabled_tools[i];
		for (int i = 0; i < config->react.hitl_tools_count; i++)
			hitl[i] = config->react.hitl_tools[i];
		if (config->react.disabled_tools_count > 0)
			fields[count++] = (struct cli_output_field){
				.label = "disabled_tools",
				.kind = CLI_OUTPUT_FIELD_LIST,
				.items = disabled,
				.item_count = (size_t)config->react.disabled_tools_count,
			};
		if (config->react.hitl_tools_count > 0)
			fields[count++] = (struct cli_output_field){
				.label = "hitl_tools",
				.kind = CLI_OUTPUT_FIELD_LIST,
				.items = hitl,
				.item_count = (size_t)config->react.hitl_tools_count,
			};
		(void)cli_command_output_fields(ctx, fields, count);
	}
	{
		int ro = 0, rw = 0;
		int tool_count = runtime_tool_count(ctx->runtime);
		struct tool_desc tool;
		unsigned flags;
		char count_text[64];
		char readonly_text[64];
		char tail_text[64];
		const char **readonly_items =
			calloc((size_t)tool_count, sizeof(*readonly_items));
		size_t readonly_count = 0;
		for (int i = 0; i < tool_count; i++)
			if (runtime_tool_flags(ctx->runtime, i, &flags) == 0 &&
			    (flags & TOOL_FLAG_READONLY))
				ro++;
			else
				rw++;
		snprintf(count_text, sizeof(count_text), " %d registered (", tool_count);
		snprintf(readonly_text, sizeof(readonly_text), "%d readonly", ro);
		snprintf(tail_text, sizeof(tail_text), ", %d read-write)", rw);
		{
			struct cli_output_segment segments[] = {
				{.style = CLI_OUTPUT_STYLE_BOLD, .text = "[tools]"},
				{.style = CLI_OUTPUT_STYLE_PLAIN, .text = count_text},
				{.style = CLI_OUTPUT_STYLE_GREEN, .text = readonly_text},
				{.style = CLI_OUTPUT_STYLE_PLAIN, .text = tail_text},
			};

			(void)cli_command_output_segments(
				ctx, segments, sizeof(segments) / sizeof(segments[0]), 0, 1);
		}
		for (int i = 0; readonly_items && i < tool_count; i++)
			if (runtime_tool_info_borrow(ctx->runtime, i, &tool) == 0 &&
			    runtime_tool_flags(ctx->runtime, i, &flags) == 0 &&
			    (flags & TOOL_FLAG_READONLY))
				readonly_items[readonly_count++] = tool.name;
		{
			struct cli_output_field field = {
				.label = "readonly",
				.kind = CLI_OUTPUT_FIELD_LIST,
				.items = readonly_items,
				.item_count = readonly_count,
				.separator = ":",
			};

			(void)cli_command_output_fields(ctx, &field, 1);
		}
		free(readonly_items);
	}
	(void)cli_command_output_sections(ctx, after_tools,
					  sizeof(after_tools) / sizeof(after_tools[0]));
	for (int i = 0; i < config->mcp.server_count; i++) {
		const struct config_mcp_server *s = &config->mcp.servers[i];
		struct cli_output_field fields[6];
		struct cli_output_section section = {
			.title = NULL,
			.fields = fields,
			.field_indent = 4,
		};

		fields[section.field_count++] =
			(struct cli_output_field){.label = "name", .text = s->name};
		fields[section.field_count++] =
			(struct cli_output_field){.label = "transport", .text = s->transport};
		if (strcmp(s->transport, "stdio") == 0) {
			fields[section.field_count++] =
				(struct cli_output_field){.label = "command", .text = s->command};
			fields[section.field_count++] = (struct cli_output_field){
				.label = "args_count",
				.kind = CLI_OUTPUT_FIELD_INT,
				.integer = s->args_count,
			};
		} else {
			fields[section.field_count++] =
				(struct cli_output_field){.label = "http_url", .text = s->http_url};
		}
		fields[section.field_count++] = (struct cli_output_field){
			.label = "auto_connect",
			.kind = CLI_OUTPUT_FIELD_INT,
			.integer = s->auto_connect,
		};
		(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_PLAIN, 2,
					       "[[mcp.servers.%d]]", i);
		(void)cli_command_output_sections(ctx, &section, 1);
	}
	return 0;
}
static int cli_runtime_command_export_alias(struct cli_application *ctx, int argc, char **argv)
{
	(void)ctx;
	(void)argc;
	(void)argv;
	(void)cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_PLAIN, 0,
				       "use /save [format] instead");
	return 0;
}

static int cli_runtime_command_clear(struct cli_application *ctx, int argc, char **argv)
{
	(void)ctx;
	(void)argc;
	(void)argv;
	cli_command_output_clear_screen(ctx);
	return 0;
}

static int cli_runtime_command_recap(struct cli_application *ctx, int argc, char **argv)
{
	(void)argv;
	if (argc != 1) {
		CMD_ERROR(ctx, "usage: /recap");
		return -EINVAL;
	}
	return cli_recap_start(ctx, 0);
}

static const struct cli_command cli_runtime_entries[] = {
	{"/quit", cli_runtime_command_quit, "Exit the program", ""},
	{"/q", cli_runtime_command_quit, "Alias for /quit", ""},
	{"/help", cli_runtime_command_help, "Show help for commands", "/help [command]"},
	{"/h", cli_runtime_command_help, "Alias for /help", "/h [command]"},
	{"/copy", cli_runtime_command_copy, "Copy the last assistant answer to clipboard", "/copy"},
	{"/model", cli_runtime_command_model, "Select Normal or Ultra, or switch model", "/model [normal|ultra|status|name]"},
	{"/m", cli_runtime_command_model, "Alias for /model", "/m [normal|ultra|status|name]"},
	{"/trace", cli_runtime_command_trace, "Show ReAct trace for current turn",
	 "/trace [--from-db]"},
	{"/t", cli_runtime_command_trace, "Alias for /trace", "/t [--from-db]"},
	{"/context", cli_runtime_command_context, "Show token usage and context info", "/context"},
	{"/ctx", cli_runtime_command_context, "Alias for /context", "/ctx"},
	{"/credits", cli_runtime_command_credits, "Show credit usage and estimated cost",
	 "/credits"},
	{"/compress", cli_runtime_command_compress, "Manually compress context window",
	 "/compress"},
	{"/recap", cli_runtime_command_recap, "Summarize the current conversation", "/recap"},
	{"/cp", cli_runtime_command_compress, "Alias for /compress", "/cp"},
	{"/save", cli_runtime_command_save, "Export session to a file", "/save [format]"},
	{"/config", cli_runtime_command_config, "View current configuration", "/config"},
	{"/cfg", cli_runtime_command_config, "Alias for /config", "/cfg"},
	{"/export", cli_runtime_command_export_alias, "Alias for /save", "/export <format>"},
	{"/clear", cli_runtime_command_clear, "Clear the terminal screen", "/clear"},
	{"/cl", cli_runtime_command_clear, "Alias for /clear", "/cl"},
};

struct cli_command_group cli_runtime_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_runtime_entries,
		.count = (int)(sizeof(cli_runtime_entries) / sizeof(cli_runtime_entries[0])),
		.category = "Core",
	};
}
