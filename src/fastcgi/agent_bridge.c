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
#include "guardrail.h"
#include "session_store.h"
#include "session.h"
#include "agent/memory.h"
#include "agent/context.h"
#include "agent/tokenizer.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tools/text_gen.h"
#include "agent/tools/text_qa.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_qa.h"
#include "agent/tools/img_inpaint.h"
#include "agent/tools/img_compose.h"
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
#include "ext/ext.h"
#include "ext/manifest.h"
#include "models/llm.h"
#include "config.h"
#include "util/log.h"
#include "util/file.h"
#include <errno.h>
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
static struct tool_context *g_tctx    = NULL;

static void bridge_init_once(void);

const char *fcgi_artifact_output_dir(void)
{
	pthread_once(&g_once, bridge_init_once);
	const char *env = getenv("MORPH_FCGI_OUTPUT_DIR");
	if (env && *env)
		return env;
	return g_config.general.output_dir[0]
		? g_config.general.output_dir : "/var/lib/morph/output";
}

const struct config *fcgi_bridge_config(void)
{
	pthread_once(&g_once, bridge_init_once);
	return &g_config;
}

int react_memory_options_for_session(struct memory_options *out)
{
	if (!out)
		return -EINVAL;
	pthread_once(&g_once, bridge_init_once);
	memset(out, 0, sizeof(*out));
	out->enabled = g_config.memory.enabled;
	out->hot_path_enabled = g_config.memory.hot_path_enabled;
	out->cold_path_enabled = g_config.memory.cold_path_enabled;
	out->llm_extract_enabled = g_config.memory.llm_extract_enabled;
	out->max_facts = g_config.memory.max_facts;
	out->max_episodes = g_config.memory.max_episodes;
	out->max_procedures = g_config.memory.max_procedures;
	out->max_context_chars = g_config.memory.max_context_chars;
	return 0;
}

static void bridge_init_once(void)
{
	config_set_defaults(&g_config);

	/* MORPH_FCGI_CONFIG env → $HOME/.morph/config.toml */
	const char *cfg_env = getenv("MORPH_FCGI_CONFIG");
	if (cfg_env && *cfg_env) {
		config_load(&g_config, cfg_env);
	} else {
		const char *home = getenv("HOME");
		if (home) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path),
				 "%s/.morph/config.toml", home);
			config_load(&g_config, path);
		}
	}

	/* Environment variable overrides (take precedence over config) */
	{
		const char *env;
		if ((env = getenv("MORPH_FCGI_OUTPUT_DIR")) && *env)
			strncpy(g_config.general.output_dir, env,
				sizeof(g_config.general.output_dir) - 1);
		if ((env = getenv("MORPH_FCGI_LOG_FILE")) && *env)
			strncpy(g_config.general.log_file, env,
				sizeof(g_config.general.log_file) - 1);
	}

	/* Initialize logging from config, with fallback */
	{
		const char *lf = g_config.general.log_file;
		const char *log_path = (lf && *lf)
			? lf : "/var/lib/morph/log/fastcgi.log";
		log_init(log_path, LOG_DEBUG);
		fprintf(stderr, "fcgi-bridge: log initialized to %s\n",
			log_path);
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

	/* Wire chat LLM into memory subsystem for cold-path extraction. */
	memory_set_llm(g_llm);

	/* Register tools — same set as cli.c */
	tool_registry_init(&g_tools);

	g_tctx = tool_context_create(g_config.general.output_dir,
				     g_config.general.output_dir);
	if (!g_tctx)
		fprintf(stderr, "fcgi-bridge: tool_context_create failed\n");

	text_gen_init(&g_tools, g_llm);
	text_qa_init(&g_tools, g_llm);
	img_qa_init(&g_tools, g_llm, g_tctx);

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
			img_gen_init(&g_tools, img_m, g_tctx);
			img_inpaint_init(&g_tools, img_m, g_tctx);
			img_compose_init(&g_tools, img_m, g_tctx);
		}
	}

	img_info_init(&g_tools, g_tctx);
	img_resize_init(&g_tools, g_tctx);
	img_convert_init(&g_tools, g_tctx);

	file_read_init(&g_tools, g_tctx);
	file_list_init(&g_tools, g_tctx);
	file_info_init(&g_tools, g_tctx);

	if (g_config.react.bash_exec_enabled) {
		for (int i = 0;
		     i < g_config.react.bash_exec_allowed_commands_count; i++)
			tool_context_allow_command_pattern(
				g_tctx,
				g_config.react.bash_exec_allowed_commands[i]);
		for (int i = 0;
		     i < g_config.react.bash_exec_allowed_cwds_count; i++)
			tool_context_allow_command_scope(
				g_tctx,
				g_config.react.bash_exec_allowed_cwds[i]);
		bash_exec_init(&g_tools, g_tctx);
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
			vid_gen_init(&g_tools, vid_m, g_tctx);
	}

	/* Apply disabled_tools from config */
	for (int i = 0; i < g_config.react.disabled_tools_count; i++)
		tool_disable(&g_tools, g_config.react.disabled_tools[i]);

	plan_registry_init(&g_plans);
	plan_tool_init(&g_tools, &g_plans, g_llm);

	log_info("fcgi-bridge: registered %d tools", g_tools.count);
}

