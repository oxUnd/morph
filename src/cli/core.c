#include "cli/internal.h"
#include "cli/commands/registry.h"
#include "agent/tools/dynamic_tools.h"
#include <stdarg.h>

const char *default_db_path = "~/.morph/data.db";
const char *default_config_path = "~/.morph/config.toml";
static struct cli_context *g_cli_usage_ctx;
static int g_cli_color_enabled = 1;

void cli_set_color_enabled(int enabled)
{
	g_cli_color_enabled = enabled ? 1 : 0;
}

int cli_color_enabled(void)
{
	return g_cli_color_enabled;
}

static size_t cli_strip_ansi_into(char *dst, size_t dst_size,
				  const char *src)
{
	size_t out = 0;
	const unsigned char *p = (const unsigned char *)src;

	if (!src)
		return 0;
	while (*p) {
		if (*p == 0x1b) {
			p++;
			if (*p == '[') {
				p++;
				while (*p && (*p < 0x40 || *p > 0x7e))
					p++;
				if (*p)
					p++;
				continue;
			}
			if (*p)
				p++;
			continue;
		}
		if (dst && dst_size > 0 && out + 1 < dst_size)
			dst[out] = (char)*p;
		out++;
		p++;
	}
	if (dst && dst_size > 0) {
		size_t term = out < dst_size ? out : dst_size - 1;
		dst[term] = '\0';
	}
	return out;
}

int cli_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (g_cli_color_enabled) {
		va_start(ap, fmt);
		n = vfprintf(stdout, fmt, ap);
		va_end(ap);
		return n;
	}

	va_start(ap, fmt);
	va_list ap_size;
	va_copy(ap_size, ap);
	n = vsnprintf(NULL, 0, fmt, ap_size);
	va_end(ap_size);
	if (n < 0) {
		va_end(ap);
		return n;
	}
	char *buf = malloc((size_t)n + 1);
	if (!buf) {
		va_end(ap);
		return -1;
	}
	vsnprintf(buf, (size_t)n + 1, fmt, ap);
	va_end(ap);
	size_t stripped_len = cli_strip_ansi_into(NULL, 0, buf);
	char *stripped = malloc(stripped_len + 1);
	if (!stripped) {
		free(buf);
		return -1;
	}
	cli_strip_ansi_into(stripped, stripped_len + 1, buf);
	free(buf);
	fputs(stripped, stdout);
	free(stripped);
	return n;
}

static int cli_turn_background_cb(void *user_data, const char *name,
				  const char *phase, const char *message,
				  const char *task, int count,
				  int error_code)
{
	struct cli_context *ctx = user_data;

	return cli_emit_background_event(ctx, name, phase, message, task,
					 count, error_code);
}

/* Load stored messages from DB into react context.
 * Clears any existing in-memory messages first. */
void session_load_history(struct cli_context *ctx)
{
	struct agent_session_runtime runtime;

	if (!ctx || !ctx->react)
		return;
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &ctx->database;
	runtime.session_id = ctx->current_session.id;
	runtime.react = ctx->react;
	agent_session_load_history(&runtime);
}

struct memory_options cli_memory_options(const struct cli_context *ctx)
{
	struct memory_options opts;

	memset(&opts, 0, sizeof(opts));
	if (!ctx)
		return opts;
	opts.enabled = ctx->config.memory.enabled;
	opts.hot_path_enabled = ctx->config.memory.hot_path_enabled;
	opts.cold_path_enabled = ctx->config.memory.cold_path_enabled;
	opts.llm_extract_enabled = ctx->config.memory.llm_extract_enabled;
	opts.max_facts = ctx->config.memory.max_facts;
	opts.max_episodes = ctx->config.memory.max_episodes;
	opts.max_procedures = ctx->config.memory.max_procedures;
	opts.max_context_chars = ctx->config.memory.max_context_chars;
	return opts;
}

void print_padded(const char *s, int target_width)
{
	size_t width;
	int dw;
	int pad;

	if (!s) s = "";
	width = utf8_display_width(s);
	dw = width > (size_t)INT_MAX ? INT_MAX : (int)width;
	fputs(s, stdout);
	pad = target_width - dw;
	for (int i = 0; i < pad; i++)
		putchar(' ');
}

