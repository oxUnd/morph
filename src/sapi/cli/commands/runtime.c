#include "sapi/cli/commands/registry.h"

static int cmd_quit(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	ctx->running = 0;
	CMD_OK(ctx, "Goodbye!");
	return 0;
}

static int cmd_help(struct cli_context *ctx, int argc, char **argv)
{
	(void)ctx;
	const char *topic = cli_cmd_arg(argc, argv, 1);
	char full[64];
	if (topic) {
		if (topic[0] != '/') {
			snprintf(full, sizeof(full), "/%s", topic);
			topic = full;
		}
		const struct cli_command *e = cli_command_find(topic);
		if (!e) {
			CMD_ERROR(ctx, "unknown command: %s", topic);
			return -ENOENT;
		}
		printf(ctx, ANSI_BOLD "  %s" ANSI_RESET " — %s\n", e->name, e->desc);
		if (e->usage && *e->usage)
			printf(ctx, "  " ANSI_DIM "usage: %s" ANSI_RESET "\n", e->usage);
		return 0;
	}
	cli_print_help(ctx);
	return 0;
}

static int cmd_model(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg(argc, argv, 1);
	struct session current;
	if (name) {
		int rc = runtime_session_set_model(ctx->runtime, name);
		if (rc != 0)
			return rc;
		CMD_OK(ctx, "model switched to: %s", name);
	} else {
		(void)runtime_session_current(ctx->runtime, &current);
		printf(ctx, "current model: %s\n", current.model);
	}
	return 0;
}

static void print_trace_steps(struct cli_context *ctx, struct react_step *steps, int count,
			      const char *state_name)
{
	struct react_step *cur = steps;
	int step = 1;
	while (cur) {
		const char *color = "";
		switch (cur->type) {
		case REACT_STEP_CONTENT:
		case REACT_STEP_THOUGHT:	color = ANSI_DIM; break;
		case REACT_STEP_ACTION:		color = ANSI_YELLOW; break;
		case REACT_STEP_OBSERVATION:	color = ANSI_DIM; break;
		case REACT_STEP_REFLECTION:	color = ANSI_CYAN; break;
		case REACT_STEP_FINAL:		color = ANSI_GREEN; break;
		case REACT_STEP_REASONING:	color = ANSI_DIM; break;
		default:			color = ""; break;
		}
		printf(ctx, "  %d. %s[%s]%s", step, color, react_step_type_name(cur->type),
		       ANSI_RESET);
		if (cur->content) {
			printf(ctx, " ");
			(void)cli_print_untrusted_text(ctx, cur->content,
						       UTF8_TERMINAL_TEXT_SINGLE_LINE);
		}
		if (cur->tool_name)
			printf(ctx, ANSI_DIM " (tool: %s)" ANSI_RESET, cur->tool_name);
		printf(ctx, "\n");
		cur = cur->next;
		step++;
	}
	printf(ctx, ANSI_DIM "state: %s, steps: %d" ANSI_RESET "\n",
	       state_name ? state_name : "n/a", count);
}

static void print_runtime_trace(struct cli_context *ctx, const struct runtime_turn_status *status)
{
	for (int i = 0; status && i < status->step_count; i++) {
		const struct runtime_trace_step *step = &status->steps[i];
		printf(ctx, "  %d. [%s]", i + 1, react_step_type_name(step->type));
		if (step->content) {
			printf(ctx, " ");
			(void)cli_print_untrusted_text(ctx, step->content,
						       UTF8_TERMINAL_TEXT_SINGLE_LINE);
		}
		if (step->tool_name)
			printf(ctx, ANSI_DIM " (tool: %s)" ANSI_RESET, step->tool_name);
		printf(ctx, "\n");
	}
	printf(ctx, ANSI_DIM "state: %s, steps: %d" ANSI_RESET "\n",
	       status ? react_state_name(status->state) : "n/a", status ? status->step_count : 0);
}

