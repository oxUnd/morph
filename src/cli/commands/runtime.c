#include "cli/commands/registry.h"

static int cmd_quit(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	ctx->running = 0;
	CMD_OK("Goodbye!");
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
			CMD_ERROR("unknown command: %s", topic);
			return -ENOENT;
		}
		printf(ANSI_BOLD "  %s" ANSI_RESET " — %s\n", e->name, e->desc);
		if (e->usage && *e->usage)
			printf("  " ANSI_DIM "usage: %s" ANSI_RESET "\n", e->usage);
		return 0;
	}
	cli_print_help();
	return 0;
}

static int cmd_model(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cli_cmd_arg(argc, argv, 1);
	if (name) {
		strncpy(ctx->current_session.model, name,
			sizeof(ctx->current_session.model) - 1);
		ctx->current_session.model[sizeof(ctx->current_session.model) - 1] = '\0';
		session_update_model(&ctx->database, ctx->current_session.id, name);
		if (ctx->llm) {
			strncpy(ctx->llm->model_id, name,
				sizeof(ctx->llm->model_id) - 1);
			ctx->llm->model_id[sizeof(ctx->llm->model_id) - 1] = '\0';
		}
		CMD_OK("model switched to: %s", name);
	} else {
		printf("current model: %s\n", ctx->current_session.model);
	}
	return 0;
}

static void print_trace_steps(struct react_step *steps, int count, const char *state_name)
{
	struct react_step *cur = steps;
	int step = 1;
	while (cur) {
		const char *color = "";
		switch (cur->type) {
		case REACT_STEP_THOUGHT:	color = ANSI_DIM; break;
		case REACT_STEP_ACTION:		color = ANSI_YELLOW; break;
		case REACT_STEP_OBSERVATION:	color = ANSI_DIM; break;
		case REACT_STEP_REFLECTION:	color = ANSI_CYAN; break;
		case REACT_STEP_FINAL:		color = ANSI_GREEN; break;
		case REACT_STEP_REASONING:	color = ANSI_DIM; break;
		default:			color = ""; break;
		}
		printf("  %d. %s[%s]%s", step, color, react_step_type_name(cur->type), ANSI_RESET);
		if (cur->content)
			printf(" %s", cur->content);
		if (cur->tool_name)
			printf(ANSI_DIM " (tool: %s)" ANSI_RESET, cur->tool_name);
		printf("\n");
		cur = cur->next;
		step++;
	}
	printf(ANSI_DIM "state: %s, steps: %d" ANSI_RESET "\n",
	       state_name ? state_name : "n/a", count);
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
		char *json = trace_load_latest(&ctx->database, ctx->current_session.id,
					      &round_no, &aborted);
		if (!json) {
			printf("no traces saved in DB for this session\n");
			return 0;
		}
		CMD_HEADER("ReAct trace (round %d, %s)", round_no,
			   aborted ? "aborted" : "completed");
		int count = 0;
		struct arena *arena = arena_create(64 * 1024);
		struct react_step *steps = json_to_react_steps(arena, json, &count);
		if (steps) {
			print_trace_steps(steps, count, aborted ? "ABORT" : "DONE");
		} else {
			printf("  (raw) %s\n", json);
		}
		arena_destroy(arena);
		free(json);
		return 0;
	}
	if (!ctx->react || !ctx->react->steps) {
		printf("no ReAct trace for current turn\n");
		return 0;
	}
	CMD_HEADER("ReAct trace (%s)", ctx->current_session.name);
	print_trace_steps(ctx->react->steps, ctx->react->step_count,
			  react_state_name(ctx->react->state));
	return 0;
}

static int cmd_credits(struct cli_context *ctx, int argc, char **argv)
{
	struct credit_summary today;
	struct credit_summary session;
	char sid[64];
	int rc;

	(void)argc;
	(void)argv;
	cli_credit_session_key(ctx, sid, sizeof(sid));
	rc = credit_summary_today(&ctx->database, "local", &today);
	if (rc != 0)
		return rc;
	rc = credit_summary_session(&ctx->database, sid, &session);
	if (rc != 0)
		return rc;

	printf("credits: %lld",
	       (long long)today.credits);
	if (ctx->config.credits.daily_limit >= 0) {
		printf(" / %d today", ctx->config.credits.daily_limit);
		if (today.credits > ctx->config.credits.daily_limit)
			printf(" " ANSI_YELLOW "(over limit)" ANSI_RESET);
	} else {
		printf(" today");
	}
	printf(" | est. cost: %.6f %s | session: %lld credits",
	       today.estimated_cost,
	       ctx->config.credits.currency,
	       (long long)session.credits);
	if (session.event_count == 0)
		printf(" | no events");
	printf("\n");
	return 0;
}