void cli_credit_session_key(struct cli_context *ctx, char *buf,
					   size_t size)
{
	if (!ctx || !buf || size == 0)
		return;
	if (ctx->current_session.display_id[0]) {
		snprintf(buf, size, "%s", ctx->current_session.display_id);
		return;
	}
	snprintf(buf, size, "%lld", (long long)ctx->current_session.id);
}

void cli_update_tool_runtime_context(struct cli_context *ctx)
{
	struct tool_runtime_context rt;
	const char *session_id;

	if (!ctx || !ctx->react)
		return;
	memset(&rt, 0, sizeof(rt));
	rt.db = &ctx->database;
	rt.config = &ctx->config;
	rt.user_id = "local";
	session_id = ctx->current_session.display_id[0]
		? ctx->current_session.display_id
		: ctx->current_session.name;
	rt.credit_session_id = session_id;
	rt.memory_session_id = ctx->current_session.id;
	rt.restrict_memory_to_user = 0;
	react_set_tool_runtime_context(ctx->react, &rt);
	(void)dynamic_tools_set_session_id(&ctx->tools, session_id);
}

void cli_set_usage_context(struct cli_context *ctx)
{
	g_cli_usage_ctx = ctx;
}

static char *cli_model_usage_metadata(const struct model_usage *usage)
{
	cJSON *obj;
	char *json;

	if (!usage)
		return NULL;
	obj = cJSON_CreateObject();
	if (!obj)
		return NULL;
	if (usage->response_id[0])
		cJSON_AddStringToObject(obj, "response_id",
					usage->response_id);
	if (usage->model[0])
		cJSON_AddStringToObject(obj, "actual_model", usage->model);
	if (usage->finish_reason[0])
		cJSON_AddStringToObject(obj, "finish_reason",
					usage->finish_reason);
	if (usage->system_fingerprint[0])
		cJSON_AddStringToObject(obj, "system_fingerprint",
					usage->system_fingerprint);
	if (usage->usage_source[0])
		cJSON_AddStringToObject(obj, "usage_source",
					usage->usage_source);
	if (usage->created > 0)
		cJSON_AddNumberToObject(obj, "created",
					(double)usage->created);
	if (usage->total_tokens > 0)
		cJSON_AddNumberToObject(obj, "total_tokens",
					(double)usage->total_tokens);
	if (usage->cached_tokens > 0)
		cJSON_AddNumberToObject(obj, "cached_tokens",
					(double)usage->cached_tokens);
	if (usage->reasoning_tokens > 0)
		cJSON_AddNumberToObject(obj, "reasoning_tokens",
					(double)usage->reasoning_tokens);
	if (usage->audio_tokens > 0)
		cJSON_AddNumberToObject(obj, "audio_tokens",
					(double)usage->audio_tokens);
	if (usage->image_tokens > 0)
		cJSON_AddNumberToObject(obj, "image_tokens",
					(double)usage->image_tokens);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	return json;
}

void cli_record_model_usage(const struct model_usage *usage,
				   void *user_data)
{
	struct cli_context *ctx = user_data ? user_data : g_cli_usage_ctx;
	struct credit_event event;
	struct credit_summary today;
	char sid[64];
	char *metadata;
	int rc;

	if (!ctx || !usage)
		return;
	if (usage->input_tokens <= 0 && usage->output_tokens <= 0 &&
	    usage->image_units <= 0 && usage->video_seconds <= 0)
		return;
	cli_credit_session_key(ctx, sid, sizeof(sid));
	metadata = cli_model_usage_metadata(usage);
	memset(&event, 0, sizeof(event));
	event.user_id = "local";
	event.session_id = sid;
	event.kind = usage->kind[0] ? usage->kind : "model_text";
	event.provider = usage->provider[0] ? usage->provider :
		ctx->config.models.text.provider;
	event.model = usage->model[0] ? usage->model :
		ctx->config.models.text.model;
	event.input_tokens = usage->input_tokens;
	event.output_tokens = usage->output_tokens;
	event.image_units = usage->image_units;
	event.video_seconds = usage->video_seconds;
	event.metadata_json = metadata;
	rc = credit_record_event(&ctx->database, &ctx->config.credits,
				 &event, NULL);
	free(metadata);
	if (rc != 0 || ctx->config.credits.daily_limit < 0 ||
	    ctx->event_mode == CLI_EVENTS_JSON)
		return;
	rc = credit_summary_today(&ctx->database, "local", &today);
	if (rc == 0 && today.credits > ctx->config.credits.daily_limit) {
		printf(ANSI_YELLOW
		       "credits warning: %lld / %d today"
		       ANSI_RESET "\n",
		       (long long)today.credits,
		       ctx->config.credits.daily_limit);
	}
}