static struct react_step *json_to_react_steps(struct arena *arena, const char *json, int *out_count)
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
		const char *type_name = cJSON_IsString(type_item) ? type_item->valuestring : "Unknown";
		enum react_step_type type = REACT_STEP_THOUGHT;
		if (strcmp(type_name, "Thought") == 0)		type = REACT_STEP_THOUGHT;
		else if (strcmp(type_name, "Action") == 0)	type = REACT_STEP_ACTION;
		else if (strcmp(type_name, "Observation") == 0)	type = REACT_STEP_OBSERVATION;
		else if (strcmp(type_name, "Reflection") == 0)	type = REACT_STEP_REFLECTION;
		else if (strcmp(type_name, "Final") == 0)	type = REACT_STEP_FINAL;
		else if (strcmp(type_name, "Reasoning") == 0)	type = REACT_STEP_REASONING;
		else if (strcmp(type_name, "Content") == 0)	type = REACT_STEP_CONTENT;
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
			arena,
			type,
			cJSON_IsString(content) ? content->valuestring : NULL,
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

static int cmd_trace(struct cli_context *ctx, int argc, char **argv)
{
	int from_db = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--from-db") == 0 || strcmp(argv[i], "--all") == 0)
			from_db = 1;
	}
	if (from_db) {
		int round_no = 0, aborted = 0;
		char *json = runtime_trace_load_latest_current(ctx->runtime,
						       &round_no, &aborted);
		if (!json) {
			printf(ctx, "no traces saved in DB for this session\n");
			return 0;
		}
		CMD_HEADER(ctx, "ReAct trace (round %d, %s)", round_no,
			   aborted ? "aborted" : "completed");
		int count = 0;
		struct arena *arena = arena_create(64 * 1024);
		struct react_step *steps = json_to_react_steps(arena, json, &count);
		if (steps) {
			print_trace_steps(ctx, steps, count, aborted ? "ABORT" : "DONE");
		} else {
			printf(ctx, "  (raw) %s\n", json);
		}
		arena_destroy(arena);
		free(json);
		return 0;
	}
	struct runtime_turn_status status;
	struct session current;
	if (runtime_turn_status_get(ctx->runtime, &status) != 0 ||
	    status.step_count == 0) {
		printf(ctx, "no ReAct trace for current turn\n");
		return 0;
	}
	(void)runtime_session_current(ctx->runtime, &current);
	CMD_HEADER(ctx, "ReAct trace (%s)", current.name);
	print_runtime_trace(ctx, &status);
	runtime_turn_status_cleanup(&status);
	return 0;
}

static int cmd_credits(struct cli_context *ctx, int argc, char **argv)
{
	struct credit_summary today;
	struct credit_summary session;
	int rc;

	(void)argc;
	(void)argv;
	rc = runtime_credit_summary_today_get(ctx->runtime, &today);
	if (rc != 0)
		return rc;
	rc = runtime_credit_summary_current_get(ctx->runtime, &session);
	if (rc != 0)
		return rc;

	printf(ctx, "credits: %lld", (long long)today.credits);
	if ((*runtime_config_get(ctx->runtime)).credits.daily_limit >= 0) {
		printf(ctx, " / %d today", (*runtime_config_get(ctx->runtime)).credits.daily_limit);
		if (today.credits > (*runtime_config_get(ctx->runtime)).credits.daily_limit)
			printf(ctx, " " ANSI_YELLOW "(over limit)" ANSI_RESET);
	} else {
		printf(ctx, " today");
	}
	printf(ctx, " | est. cost: %.6f %s | session: %lld credits", today.estimated_cost,
	       (*runtime_config_get(ctx->runtime)).credits.currency, (long long)session.credits);
	if (session.event_count == 0)
		printf(ctx, " | no events");
	printf(ctx, "\n");
	return 0;
}