static void bridge_discover_guardrail_exts(struct guardrail_config *gcfg)
{
	char exts_dir[PATH_MAX] = {0};
	char *exts_home = file_expand_path("~/.morph/exts");
	if (exts_home) {
		strncpy(exts_dir, exts_home, sizeof(exts_dir) - 1);
		free(exts_home);
	} else {
		return;
	}
	if (!file_exists(exts_dir))
		return;
	char **ext_dirs = NULL;
	int ext_count = 0;
	if (file_list_dirs(exts_dir, &ext_dirs, &ext_count) != 0)
		return;
	for (int i = 0; i < ext_count; i++) {
		char ed_path[PATH_MAX];
		if (file_path_join(ed_path, sizeof(ed_path),
				   exts_dir, ext_dirs[i]) != 0)
			continue;
		struct ext ex;
		int rc = ext_load(&ex, ed_path);
		if (rc != 0 || !ex.enabled
		    || ex.manifest.purpose != EXT_PURPOSE_GUARDRAIL) {
			ext_unload(&ex);
			continue;
		}
		enum guardrail_hook gh = GUARDRAIL_HOOK_OUTPUT;
		if (strcmp(ex.manifest.hook, "input") == 0)
			gh = GUARDRAIL_HOOK_INPUT;
		else if (strcmp(ex.manifest.hook, "tool_output") == 0)
			gh = GUARDRAIL_HOOK_TOOL_OUTPUT;
		enum guardrail_ext_type et = GUARDRAIL_EXT_EXEC;
		if (strcmp(ex.manifest.type, "so") == 0)
			et = GUARDRAIL_EXT_SO;
		guardrail_rule_register(gcfg, ex.manifest.name, gh,
					GUARDRAIL_RULE_EXT, NULL,
					ex.manifest.description, NULL,
					ex.manifest.action_text[0]
						? ex.manifest.action_text
						: NULL);
		if (et == GUARDRAIL_EXT_SO) {
			struct guardrail_rule *r =
				guardrail_rule_lookup(gcfg, ex.manifest.name);
			if (r) {
				r->ext_type = GUARDRAIL_EXT_SO;
				char full[PATH_MAX];
				if (file_path_join(full, sizeof(full),
						   ed_path, ex.manifest.entry)
				    != 0)
					continue;
				strncpy(r->ext_entry, full,
					sizeof(r->ext_entry) - 1);
				guardrail_ext_so_load(r);
			}
		} else {
			struct guardrail_rule *r =
				guardrail_rule_lookup(gcfg, ex.manifest.name);
			if (r) {
				r->ext_type = GUARDRAIL_EXT_EXEC;
				char full[PATH_MAX];
				if (file_path_join(full, sizeof(full),
						   ed_path, ex.manifest.entry)
				    != 0)
					continue;
				strncpy(r->ext_entry, full,
					sizeof(r->ext_entry) - 1);
			}
		}
		log_info("fcgi-bridge: registered guardrail ext: %s",
			 ex.manifest.name);
		ext_unload(&ex);
	}
	file_free_list(ext_dirs, ext_count);
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

	for (int i = 0; i < g_config.react.guardrail_llm_rule_count; i++) {
		struct config_guardrail_llm_rule *cr =
			&g_config.react.guardrail_llm_rules[i];
		enum guardrail_hook hook = GUARDRAIL_HOOK_OUTPUT;
		if (strcmp(cr->hook, "input") == 0)
			hook = GUARDRAIL_HOOK_INPUT;
		guardrail_rule_register(&ctx->guardrail, cr->name,
			hook, GUARDRAIL_RULE_LLM, NULL,
			cr->description, NULL, cr->action_text);
	}
	for (int i = 0; i < g_config.react.guardrail_ext_rule_count; i++) {
		struct config_guardrail_ext_rule *cr =
			&g_config.react.guardrail_ext_rules[i];
		enum guardrail_hook hook = GUARDRAIL_HOOK_OUTPUT;
		if (strcmp(cr->hook, "input") == 0)
			hook = GUARDRAIL_HOOK_INPUT;
		guardrail_rule_register(&ctx->guardrail, cr->name,
			hook, GUARDRAIL_RULE_EXT, NULL,
			cr->ext_type[0] == '\0' || strcmp(cr->ext_type, "exec") == 0
				? NULL : cr->ext_type,
			cr->ext_entry, cr->action_text);
		if (strcmp(cr->ext_type, "so") == 0) {
			struct guardrail_rule *r =
				guardrail_rule_lookup(&ctx->guardrail, cr->name);
			if (r) {
				r->ext_type = GUARDRAIL_EXT_SO;
				guardrail_ext_so_load(r);
			}
		}
	}
	bridge_discover_guardrail_exts(&ctx->guardrail);
	if (g_llm)
		guardrail_set_llm(&ctx->guardrail, g_llm);
	for (int i = 0; i < g_config.react.guardrail_disabled_rule_count; i++)
		guardrail_rule_disable(&ctx->guardrail,
			g_config.react.guardrail_disabled_rules[i]);

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
				ctx->session_arena, cur->role, cur->content, cur->token_count);
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