void cli_record_media_credits(struct cli_context *ctx, const char *kind,
				     int64_t image_units,
				     int64_t video_seconds,
				     const char *provider,
				     const char *model,
				     const char *metadata_json)
{
	struct credit_event event;
	struct credit_summary today;
	char sid[64];
	int rc;

	if (!ctx || !kind)
		return;
	cli_credit_session_key(ctx, sid, sizeof(sid));
	memset(&event, 0, sizeof(event));
	event.user_id = "local";
	event.session_id = sid;
	event.kind = kind;
	event.provider = provider;
	event.model = model;
	event.image_units = image_units;
	event.video_seconds = video_seconds;
	event.metadata_json = metadata_json;
	rc = credit_record_event(&ctx->database, &ctx->config.credits,
				 &event, NULL);
	if (rc != 0 || ctx->config.credits.daily_limit < 0 ||
	    ctx->event_mode == CLI_EVENTS_JSON)
		return;
	rc = credit_summary_today(&ctx->database, "local", &today);
	if (rc == 0 && today.credits > ctx->config.credits.daily_limit) {
		printf(ANSI_YELLOW
		       "credits warning: %lld / %d today"
		       ANSI_RESET "\n",
		       (long long)today.credits,
		       ctx->config.credits.daily_limit);
	}
}
int cli_handle_command(struct cli_context *ctx, const char *input)
{
	int64_t command_started_at;

	if (!ctx || !input)
		return -EINVAL;
	command_started_at = (int64_t)time(NULL);
	(void)scheduled_tasks_tool_set_time_anchor(&ctx->tools,
						   command_started_at);
	(void)scheduled_tasks_tool_set_source_session(&ctx->tools,
						      ctx->current_session.id);

	cli_process_due_tasks(ctx);

	if (input[0] == '/') {
		int rc = cli_command_dispatch(ctx, input);
		cli_process_due_tasks(ctx);
		return rc;
	}

	char input_buf[8192];
	const char *effective_input = input;
	if (ctx->image_path[0]) {
		int n = snprintf(input_buf, sizeof(input_buf),
				 "[Image: %s]\n%s", ctx->image_path, input);
		if (n > 0 && (size_t)n < sizeof(input_buf))
			effective_input = input_buf;
		ctx->image_path[0] = '\0';
	}

	/* Auto-name session from first user input */
	if (!ctx->session_auto_named && input[0] != '/') {
		char title[48];
		size_t len = strlen(input);
		size_t max_bytes = sizeof(title) - 4;
		if (len > max_bytes) {
			size_t chop = utf8_clamp_bytes(input, max_bytes);
			memcpy(title, input, chop);
			title[chop] = '\0';
			strcat(title, "...");
		} else {
			memcpy(title, input, len);
			title[len] = '\0';
		}
		session_rename(&ctx->database, ctx->current_session.id, title);
		strncpy(ctx->current_session.name, title,
			sizeof(ctx->current_session.name) - 1);
		ctx->session_auto_named = 1;
	}

	if (ctx->event_mode != CLI_EVENTS_JSON) {
		printf(ANSI_BOLD ANSI_CYAN "▸ %s" ANSI_RESET "\n", input);
		fflush(stdout);
	}

	cli_sigint_received = 0;
	pthread_mutex_lock(&ctx->react_lock);
	if (ctx->react)
		react_cancel(ctx->react);

	struct memory_options mem_opts = cli_memory_options(ctx);
	struct agent_session_runtime runtime;
	struct agent_turn turn;
	struct agent_turn_input turn_input;

	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &ctx->database;
	runtime.session_id = ctx->current_session.id;
	runtime.react = ctx->react;
	runtime.memory_options = &mem_opts;
	runtime.background_cb = cli_turn_background_cb;
	runtime.background_user_data = ctx;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS |
		AGENT_TURN_SAVE_EMPTY_USER |
		AGENT_TURN_SAVE_EMPTY_ASSISTANT;
	memset(&turn_input, 0, sizeof(turn_input));
	turn_input.model_input = effective_input;
	int turn_rc = agent_turn_begin(&turn, &runtime, &turn_input);
	if (turn_rc != 0) {
		pthread_mutex_unlock(&ctx->react_lock);
		return turn_rc;
	}

	int react_rc = react_run(ctx->react, effective_input,
				 ctx->event_mode == CLI_EVENTS_JSON ?
				 NULL : output_callback, ctx);

	if (ctx->react && ctx->react->state == REACT_STATE_ABORT &&
	    ctx->event_mode != CLI_EVENTS_JSON) {
		const char *outcome = react_outcome_name(ctx->react->outcome);
		const char *error = react_rc < 0 ? morph_strerror(react_rc) :
			"aborted";
		printf(ANSI_YELLOW "[%s] %s" ANSI_RESET,
		       outcome, error);
		if (ctx->react->outcome_reason[0])
			printf(ANSI_DIM " (%s)" ANSI_RESET,
			       ctx->react->outcome_reason);
		printf("\n");
	}

	agent_turn_finish(&turn, NULL);
	ctx->streaming = 0;
	pthread_mutex_unlock(&ctx->react_lock);
	cli_process_due_tasks(ctx);
	return 0;
}