static int cmd_context(struct cli_context *ctx, int argc, char **argv)
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
		runtime_session_messages_current(ctx->runtime, &transcript_count);

	runtime_session_messages_free(transcript);
	(void)runtime_session_model_context_stats(ctx->runtime, &active_items,
		&total_tokens, &tool_tokens, &compactions, &limit);
	double pct = limit > 0 ? (double)total_tokens / limit * 100.0 : 0.0;
	printf(ctx,
	       "context: %s%d / %d tokens (%.1f%%)%s | model items: %d | "
	       "transcript: %d | tool results: %d tokens | compactions: %d\n",
	       pct >= 80.0 ? ANSI_YELLOW : "", total_tokens, limit, pct,
	       pct >= 80.0 ? ANSI_RESET : "", active_items, transcript_count, tool_tokens,
	       compactions);
	{
		static const char *const kinds[] = {
			"user_message", "assistant_message",
			"assistant_tool_calls", "tool_result",
			"compaction_summary", "background_receipt"
		};
		int count = 0;
		struct model_history_item *items =
			runtime_session_model_history_current(ctx->runtime, 1, &count);

		printf(ctx, "context by kind:");
		for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
			int kind_count = 0;
			int kind_tokens = 0;

			for (struct model_history_item *item = items; item;
			     item = item->next) {
				if (strcmp(item->kind, kinds[i]) == 0) {
					kind_count++;
					kind_tokens += item->token_count;
				}
			}
			if (kind_count > 0)
				printf(ctx, " %s=%d/%dt", kinds[i], kind_count, kind_tokens);
		}
		printf(ctx, "\n");
		runtime_session_model_history_free(items);
	}
	{
		struct runtime_history_compaction_status status;

		if (runtime_session_compaction_status(ctx->runtime, &status) == 0)
			printf(ctx, "last compaction: %s/%s %d -> %d tokens%s\n",
			       status.trigger_kind, status.status, status.input_tokens,
			       status.output_tokens, status.error_code < 0 ? " (failed)" : "");
	}
	return 0;
}

static int cmd_compress(struct cli_context *ctx, int argc, char **argv)
{
	int window_removed = 0;
	int kept = 0;
	int rc;

	(void)argc;
	(void)argv;
	rc = runtime_session_compress(ctx->runtime, NULL,
					      &window_removed, &kept);
	if (rc < 0) {
		CMD_ERROR(ctx, "compression failed: %s", morph_strerror(rc));
		return rc;
	}
	CMD_OK(ctx, "model history compacted atomically: removed %d active items, kept %d",
	       window_removed, kept);
	return 0;
}

