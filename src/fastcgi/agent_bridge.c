/* agent_bridge.c — strong-symbol implementations for optional
 * agent-side hooks (see PATCHES.md §3).
 *
 * Provides react_context_create_for_session:
 *   loads config, creates LLM model + tokenizer, populates message
 *   history from the session store, and returns a ready react_context.
 *
 * Linked into morph-fastcgi; overrides the weak symbols in turns.c.
 */
#include "react.h"
#include "session_store.h"
#include "session.h"
#include "agent/context.h"
#include "agent/tokenizer.h"
#include "agent/tool.h"
#include "agent/tools/text_gen.h"
#include "agent/tools/text_qa.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_edit.h"
#include "agent/tools/img_info.h"
#include "agent/tools/img_resize.h"
#include "agent/tools/img_convert.h"
#include "agent/tools/file_read.h"
#include "agent/tools/file_list.h"
#include "agent/tools/file_info.h"
#include "agent/tools/bash_exec.h"
#include "agent/tools/vid_gen.h"
#include "agent/tools/plan.h"
#include "agent/tools/ask_user.h"
#include "models/llm.h"
#include "config.h"
#include "util/log.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t  g_once    = PTHREAD_ONCE_INIT;
static struct config   g_config;
static struct tokenizer *g_tokenizer = NULL;
static struct model      *g_llm       = NULL;
static struct tool_registry g_tools;
static struct plan_registry g_plans;

static void bridge_init_once(void)
{
	config_set_defaults(&g_config);

	/* Try HOME/.morph/config.toml, fall back to cwd */
	const char *home = getenv("HOME");
	if (home) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/.morph/config.toml", home);
		config_load(&g_config, path);
	} else {
		config_load(&g_config, "config.toml");
	}

	/* Initialize logging from config, with fallback */
	{
		const char *lf = g_config.general.log_file;
		char log_path[512];
		if (lf && lf[0] == '/') {
			snprintf(log_path, sizeof(log_path), "%s", lf);
		} else {
			snprintf(log_path, sizeof(log_path),
				 "/var/lib/morph/log/fastcgi.log");
		}
		log_init(log_path, LOG_DEBUG);
		fprintf(stderr, "fcgi-bridge: log initialized to %s\n", log_path);
	}

	g_tokenizer = tokenizer_create(
		g_config.models.text.model,
		g_config.models.text.context_limit);

	if (!g_tokenizer) {
		fprintf(stderr, "fcgi-bridge: tokenizer_create failed\n");
		return;
	}

	/* Resolve API key from env var if needed */
	if (g_config.models.text.api_key_env[0] &&
	    !g_config.models.text.api_key[0]) {
		const char *env = getenv(g_config.models.text.api_key_env);
		if (env)
			snprintf(g_config.models.text.api_key,
				 sizeof(g_config.models.text.api_key),
				 "%s", env);
	}

	g_llm = model_llm_create(
		g_config.models.text.provider,
		g_config.models.text.model,
		g_config.models.text.api_base,
		g_config.models.text.api_key);

	if (!g_llm)
		fprintf(stderr, "fcgi-bridge: model_llm_create failed\n");

	/* Register tools — same set as cli.c */
	tool_registry_init(&g_tools);

	text_gen_init(&g_tools, g_llm);
	text_qa_init(&g_tools, g_llm);

	/* Image generation model */
	{
		const char *img_key = g_config.models.image.api_key[0]
			? g_config.models.image.api_key
			: getenv(g_config.models.image.api_key_env);
		struct model *img_m = model_llm_create(
			g_config.models.image.provider,
			g_config.models.image.model,
			g_config.models.image.api_base[0]
				? g_config.models.image.api_base : NULL,
			img_key ? img_key : "");
		if (img_m) {
			img_gen_init(&g_tools, img_m);
			img_edit_init(&g_tools, g_llm);
		}
	}

	img_info_init(&g_tools);
	img_resize_init(&g_tools);
	img_convert_init(&g_tools);

	file_read_init(&g_tools);
	file_list_init(&g_tools);
	file_info_init(&g_tools);

	if (g_config.react.bash_exec_enabled) {
		bash_exec_clear_allowlist();
		for (int i = 0;
		     i < g_config.react.bash_exec_allowed_commands_count; i++)
			bash_exec_allow_command(
				g_config.react.bash_exec_allowed_commands[i]);
		for (int i = 0;
		     i < g_config.react.bash_exec_allowed_cwds_count; i++)
			bash_exec_allow_cwd(
				g_config.react.bash_exec_allowed_cwds[i]);
		bash_exec_init(&g_tools);
		log_info("fcgi-bridge: bash_exec explicitly enabled");
	} else {
		log_info("fcgi-bridge: bash_exec disabled by default");
	}

	/* Video generation model */
	{
		const char *vid_key = g_config.models.video.api_key[0]
			? g_config.models.video.api_key
			: getenv(g_config.models.video.api_key_env);
		struct model *vid_m = model_llm_create(
			g_config.models.video.provider,
			g_config.models.video.model,
			g_config.models.video.api_base[0]
				? g_config.models.video.api_base : NULL,
			vid_key ? vid_key : "");
		if (vid_m)
			vid_gen_init(&g_tools, vid_m);
	}

	/* Apply disabled_tools from config */
	for (int i = 0; i < g_config.react.disabled_tools_count; i++)
		tool_disable(&g_tools, g_config.react.disabled_tools[i]);

	plan_registry_init(&g_plans);
	plan_tool_init(&g_tools, &g_plans, g_llm);
	ask_user_init(&g_tools, NULL, NULL);

	log_info("fcgi-bridge: registered %d tools", g_tools.count);
}