/* ---- cli_shutdown ---- */

void cli_shutdown(struct cli_context *ctx)
{
	int wait_for_memory;

	if (!ctx)
		return;
	/* Drain the memory async worker before tearing down the db so
	 * any in-flight consolidation job finishes against a live file. */
	wait_for_memory = memory_async_pending();
	if (wait_for_memory)
		cli_emit_background_event(ctx, "background.progress",
					  "progress",
					  "waiting for memory consolidation",
					  "memory_consolidation", -1, 0);
	if (wait_for_memory && ctx->event_mode != CLI_EVENTS_JSON) {
		printf(ANSI_DIM
		       "Saving memory summary before exit, please wait..."
		       ANSI_RESET "\n");
		fflush(stdout);
	}
	memory_async_shutdown();
	if (wait_for_memory)
		cli_emit_background_event(ctx, "background.completed", "end",
					  "memory consolidation drained",
					  "memory_consolidation", -1, 0);
	cli_scheduler_stop(ctx);
	if (ctx->react) {
		free(ctx->react->sub_agent_info);
		ctx->react->sub_agent_info = NULL;
		ctx->react->sub_agent_info_count = 0;
		react_context_destroy(ctx->react);
	}
	if (ctx->tokenizer)
		tokenizer_destroy(ctx->tokenizer);
	tool_registry_cleanup(&ctx->tools);
	if (ctx->tctx) {
		tool_context_destroy(ctx->tctx);
		ctx->tctx = NULL;
	}
	if (ctx->llm)
		model_destroy(ctx->llm);
	if (ctx->img_llm)
		model_destroy(ctx->img_llm);
	if (ctx->vid_llm)
		model_destroy(ctx->vid_llm);
	if (ctx->sub_agents) {
		sub_agent_runtime_destroy(ctx->sub_agents);
		ctx->sub_agents = NULL;
	}
	if (ctx->react_lock_ready) {
		pthread_mutex_destroy(&ctx->react_lock);
		ctx->react_lock_ready = 0;
	}
	skill_registry_cleanup(ctx->skills);
	free(ctx->skills);
	ctx->skills = NULL;
	mcp_registry_cleanup(&ctx->mcp);
	db_close(&ctx->database);
	log_info("cli shutdown complete");
}