static int cmd_context(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int msg_count = message_count(&ctx->database, ctx->current_session.id);
	int total_tokens = 0;
	int limit = ctx->tokenizer ? ctx->tokenizer->context_limit : 0;
	struct message *msgs = message_list(&ctx->database,
					    ctx->current_session.id, &msg_count);
	struct message *cur = msgs;
	while (cur) {
		total_tokens += cur->token_count;
		cur = cur->next;
	}
	message_free_list(msgs);
	double pct = limit > 0 ? (double)total_tokens / limit * 100.0 : 0.0;
	printf("context: %s%d / %d tokens (%.1f%%)%s | messages: %d\n",
	       pct >= 80.0 ? ANSI_YELLOW : "",
	       total_tokens, limit, pct,
	       pct >= 80.0 ? ANSI_RESET : "",
	       msg_count);
	return 0;
}

static int cmd_compress(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int count = 0;
	struct message *msgs = message_list(&ctx->database,
					    ctx->current_session.id, &count);
	if (!msgs || count == 0) {
		printf("no messages to compress\n");
		message_free_list(msgs);
		return 0;
	}
	int keep = ctx->config.context.keep_recent_rounds * 2;
	if (count <= keep) {
		CMD_OK("only %d messages, no compression needed (keep %d)", count, keep);
		message_free_list(msgs);
		return 0;
	}

	/* Build an in-memory list mirroring DB rows so we can apply the
	 * layered compression fallback (REQUIREMENTS §6.3): react_trace →
	 * sliding_window. Track which DB ids survive; everything else is
	 * deleted from the DB. */
	struct message_list *head = NULL;
	struct arena *cmp_arena = arena_create(0);
	int *ids = calloc((size_t)count, sizeof(*ids));
	if (!cmp_arena || !ids) {
		message_free_list(msgs);
		if (cmp_arena) arena_destroy(cmp_arena);
		free(ids);
		return -ENOMEM;
	}
	int n_ids = 0;
	for (struct message *m = msgs; m; m = m->next) {
		struct message_list *node = msg_list_create(cmp_arena, m->role, m->content,
							    m->token_count);
		if (!node)
			continue;
		node->compressed = m->compressed;
		msg_list_append(&head, node);
		ids[n_ids++] = (int)m->id;
	}
	message_free_list(msgs);

	struct compress_result trace_res = {0};
	(void)compress_react_trace(&head, &trace_res);
	struct compress_result win_res = {0};
	int rc = compress_sliding_window(&head,
		ctx->config.context.keep_recent_rounds, &win_res);
	if (rc < 0) {
		arena_destroy(cmp_arena);
		free(ids);
		CMD_ERROR("compression failed: %s", morph_strerror(rc));
		return rc;
	}

	/* Compute survivors by walking remaining list and matching content
	 * with the original DB rows (preserving order).  Entries from the
	 * tail of `ids` correspond to the last messages, which sliding_window
	 * keeps; we delete the prefix that was dropped. */
	int kept = msg_list_count(head);
	int removed = count - kept;
	for (int i = 0; i < removed; i++) {
		/* Best-effort: delete by id; keep going on failure. */
		(void)message_delete(&ctx->database, ids[i]);
	}
	msg_list_destroy(head);
	arena_destroy(cmp_arena);
	free(ids);

	/* Refresh in-memory react context from DB. */
	session_load_history(ctx);

	CMD_OK("compressed: react_trace removed %d, sliding_window removed %d, kept %d",
	       trace_res.messages_removed, win_res.messages_removed, kept);
	return 0;
}

static int cmd_save(struct cli_context *ctx, int argc, char **argv)
{
	const char *fmt = cli_cmd_arg(argc, argv, 1);
	if (!fmt)
		fmt = "md";
	int count = 0;
	struct message *msgs = message_list(&ctx->database,
					     ctx->current_session.id, &count);
	char filename[PATH_MAX];
	snprintf(filename, sizeof(filename), "%s_%lld.%s",
		 ctx->current_session.name,
		 (long long)time(NULL), fmt);
	CMD_HEADER("saving session to %s", filename);
	FILE *f = fopen(filename, "w");
	if (f) {
		fprintf(f, "# Session: %s\n\n", ctx->current_session.name);
		struct message *cur = msgs;
		while (cur) {
			fprintf(f, "**%s**: %s\n\n", cur->role,
				cur->content ? cur->content : "");
			cur = cur->next;
		}
		fclose(f);
		CMD_OK("saved %d messages", count);
	} else {
		CMD_ERROR("failed to open file for writing");
	}
	message_free_list(msgs);
	return 0;
}