static int cmd_save(struct cli_context *ctx, int argc, char **argv)
{
	const char *fmt = cli_cmd_arg(argc, argv, 1);
	if (!fmt)
		fmt = "md";
	int count = 0;
	struct message *msgs = runtime_session_messages_current(ctx->runtime, &count);
	struct session current;
	(void)runtime_session_current(ctx->runtime, &current);
	char filename[PATH_MAX];
	snprintf(filename, sizeof(filename), "%s_%lld.%s",
		 current.name,
		 (long long)time(NULL), fmt);
	CMD_HEADER(ctx, "saving session to %s", filename);
	FILE *f = fopen(filename, "w");
	if (f) {
		fprintf(f, "# Session: %s\n\n", current.name);
		struct message *cur = msgs;
		while (cur) {
			fprintf(f, "**%s**: %s\n\n", cur->role,
				cur->content ? cur->content : "");
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

static int cmd_config(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf(ctx, ANSI_BOLD "[general]" ANSI_RESET "\n");
	printf(ctx, "  default_session = %s\n",
	       (*runtime_config_get(ctx->runtime)).general.default_session);
	printf(ctx, "  output_dir = %s\n", (*runtime_config_get(ctx->runtime)).general.output_dir);
	printf(ctx, "  log_level = %s\n", (*runtime_config_get(ctx->runtime)).general.log_level);
	printf(ctx, "  log_file = %s\n", (*runtime_config_get(ctx->runtime)).general.log_file);
	printf(ctx, ANSI_BOLD "[model.text]" ANSI_RESET "\n");
	printf(ctx, "  provider = %s\n", (*runtime_config_get(ctx->runtime)).models.text.provider);
	printf(ctx, "  model = %s\n", (*runtime_config_get(ctx->runtime)).models.text.model);
	printf(ctx, "  api_base = %s\n", (*runtime_config_get(ctx->runtime)).models.text.api_base);
	printf(ctx, "  context_limit = %d\n",
	       (*runtime_config_get(ctx->runtime)).models.text.context_limit);
	printf(ctx, "  max_tokens = %d\n",
	       (*runtime_config_get(ctx->runtime)).models.text.max_tokens);
	printf(ctx, "  timeout_seconds = %d\n",
	       (*runtime_config_get(ctx->runtime)).models.text.timeout_seconds);
	printf(ctx, ANSI_BOLD "[model.vision]" ANSI_RESET "\n");
	printf(ctx, "  provider = %s\n",
	       (*runtime_config_get(ctx->runtime)).models.vision.provider);
	printf(ctx, "  model = %s\n", (*runtime_config_get(ctx->runtime)).models.vision.model);
	printf(ctx, "  api_base = %s\n",
	       (*runtime_config_get(ctx->runtime)).models.vision.api_base);
	printf(ctx, "  context_limit = %d\n",
	       (*runtime_config_get(ctx->runtime)).models.vision.context_limit);
	printf(ctx, ANSI_BOLD "[model.image]" ANSI_RESET "\n");
	printf(ctx, "  provider = %s\n", (*runtime_config_get(ctx->runtime)).models.image.provider);
	printf(ctx, "  adapter = %s\n",
	       (*runtime_config_get(ctx->runtime)).models.image.adapter[0]
		       ? (*runtime_config_get(ctx->runtime)).models.image.adapter
		       : "(auto)");
	printf(ctx, "  model = %s\n", (*runtime_config_get(ctx->runtime)).models.image.model);
	printf(ctx, "  api_base = %s\n", (*runtime_config_get(ctx->runtime)).models.image.api_base);
	printf(ctx, "  context_limit = %d\n",
	       (*runtime_config_get(ctx->runtime)).models.image.context_limit);
	printf(ctx, ANSI_BOLD "[model.video]" ANSI_RESET "\n");
	printf(ctx, "  provider = %s\n", (*runtime_config_get(ctx->runtime)).models.video.provider);
	printf(ctx, "  adapter = %s\n",
	       (*runtime_config_get(ctx->runtime)).models.video.adapter[0]
		       ? (*runtime_config_get(ctx->runtime)).models.video.adapter
		       : "(auto)");
	printf(ctx, "  model = %s\n", (*runtime_config_get(ctx->runtime)).models.video.model);
	printf(ctx, "  api_base = %s\n", (*runtime_config_get(ctx->runtime)).models.video.api_base);
	printf(ctx, "  context_limit = %d\n",
	       (*runtime_config_get(ctx->runtime)).models.video.context_limit);
	printf(ctx, ANSI_BOLD "[credits]" ANSI_RESET "\n");
	printf(ctx, "  daily_limit = %d\n",
	       (*runtime_config_get(ctx->runtime)).credits.daily_limit);
	printf(ctx, "  currency = %s\n", (*runtime_config_get(ctx->runtime)).credits.currency);
	printf(ctx, "  cost_to_credit_coef = %.3f\n",
	       (*runtime_config_get(ctx->runtime)).credits.cost_to_credit_coef);
	printf(ctx, "  input_token_credit_coef = %.6f\n",
	       (*runtime_config_get(ctx->runtime)).credits.input_token_credit_coef);
	printf(ctx, "  output_token_credit_coef = %.6f\n",
	       (*runtime_config_get(ctx->runtime)).credits.output_token_credit_coef);
	printf(ctx, "  image_unit_credit_coef = %.6f\n",
	       (*runtime_config_get(ctx->runtime)).credits.image_unit_credit_coef);
	printf(ctx, "  video_second_credit_coef = %.6f\n",
	       (*runtime_config_get(ctx->runtime)).credits.video_second_credit_coef);
	printf(ctx, "  prices = %d\n", (*runtime_config_get(ctx->runtime)).credits.price_count);
	printf(ctx, ANSI_BOLD "[react]" ANSI_RESET "\n");
	printf(ctx, "  max_iterations = %d\n",
	       (*runtime_config_get(ctx->runtime)).react.max_iterations);
	printf(ctx, "  tool_timeout = %d\n",
	       (*runtime_config_get(ctx->runtime)).react.tool_timeout_seconds);
	printf(ctx, "  tool_max_retries = %d\n",
	       (*runtime_config_get(ctx->runtime)).react.tool_max_retries);
	printf(ctx, "  guardrail_enabled = %d\n",
	       (*runtime_config_get(ctx->runtime)).react.guardrail_enabled);
	printf(ctx, "  guardrail_max_retries = %d\n",
	       (*runtime_config_get(ctx->runtime)).react.guardrail_max_retries);
	printf(ctx, "  guardrail_max_empty_rounds = %d\n",
	       (*runtime_config_get(ctx->runtime)).react.guardrail_max_empty_rounds);
	printf(ctx, "  guardrail_llm_model = %s\n",
	       (*runtime_config_get(ctx->runtime)).react.guardrail_llm_model);
	printf(ctx, "  hitl_enabled = %d\n",
	       (*runtime_config_get(ctx->runtime)).react.hitl_enabled);
	printf(ctx, "  hitl_auto_approve_readonly = %d\n",
	       (*runtime_config_get(ctx->runtime)).react.hitl_auto_approve_readonly);
	printf(ctx, "  exec.shell = %s\n", (*runtime_config_get(ctx->runtime)).exec.shell);
	printf(ctx, "  exec.default_timeout_ms = %d\n",
	       (*runtime_config_get(ctx->runtime)).exec.default_timeout_ms);
	printf(ctx, "  exec.yield_time_ms = %d\n",
	       (*runtime_config_get(ctx->runtime)).exec.yield_time_ms);
	printf(ctx, "  exec.max_inline_output = %d\n",
	       (*runtime_config_get(ctx->runtime)).exec.max_inline_output);
	printf(ctx, "  exec.max_session_output = %d\n",
	       (*runtime_config_get(ctx->runtime)).exec.max_session_output);
	printf(ctx, "  exec.kill_grace_ms = %d\n",
	       (*runtime_config_get(ctx->runtime)).exec.kill_grace_ms);
	printf(ctx, "  exec.network = %d\n", (*runtime_config_get(ctx->runtime)).exec.network);
	if ((*runtime_config_get(ctx->runtime)).react.disabled_tools_count > 0) {
		printf(ctx, "  disabled_tools =");
		for (int i = 0; i < (*runtime_config_get(ctx->runtime)).react.disabled_tools_count; i++)
			printf(ctx, " %s",
			       (*runtime_config_get(ctx->runtime)).react.disabled_tools[i]);
		printf(ctx, "\n");
	}
	if ((*runtime_config_get(ctx->runtime)).react.hitl_tools_count > 0) {
		printf(ctx, "  hitl_tools =");
		for (int i = 0; i < (*runtime_config_get(ctx->runtime)).react.hitl_tools_count; i++)
			printf(ctx, " %s", (*runtime_config_get(ctx->runtime)).react.hitl_tools[i]);
		printf(ctx, "\n");
	}
	{
		int ro = 0, rw = 0;
		int tool_count = runtime_tool_count(ctx->runtime);
		struct tool_desc tool;
		unsigned flags;
		for (int i = 0; i < tool_count; i++)
			if (runtime_tool_flags(ctx->runtime, i, &flags) == 0 &&
			    (flags & TOOL_FLAG_READONLY))
				ro++;
			else
				rw++;
		printf(ctx,
		       ANSI_BOLD "[tools]" ANSI_RESET " %d registered"
				 " (" ANSI_GREEN "%d readonly" ANSI_RESET ", %d read-write)\n",
		       tool_count, ro, rw);
		printf(ctx, "  readonly:");
		for (int i = 0; i < tool_count; i++)
			if (runtime_tool_info(ctx->runtime, i, &tool) == 0 &&
			    runtime_tool_flags(ctx->runtime, i, &flags) == 0 &&
			    (flags & TOOL_FLAG_READONLY))
				printf(ctx, " %s", tool.name);
		printf(ctx, "\n");
	}
	printf(ctx, ANSI_BOLD "[context]" ANSI_RESET "\n");
	printf(ctx, "  threshold = %.1f\n",
	       (*runtime_config_get(ctx->runtime)).context.summarize_threshold_ratio);
	printf(ctx, "  target = %.1f\n",
	       (*runtime_config_get(ctx->runtime)).context.compress_target_ratio);
	printf(ctx, "  keep_rounds = %d\n",
	       (*runtime_config_get(ctx->runtime)).context.keep_recent_rounds);
	printf(ctx, ANSI_BOLD "[memory]" ANSI_RESET "\n");
	printf(ctx, "  enabled = %d\n", (*runtime_config_get(ctx->runtime)).memory.enabled);
	printf(ctx, "  hot_path_enabled = %d\n",
	       (*runtime_config_get(ctx->runtime)).memory.hot_path_enabled);
	printf(ctx, "  cold_path_enabled = %d\n",
	       (*runtime_config_get(ctx->runtime)).memory.cold_path_enabled);
	printf(ctx, "  llm_extract_enabled = %d\n",
	       (*runtime_config_get(ctx->runtime)).memory.llm_extract_enabled);
	printf(ctx, "  max_facts = %d\n", (*runtime_config_get(ctx->runtime)).memory.max_facts);
	printf(ctx, "  max_episodes = %d\n",
	       (*runtime_config_get(ctx->runtime)).memory.max_episodes);
	printf(ctx, "  max_procedures = %d\n",
	       (*runtime_config_get(ctx->runtime)).memory.max_procedures);
	printf(ctx, "  max_context_chars = %d\n",
	       (*runtime_config_get(ctx->runtime)).memory.max_context_chars);
	printf(ctx, ANSI_BOLD "[render]" ANSI_RESET "\n");
	printf(ctx, "  prefer_image_protocol = %s\n",
	       (*runtime_config_get(ctx->runtime)).render.prefer_image_protocol);
	printf(ctx, "  mpv_args = %s\n", (*runtime_config_get(ctx->runtime)).render.mpv_args);
	printf(ctx, ANSI_BOLD "[ext]" ANSI_RESET "\n");
	printf(ctx, "  dir = %s\n", (*runtime_config_get(ctx->runtime)).ext.dir);
	printf(ctx, "  default_max_memory_mb = %d\n",
	       (*runtime_config_get(ctx->runtime)).ext.default_max_memory_mb);
	printf(ctx, "  default_max_cpu_seconds = %d\n",
	       (*runtime_config_get(ctx->runtime)).ext.default_max_cpu_seconds);
	printf(ctx, ANSI_BOLD "[prompt]" ANSI_RESET "\n");
	printf(ctx, "  system_prompt_file = %s\n",
	       (*runtime_config_get(ctx->runtime)).prompt.system_prompt_file);
	printf(ctx, "  system_prompt_dir = %s\n",
	       (*runtime_config_get(ctx->runtime)).prompt.system_prompt_dir);
	printf(ctx, ANSI_BOLD "[skill]" ANSI_RESET "\n");
	printf(ctx, "  dir = %s\n", (*runtime_config_get(ctx->runtime)).skill.dir);
	printf(ctx, ANSI_BOLD "[mcp]" ANSI_RESET "\n");
	printf(ctx, "  server_count = %d\n", (*runtime_config_get(ctx->runtime)).mcp.server_count);
	for (int i = 0; i < (*runtime_config_get(ctx->runtime)).mcp.server_count; i++) {
		const struct config_mcp_server *s = &(*runtime_config_get(ctx->runtime)).mcp.servers[i];
		printf(ctx, "  [[mcp.servers.%d]]\n", i);
		printf(ctx, "    name = %s\n", s->name);
		printf(ctx, "    transport = %s\n", s->transport);
		if (strcmp(s->transport, "stdio") == 0) {
			printf(ctx, "    command = %s\n", s->command);
			printf(ctx, "    args_count = %d\n", s->args_count);
		} else {
			printf(ctx, "    http_url = %s\n", s->http_url);
		}
		printf(ctx, "    auto_connect = %d\n", s->auto_connect);
	}
	return 0;
}
static int cmd_export_alias(struct cli_context *ctx, int argc, char **argv)
{
	(void)ctx;
	(void)argc;
	(void)argv;
	printf(ctx, "use /save [format] instead\n");
	return 0;
}

static int cmd_clear(struct cli_context *ctx, int argc, char **argv)
{
	(void)ctx;
	(void)argc;
	(void)argv;
	printf(ctx, "\033[2J\033[H");
	fflush(stdout);
	return 0;
}


static int cmd_recap(struct cli_context *ctx, int argc, char **argv)
{
	(void)argv;
	if (argc != 1) {
		CMD_ERROR(ctx, "usage: /recap");
		MORPH_RETURN(-EINVAL);
	}
	return cli_recap_start(ctx, 0);
}

static const struct cli_command runtime_commands[] = {
	{ "/quit",    cmd_quit,    "Exit the program",                  "" },
	{ "/q",       cmd_quit,    "Alias for /quit",                   "" },
	{ "/help",    cmd_help,    "Show help for commands",            "/help [command]" },
	{ "/h",       cmd_help,    "Alias for /help",                   "/h [command]" },
	{ "/model",   cmd_model,   "View or switch the LLM model",      "/model [name]" },
	{ "/m",       cmd_model,   "Alias for /model",                  "/m [name]" },
	{ "/trace",   cmd_trace,   "Show ReAct trace for current turn", "/trace [--from-db]" },
	{ "/t",       cmd_trace,   "Alias for /trace",                  "/t [--from-db]" },
	{ "/context", cmd_context, "Show token usage and context info", "/context" },
	{ "/ctx",     cmd_context, "Alias for /context",                "/ctx" },
	{ "/credits", cmd_credits, "Show credit usage and estimated cost", "/credits" },
	{ "/compress",cmd_compress,"Manually compress context window",  "/compress" },
	{ "/recap",   cmd_recap,   "Summarize the current conversation", "/recap" },
	{ "/cp",      cmd_compress,"Alias for /compress",               "/cp" },
	{ "/save",    cmd_save,    "Export session to a file",          "/save [format]" },
	{ "/config",  cmd_config,  "View current configuration",        "/config" },
	{ "/cfg",     cmd_config,  "Alias for /config",                 "/cfg" },
	{ "/export",  cmd_export_alias, "Alias for /save",              "/export <format>" },
	{ "/clear",   cmd_clear,   "Clear the terminal screen",         "/clear" },
	{ "/cl",      cmd_clear,   "Alias for /clear",                  "/cl" },
};

struct cli_command_group cli_runtime_commands(void)
{
	return (struct cli_command_group){
		.entries = runtime_commands,
		.count = (int)(sizeof(runtime_commands) / sizeof(runtime_commands[0])),
		.category = "Core",
	};
}