struct react_context *
react_context_create_for_session(struct session_store *store,
				 const char *session_id,
				 const char *user_id)
{
	(void)user_id;
	pthread_once(&g_once, bridge_init_once);

	struct guardrail_config gcfg = {
		.enabled           = g_config.react.guardrail_enabled,
		.max_retries       = g_config.react.guardrail_max_retries,
		.max_empty_rounds  = g_config.react.guardrail_max_empty_rounds,
	};

	struct compress_config ccfg = {
		.max_context_tokens     = g_config.models.text.context_limit,
		.max_history_rounds    = g_config.context.keep_recent_rounds,
		.summarize_threshold_ratio =
			g_config.context.summarize_threshold_ratio,
		.compress_target_ratio = g_config.context.compress_target_ratio,
	};

	struct react_context *ctx = react_context_create(
		&g_tools, g_tokenizer, &ccfg, &gcfg);
	if (!ctx)
		return NULL;

	ctx->max_iterations       = g_config.react.max_iterations;
	ctx->step_timeout_seconds = g_config.react.step_timeout_seconds;
	ctx->tool_max_retries     = g_config.react.tool_max_retries;
	ctx->llm_model            = g_llm;
	bash_exec_set_default_timeout(g_config.react.bash_exec_default_timeout);

	/* Load session from DB to get its numeric id */
	struct session sess;
	if (session_get_by_display_id(&store->db, session_id, &sess) != 0) {
		fprintf(stderr, "fcgi-bridge: session %s not found\n",
			session_id);
		return ctx;  /* return bare context — no history */
	}

	/* Replay message history into react's message_list */
	int msg_count = 0;
	struct message *msgs = message_list(&store->db, sess.id, &msg_count);
	struct message *cur  = msgs;
	while (cur) {
		if (cur->role[0] && cur->content) {
			struct message_list *ml = msg_list_create(
				cur->role, cur->content, cur->token_count);
			if (ml) {
				ml->compressed = cur->compressed;
				msg_list_append(&ctx->messages, ml);
			}
		}
		cur = cur->next;
	}
	message_free_list(msgs);

	return ctx;
}