static int cmd_config(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf(ANSI_BOLD "[general]" ANSI_RESET "\n");
	printf("  default_session = %s\n", ctx->config.general.default_session);
	printf("  output_dir = %s\n", ctx->config.general.output_dir);
	printf("  log_level = %s\n", ctx->config.general.log_level);
	printf("  log_file = %s\n", ctx->config.general.log_file);
	printf(ANSI_BOLD "[model.text]" ANSI_RESET "\n");
	printf("  provider = %s\n", ctx->config.models.text.provider);
	printf("  model = %s\n", ctx->config.models.text.model);
	printf("  api_base = %s\n", ctx->config.models.text.api_base);
	printf("  context_limit = %d\n", ctx->config.models.text.context_limit);
	printf("  max_tokens = %d\n", ctx->config.models.text.max_tokens);
	printf("  timeout_seconds = %d\n",
	       ctx->config.models.text.timeout_seconds);
	printf(ANSI_BOLD "[model.image]" ANSI_RESET "\n");
	printf("  provider = %s\n", ctx->config.models.image.provider);
	printf("  model = %s\n", ctx->config.models.image.model);
	printf("  api_base = %s\n", ctx->config.models.image.api_base);
	printf("  context_limit = %d\n",
	       ctx->config.models.image.context_limit);
	printf(ANSI_BOLD "[model.video]" ANSI_RESET "\n");
	printf("  provider = %s\n", ctx->config.models.video.provider);
	printf("  model = %s\n", ctx->config.models.video.model);
	printf("  api_base = %s\n", ctx->config.models.video.api_base);
	printf("  context_limit = %d\n",
	       ctx->config.models.video.context_limit);
	printf(ANSI_BOLD "[credits]" ANSI_RESET "\n");
	printf("  daily_limit = %d\n", ctx->config.credits.daily_limit);
	printf("  currency = %s\n", ctx->config.credits.currency);
	printf("  cost_to_credit_coef = %.3f\n",
	       ctx->config.credits.cost_to_credit_coef);
	printf("  input_token_credit_coef = %.6f\n",
	       ctx->config.credits.input_token_credit_coef);
	printf("  output_token_credit_coef = %.6f\n",
	       ctx->config.credits.output_token_credit_coef);
	printf("  image_unit_credit_coef = %.6f\n",
	       ctx->config.credits.image_unit_credit_coef);
	printf("  video_second_credit_coef = %.6f\n",
	       ctx->config.credits.video_second_credit_coef);
	printf("  prices = %d\n", ctx->config.credits.price_count);
	printf(ANSI_BOLD "[react]" ANSI_RESET "\n");
	printf("  max_iterations = %d\n", ctx->config.react.max_iterations);
	printf("  tool_timeout = %d\n",
	       ctx->config.react.tool_timeout_seconds);
	printf("  tool_max_retries = %d\n", ctx->config.react.tool_max_retries);
	printf("  guardrail_enabled = %d\n",
	       ctx->config.react.guardrail_enabled);
	printf("  guardrail_max_retries = %d\n",
	       ctx->config.react.guardrail_max_retries);
	printf("  guardrail_max_empty_rounds = %d\n",
	       ctx->config.react.guardrail_max_empty_rounds);
	printf("  guardrail_llm_model = %s\n",
	       ctx->config.react.guardrail_llm_model);
	printf("  hitl_enabled = %d\n", ctx->config.react.hitl_enabled);
	printf("  hitl_auto_approve_readonly = %d\n",
	       ctx->config.react.hitl_auto_approve_readonly);
	printf("  bash_exec_enabled = %d\n",
	       ctx->config.react.bash_exec_enabled);
	printf("  bash_exec_default_timeout = %d\n",
	       ctx->config.react.bash_exec_default_timeout);
	if (ctx->config.react.disabled_tools_count > 0) {
		printf("  disabled_tools =");
		for (int i = 0; i < ctx->config.react.disabled_tools_count; i++)
			printf(" %s", ctx->config.react.disabled_tools[i]);
		printf("\n");
	}
	if (ctx->config.react.hitl_tools_count > 0) {
		printf("  hitl_tools =");
		for (int i = 0; i < ctx->config.react.hitl_tools_count; i++)
			printf(" %s", ctx->config.react.hitl_tools[i]);
		printf("\n");
	}
	{
		int ro = 0, rw = 0;
		for (int i = 0; i < ctx->tools.count; i++)
			if (ctx->tools.entries[i].flags & TOOL_FLAG_READONLY)
				ro++;
			else
				rw++;
		printf(ANSI_BOLD "[tools]" ANSI_RESET " %d registered"
		       " (" ANSI_GREEN "%d readonly" ANSI_RESET ", %d read-write)\n",
		       ctx->tools.count, ro, rw);
		printf("  readonly:");
		for (int i = 0; i < ctx->tools.count; i++)
			if (ctx->tools.entries[i].flags & TOOL_FLAG_READONLY)
				printf(" %s", ctx->tools.entries[i].desc.name);
		printf("\n");
	}
	if (ctx->config.react.bash_exec_allowed_commands_count > 0) {
		printf("  bash_exec_allowed_commands =");
		for (int i = 0;
		     i < ctx->config.react.bash_exec_allowed_commands_count;
		     i++)
			printf(" %s",
			       ctx->config.react.bash_exec_allowed_commands[i]);
		printf("\n");
	}
	if (ctx->config.react.bash_exec_allowed_cwds_count > 0) {
		printf("  bash_exec_allowed_cwds =");
		for (int i = 0;
		     i < ctx->config.react.bash_exec_allowed_cwds_count; i++)
			printf(" %s",
			       ctx->config.react.bash_exec_allowed_cwds[i]);
		printf("\n");
	}
	printf(ANSI_BOLD "[context]" ANSI_RESET "\n");
	printf("  threshold = %.1f\n",
	       ctx->config.context.summarize_threshold_ratio);
	printf("  target = %.1f\n",
	       ctx->config.context.compress_target_ratio);
	printf("  keep_rounds = %d\n",
	       ctx->config.context.keep_recent_rounds);
	printf(ANSI_BOLD "[memory]" ANSI_RESET "\n");
	printf("  enabled = %d\n", ctx->config.memory.enabled);
	printf("  hot_path_enabled = %d\n",
	       ctx->config.memory.hot_path_enabled);
	printf("  cold_path_enabled = %d\n",
	       ctx->config.memory.cold_path_enabled);
	printf("  llm_extract_enabled = %d\n",
	       ctx->config.memory.llm_extract_enabled);
	printf("  max_facts = %d\n", ctx->config.memory.max_facts);
	printf("  max_episodes = %d\n", ctx->config.memory.max_episodes);
	printf("  max_procedures = %d\n", ctx->config.memory.max_procedures);
	printf("  max_context_chars = %d\n",
	       ctx->config.memory.max_context_chars);
	printf(ANSI_BOLD "[render]" ANSI_RESET "\n");
	printf("  prefer_image_protocol = %s\n",
	       ctx->config.render.prefer_image_protocol);
	printf("  mpv_args = %s\n", ctx->config.render.mpv_args);
	printf(ANSI_BOLD "[ext]" ANSI_RESET "\n");
	printf("  dir = %s\n", ctx->config.ext.dir);
	printf("  default_max_memory_mb = %d\n",
	       ctx->config.ext.default_max_memory_mb);
	printf("  default_max_cpu_seconds = %d\n",
	       ctx->config.ext.default_max_cpu_seconds);
	printf(ANSI_BOLD "[prompt]" ANSI_RESET "\n");
	printf("  system_prompt_file = %s\n",
	       ctx->config.prompt.system_prompt_file);
	printf("  system_prompt_dir = %s\n",
	       ctx->config.prompt.system_prompt_dir);
	printf(ANSI_BOLD "[skill]" ANSI_RESET "\n");
	printf("  dir = %s\n", ctx->config.skill.dir);
	printf(ANSI_BOLD "[mcp]" ANSI_RESET "\n");
	printf("  server_count = %d\n", ctx->config.mcp.server_count);
	for (int i = 0; i < ctx->config.mcp.server_count; i++) {
		struct config_mcp_server *s = &ctx->config.mcp.servers[i];
		printf("  [[mcp.servers.%d]]\n", i);
		printf("    name = %s\n", s->name);
		printf("    transport = %s\n", s->transport);
		if (strcmp(s->transport, "stdio") == 0) {
			printf("    command = %s\n", s->command);
			printf("    args_count = %d\n", s->args_count);
		} else {
			printf("    http_url = %s\n", s->http_url);
		}
		printf("    auto_connect = %d\n", s->auto_connect);
	}
	return 0;
}
static int cmd_export_alias(struct cli_context *ctx, int argc, char **argv)
{
	(void)ctx;
	(void)argc;
	(void)argv;
	printf("use /save [format] instead\n");
	return 0;
}

static int cmd_clear(struct cli_context *ctx, int argc, char **argv)
{
	(void)ctx;
	(void)argc;
	(void)argv;
	printf("\033[2J\033[H");
	fflush(stdout);
	return 0;
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
	{ "/cp",      cmd_compress,"Alias for /compress",               "/cp" },
	{ "/save",    cmd_save,    "Export session to a file",          "/save [format]" },
	{ "/config",  cmd_config,  "View current configuration",        "/config" },
	{ "/cfg",     cmd_config,  "Alias for /config",                 "/cfg" },
	{ "/export",  cmd_export_alias, "Alias for /save",              "/export <format>" },
	{ "/clear",   cmd_clear,   "Clear the terminal screen",         "/clear" },
	{ "/cl",      cmd_clear,   "Alias for /clear",                  "/cl" },
};

int cli_register_runtime_commands(void)
{
	return cli_command_register_many(runtime_commands,
		(int)(sizeof(runtime_commands) / sizeof(runtime_commands[0])));
}
