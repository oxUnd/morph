#include "cli/internal.h"
#include "cli/commands/registry.h"
#include "agent/tools/dynamic_tools.h"

#ifndef MORPH_BUILTIN_SKILLS_DIR
#define MORPH_BUILTIN_SKILLS_DIR ""
#endif

static int ext_run_wrapper(const char *args_json, struct tool_result *result, void *user_data)
{
	struct ext *ex = user_data;
	if (!ex)
		return -EINVAL;
	char *out = NULL;
	int rc = ext_run(ex, args_json, &out);
	if (out)
		(void)tool_result_take_text(result, out);
	return rc;
}
struct auto_connect_work {
	struct mcp_client *client;
	int result;
	int done;
	int detached;
	pthread_mutex_t lock;
	pthread_cond_t cond;
};

static void auto_connect_work_destroy(struct auto_connect_work *w)
{
	if (!w)
		return;
	pthread_mutex_destroy(&w->lock);
	pthread_cond_destroy(&w->cond);
	free(w);
}

static void *auto_connect_thread(void *arg)
{
	struct auto_connect_work *w = arg;
	int rc = mcp_ensure_connected(w->client);
	int detached;

	pthread_mutex_lock(&w->lock);
	w->result = rc;
	w->done = 1;
	detached = w->detached;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->lock);

	if (detached)
		auto_connect_work_destroy(w);
	return NULL;
}

/*
 * Load configuration from TOML file and set defaults.
 * ctx - CLI context to configure.
 * config_path - Path to config file, or NULL for default.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_config(struct cli_context *ctx, const char *config_path)
{
	config_set_defaults(&ctx->config);
	if (!config_path)
		config_path = default_config_path;
	char *expanded = file_expand_path(config_path);
	if (expanded) {
		snprintf(ctx->config_path, sizeof(ctx->config_path),
			 "%s", expanded);
	} else {
		snprintf(ctx->config_path, sizeof(ctx->config_path),
			 "%s", config_path);
	}
	if (expanded && file_exists(expanded))
		config_load(&ctx->config, expanded);
	free(expanded);
	return 0;
}

/*
 * Open the database and initialize its schema.
 * ctx - CLI context with config already loaded.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_database(struct cli_context *ctx)
{
	char *db_path = file_expand_path(default_db_path);
	char *db_dir = file_expand_path("~/.morph");
	file_ensure_dir(db_dir);
	free(db_dir);
	int rc = db_open(&ctx->database, db_path);
	free(db_path);
	if (rc < 0) {
		log_err("failed to open database");
		return rc;
	}
	db_init_schema(&ctx->database);
	return 0;
}

/*
 * Create tokenizer, react context, and LLM/image/video models.
 * Configures compress, guardrail, HITL, and system prompts.
 * ctx - CLI context with config and database initialized.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_models(struct cli_context *ctx)
{
	tool_registry_init(&ctx->tools);

	ctx->tokenizer = tokenizer_create(ctx->config.models.text.model,
					  ctx->config.models.text.context_limit);
	if (!ctx->tokenizer) {
		log_err("failed to create tokenizer");
		db_close(&ctx->database);
		return -ENOMEM;
	}

	struct compress_config compress_cfg = {
		.max_context_tokens = ctx->config.models.text.context_limit,
		.max_history_rounds = ctx->config.context.keep_recent_rounds,
		.summarize_threshold_ratio = ctx->config.context.summarize_threshold_ratio,
		.compress_target_ratio = ctx->config.context.compress_target_ratio,
	};
	struct guardrail_config guardrail_cfg = {
		.enabled = ctx->config.react.guardrail_enabled,
		.max_retries = ctx->config.react.guardrail_max_retries,
		.max_empty_rounds = ctx->config.react.guardrail_max_empty_rounds,
	};
	ctx->react = react_context_create(&ctx->tools, ctx->tokenizer,
					  &compress_cfg, &guardrail_cfg);
	if (!ctx->react) {
		log_err("failed to create react context");
		tokenizer_destroy(ctx->tokenizer);
		db_close(&ctx->database);
		return -ENOMEM;
	}
	react_set_event_callback(ctx->react, ctx->event_cb,
				 ctx->event_user_data);
	ctx->react->tool_timeout_seconds = ctx->config.react.tool_timeout_seconds;
	ctx->react->tool_max_retries = ctx->config.react.tool_max_retries;
	ctx->react->max_iterations = ctx->config.react.max_iterations;
	bash_exec_set_default_timeout(ctx->config.react.bash_exec_default_timeout);
	ctx->react->hitl.enabled = ctx->config.react.hitl_enabled;
	ctx->react->hitl.auto_approve_readonly = ctx->config.react.hitl_auto_approve_readonly;
	ctx->react->hitl.tools_count = ctx->config.react.hitl_tools_count;
	for (int i = 0; i < ctx->config.react.hitl_tools_count; i++)
		strncpy(ctx->react->hitl.tools[i], ctx->config.react.hitl_tools[i],
			HITL_TOOL_NAME_MAX - 1);
	if (ctx->react->hitl.enabled) {
		ctx->react->hitl.approval_cb = hitl_approval_callback;
		ctx->react->hitl.approval_user_data = ctx;
	}

	if (ctx->workdir[0]) {
		ctx->react->workdir = strdup(ctx->workdir);
		if (!ctx->react->workdir)
			log_warn("failed to set react workdir");
	}

	if (ctx->config.prompt.system_prompt_file[0]) {
		char *exp = file_expand_path(ctx->config.prompt.system_prompt_file);
		if (exp) {
			char *content = file_read_all(exp, NULL);
			if (content) {
				size_t len = strlen(content);
				while (len > 0 && (content[len-1] == '\n' ||
				       content[len-1] == '\r' || content[len-1] == ' '))
					content[--len] = '\0';
				ctx->react->system_prompt = content;
				log_info("loaded system prompt: %s",
					 ctx->config.prompt.system_prompt_file);
			} else {
				log_warn("failed to read system prompt: %s",
					 ctx->config.prompt.system_prompt_file);
			}
			free(exp);
		}
	}

	if (ctx->config.prompt.system_prompt_dir[0]) {
		char *exp2 = file_expand_path(ctx->config.prompt.system_prompt_dir);
		if (exp2) {
			char **files = NULL;
			int nfiles = 0;
			if (file_list_files(exp2, &files, &nfiles) == 0) {
				for (int i = 0; i < nfiles; i++) {
					char full[PATH_MAX];
					if (file_path_join(full, sizeof(full),
							   exp2, files[i]) != 0)
						continue;
					char *content = file_read_all(full, NULL);
					if (!content)
						continue;
					size_t clen = strlen(content);
					while (clen > 0 && (content[clen-1] == '\n' ||
					       content[clen-1] == '\r' || content[clen-1] == ' '))
						content[--clen] = '\0';
					if (!clen) {
						free(content);
						continue;
					}
					char *old = ctx->react->system_prompt;
					size_t old_len = old ? strlen(old) : 0;
					char *combined = malloc(old_len + 3 + clen + 1);
					if (combined) {
						if (old) {
							memcpy(combined, old, old_len);
							combined[old_len] = '\n';
							combined[old_len + 1] = '\n';
							memcpy(combined + old_len + 2, content, clen + 1);
						} else {
							memcpy(combined, content, clen + 1);
						}
						ctx->react->system_prompt = combined;
					}
					free(old);
					free(content);
				}
				file_free_list(files, nfiles);
				log_info("loaded %d prompt files from: %s",
					 nfiles, ctx->config.prompt.system_prompt_dir);
			}
			free(exp2);
		}
	}

	strncpy(ctx->current_session.name, ctx->config.general.default_session,
		sizeof(ctx->current_session.name) - 1);
	ctx->current_session.name[sizeof(ctx->current_session.name) - 1] = '\0';

	const char *api_key = NULL;
	if (ctx->config.models.text.api_key[0])
		api_key = ctx->config.models.text.api_key;
	else
		api_key = getenv(ctx->config.models.text.api_key_env);

	struct model *llm = model_llm_create(
		ctx->config.models.text.provider,
		ctx->config.models.text.model,
		ctx->config.models.text.api_base,
		api_key ? api_key : "");
	ctx->llm = llm;
	ctx->react->llm_model = llm;
	cli_set_usage_context(ctx);
	model_set_usage_callback(cli_record_model_usage);
	model_set_usage_user_data(ctx);
	if (llm) {
		llm->timeout_seconds = ctx->config.models.text.timeout_seconds;
		if (ctx->config.models.text.max_tokens > 0)
			llm->max_tokens = ctx->config.models.text.max_tokens;
		if (ctx->config.models.text.context_limit > 0)
			llm->context_limit = ctx->config.models.text.context_limit;
	}
	/* Wire the chat LLM into the memory subsystem so cold-path
	 * consolidation can use LLM-driven extraction. */
	memory_set_llm(llm);

	for (int i = 0; i < ctx->config.react.guardrail_llm_rule_count; i++) {
		struct config_guardrail_llm_rule *cr =
			&ctx->config.react.guardrail_llm_rules[i];
		enum guardrail_hook hook = GUARDRAIL_HOOK_OUTPUT;
		if (strcmp(cr->hook, "input") == 0)
			hook = GUARDRAIL_HOOK_INPUT;
		guardrail_rule_register(&ctx->react->guardrail, cr->name,
			hook, GUARDRAIL_RULE_LLM, NULL,
			cr->description, NULL, cr->action_text);
	}
	for (int i = 0; i < ctx->config.react.guardrail_ext_rule_count; i++) {
		struct config_guardrail_ext_rule *cr =
			&ctx->config.react.guardrail_ext_rules[i];
		enum guardrail_hook hook = GUARDRAIL_HOOK_OUTPUT;
		if (strcmp(cr->hook, "input") == 0)
			hook = GUARDRAIL_HOOK_INPUT;
		guardrail_rule_register(&ctx->react->guardrail, cr->name,
			hook, GUARDRAIL_RULE_EXT, NULL,
			cr->ext_type[0] == '\0' || strcmp(cr->ext_type, "exec") == 0
				? NULL : cr->ext_type,
			cr->ext_entry, cr->action_text);
		if (strcmp(cr->ext_type, "so") == 0) {
			struct guardrail_rule *r =
				guardrail_rule_lookup(&ctx->react->guardrail, cr->name);
			if (r) {
				r->ext_type = GUARDRAIL_EXT_SO;
				guardrail_ext_so_load(r);
			}
		}
	}
	if (ctx->config.react.guardrail_llm_model[0] && llm)
		guardrail_set_llm(&ctx->react->guardrail, llm);
	else if (llm)
		guardrail_set_llm(&ctx->react->guardrail, llm);
	for (int i = 0; i < ctx->config.react.guardrail_disabled_rule_count; i++)
		guardrail_rule_disable(&ctx->react->guardrail,
			ctx->config.react.guardrail_disabled_rules[i]);

	const char *img_api_key = NULL;
	if (ctx->config.models.image.api_key[0])
		img_api_key = ctx->config.models.image.api_key;
	else
		img_api_key = getenv(ctx->config.models.image.api_key_env);
	struct model *img_llm = model_llm_create(
		ctx->config.models.image.provider,
		ctx->config.models.image.model,
		ctx->config.models.image.api_base[0] ?
			ctx->config.models.image.api_base : NULL,
		img_api_key ? img_api_key : "");
	ctx->img_llm = img_llm;

	const char *vid_api_key = NULL;
	if (ctx->config.models.video.api_key[0])
		vid_api_key = ctx->config.models.video.api_key;
	else
		vid_api_key = getenv(ctx->config.models.video.api_key_env);
	struct model *vid_llm = model_llm_create(
		ctx->config.models.video.provider,
		ctx->config.models.video.model,
		ctx->config.models.video.api_base[0] ?
			ctx->config.models.video.api_base : NULL,
		vid_api_key ? vid_api_key : "");
	ctx->vid_llm = vid_llm;

	return 0;
}

/*
 * Register all built-in tools and configure tool flags.
 * Includes img_*, vid_*, file_*, bash_exec, runtime query,
 * skill, plan, and ask_user tools.
 * ctx - CLI context with models and react context initialized.
 *
 * Returns 0 on success, negative errno on failure.
 */
static void cli_img_annotate_pause(void *user_data)
{
	(void)user_data;
	fflush(stdout);
}

static void cli_img_annotate_resume(void *user_data)
{
	(void)user_data;
	fflush(stdout);
}

static int cli_init_tools(struct cli_context *ctx)
{
	int rc = 0;

	ctx->tctx = tool_context_create(ctx->workdir,
					ctx->config.general.output_dir);
	if (!ctx->tctx) {
		log_err("failed to create tool context");
		return -ENOMEM;
	}
	tool_context_set_operation_approval(ctx->tctx,
					    operation_approval_callback, ctx);
	tool_context_set_default_timeout(ctx->tctx,
					 ctx->config.react.tool_timeout_seconds);

	runtime_query_tools_init(&ctx->tools);
	log_info("registered runtime query tools");

	img_qa_init(&ctx->tools, ctx->llm, ctx->tctx);
	log_info("registered img_qa tool");

	img_gen_init(&ctx->tools, ctx->img_llm, ctx->tctx);
	log_info("registered img_gen tool");

	img_inpaint_init(&ctx->tools, ctx->img_llm, ctx->tctx);
	log_info("registered img_inpaint tool");

	img_compose_init(&ctx->tools, ctx->img_llm, ctx->tctx);
	log_info("registered img_compose tool");

	img_info_init(&ctx->tools, ctx->tctx);
	log_info("registered img_info tool");

	file_read_init(&ctx->tools, ctx->tctx);
	log_info("registered file_read tool");

	file_list_init(&ctx->tools, ctx->tctx);
	log_info("registered file_list tool");

	file_info_init(&ctx->tools, ctx->tctx);
	log_info("registered file_info tool");

	config_write_init(&ctx->tools, ctx->tctx, ctx->config_path);
	log_info("registered config_write tool");

	if (ctx->config.react.bash_exec_enabled) {
		for (int i = 0;
		     i < ctx->config.react.bash_exec_allowed_commands_count; i++)
			tool_context_allow_command_pattern(
				ctx->tctx,
				ctx->config.react.bash_exec_allowed_commands[i]);
		for (int i = 0;
		     i < ctx->config.react.bash_exec_allowed_cwds_count; i++)
			tool_context_allow_command_scope(
				ctx->tctx,
				ctx->config.react.bash_exec_allowed_cwds[i]);
		bash_exec_init(&ctx->tools, ctx->tctx);
		tool_set_timeout(&ctx->tools, "bash_exec",
				 ctx->config.react.bash_exec_default_timeout);
		log_info("registered bash_exec tool (explicitly enabled)");
	} else {
		log_info("bash_exec tool disabled by default");
	}

	img_resize_init(&ctx->tools, ctx->tctx);
	log_info("registered img_resize tool");

	img_convert_init(&ctx->tools, ctx->tctx);
	log_info("registered img_convert tool");

	vid_gen_init(&ctx->tools, ctx->vid_llm, ctx->tctx);
	log_info("registered vid_gen tool");

	{
		static const char *readonly_tools[] = {
			"file_read", "file_list", "file_info",
			"img_info", "credits", "memory",
			"ask_user", "activate_skill", "plan",
			"agent_status",
			NULL
		};
		for (const char **t = readonly_tools; *t; t++) {
			struct tool_entry *e = tool_lookup(&ctx->tools, *t);
			if (e)
				e->flags |= TOOL_FLAG_READONLY;
		}
	}

	ctx->skills = calloc(1, sizeof(*ctx->skills));
	if (!ctx->skills) {
		log_err("failed to allocate skill registry");
		return -ENOMEM;
	}
	skill_registry_init(ctx->skills);

	if (ctx->config.skill.dir[0]) {
		char *skill_dir = file_expand_path(ctx->config.skill.dir);
		if (skill_dir) {
			if (file_exists(skill_dir))
				skill_discover(ctx->skills, skill_dir);
			free(skill_dir);
		}
	} else {
		char *morph_skills = file_expand_path("~/.morph/skills");
		if (morph_skills) {
			if (!file_exists(morph_skills))
				file_ensure_dir(morph_skills);
			skill_discover(ctx->skills, morph_skills);
			free(morph_skills);
		}
		char *agents_skills = file_expand_path("~/.agents/skills");
		if (agents_skills) {
			if (!file_exists(agents_skills))
				file_ensure_dir(agents_skills);
			skill_discover(ctx->skills, agents_skills);
			free(agents_skills);
		}
	}
	if (MORPH_BUILTIN_SKILLS_DIR[0]) {
		char *builtin_skills = file_expand_path(MORPH_BUILTIN_SKILLS_DIR);
		if (builtin_skills) {
			if (file_exists(builtin_skills))
				skill_discover(ctx->skills, builtin_skills);
			free(builtin_skills);
		}
	}

	if (ctx->skills->count > 0) {
		skill_activate_init(&ctx->tools, ctx->skills);
		log_info("registered activate_skill tool (%d skills discovered)",
			 ctx->skills->count);
	}

	plan_registry_init(&ctx->plans);
	rc = plan_tool_init(&ctx->tools, &ctx->plans, ctx->llm);
	if (rc < 0)
		log_err("failed to register plan tool: %s", morph_strerror(rc));
	else
		log_info("registered plan tool");

	ask_user_init(&ctx->tools, cli_ask_user_callback, ctx);
	ctx->react->ask_user_fn = cli_ask_user_callback;
	ctx->react->ask_user_data = ctx;
	log_info("registered ask_user tool");

	struct scheduled_task_event_sink task_events = cli_task_event_sink(ctx);
	rc = scheduled_tasks_tool_init_events(&ctx->tools, &ctx->database,
					      &task_events);
	if (rc < 0)
		log_err("failed to register tasks tool: %s", morph_strerror(rc));
	else
		log_info("registered tasks tool");

	img_annotate_init(&ctx->tools, cli_img_annotate_pause,
			  cli_img_annotate_resume, ctx, ctx->tctx);
	log_info("registered img_annotate tool");

	for (int i = 0; i < ctx->config.react.disabled_tools_count; i++) {
		tool_disable(&ctx->tools, ctx->config.react.disabled_tools[i]);
	}

	ctx->react->skills = ctx->skills;

	return 0;
}

/*
 * Discover and load extensions from the ~/.morph/exts directory.
 * ctx - CLI context with tool registry initialized.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_exts(struct cli_context *ctx)
{
	char exts_dir[PATH_MAX] = {0};
	const char *configured = ctx->config.ext.dir[0] ?
		ctx->config.ext.dir : "~/.morph/exts";
	char *exts_home = file_expand_path(configured);
	if (exts_home) {
		strncpy(exts_dir, exts_home, sizeof(exts_dir) - 1);
		free(exts_home);
	} else {
		strncpy(exts_dir, "exts", sizeof(exts_dir) - 1);
	}
	if (!file_exists(exts_dir))
		file_ensure_dir(exts_dir);
	char **ext_dirs = NULL;
	int ext_count = 0;
	if (file_list_dirs(exts_dir, &ext_dirs, &ext_count) == 0) {
		for (int i = 0; i < ext_count; i++) {
			if (ext_dirs[i][0] == '.')
				continue;
			char ed_path[PATH_MAX + NAME_MAX + 2];
			if (file_path_join(ed_path, sizeof(ed_path),
					   exts_dir, ext_dirs[i]) != 0)
				continue;
			struct ext ex;
			int rc = ext_load(&ex, ed_path);
			if (rc == 0 && ex.enabled) {
				if (!ext_manifest_supports_front(&ex.manifest,
								 "cli")) {
					log_info("skipping ext %s: unsupported front cli",
						 ex.manifest.name);
					ext_unload(&ex);
					continue;
				}
				if (ex.manifest.purpose == EXT_PURPOSE_GUARDRAIL) {
					enum guardrail_hook gh =
						GUARDRAIL_HOOK_OUTPUT;
					if (strcmp(ex.manifest.hook, "input")
					    == 0)
						gh = GUARDRAIL_HOOK_INPUT;
					else if (strcmp(ex.manifest.hook,
							"tool_output") == 0)
						gh =
						GUARDRAIL_HOOK_TOOL_OUTPUT;
					enum guardrail_ext_type et =
						GUARDRAIL_EXT_EXEC;
					if (strcmp(ex.manifest.type, "so")
					    == 0)
						et = GUARDRAIL_EXT_SO;
					guardrail_rule_register(
						&ctx->react->guardrail,
						ex.manifest.name, gh,
						GUARDRAIL_RULE_EXT, NULL,
						ex.manifest.description,
						NULL,
						ex.manifest.action_text[0]
							? ex.manifest.action_text
							: NULL);
					if (et == GUARDRAIL_EXT_SO) {
						struct guardrail_rule *r =
							guardrail_rule_lookup(
							  &ctx->react->guardrail,
							  ex.manifest.name);
						if (r) {
							r->ext_type =
								GUARDRAIL_EXT_SO;
							char full[PATH_MAX + NAME_MAX + 130];
							if (file_path_join(
									full,
									sizeof(full),
									ed_path,
									ex.manifest.entry)
							    != 0)
								continue;
							strncpy(r->ext_entry,
								full,
								sizeof(r->ext_entry)
								- 1);
							guardrail_ext_so_load(
								r);
						}
					} else {
						struct guardrail_rule *r =
							guardrail_rule_lookup(
							  &ctx->react->guardrail,
							  ex.manifest.name);
						if (r) {
							r->ext_type =
								GUARDRAIL_EXT_EXEC;
							char full[PATH_MAX + NAME_MAX + 130];
							if (file_path_join(
									full,
									sizeof(full),
									ed_path,
									ex.manifest.entry)
							    != 0)
								continue;
							strncpy(r->ext_entry,
								full,
								sizeof(r->ext_entry)
								- 1);
						}
					}
					log_info("registered guardrail ext: %s",
						 ex.manifest.name);
					ext_unload(&ex);
					continue;
				}
				struct ext *ex_ptr = malloc(sizeof(*ex_ptr));
				if (ex_ptr) {
					memcpy(ex_ptr, &ex, sizeof(ex));
					tool_register(TOOL_ORIGIN_EXT, &ctx->tools, ex.manifest.name,
						      ex.manifest.description,
						      ex.manifest.args_schema ?
						      ex.manifest.args_schema : "",
						      ext_run_wrapper, ex_ptr,
						      ext_user_data_destroy);
					log_info("registered ext: %s", ex.manifest.name);
				}
			} else {
				ext_unload(&ex);
			}
		}
		file_free_list(ext_dirs, ext_count);
	}
	return 0;
}

/*
 * Initialize MCP servers from config and auto-connect those with auto_connect=true.
 * ctx - CLI context with config and tool registry initialized.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_mcp(struct cli_context *ctx)
{
	mcp_registry_init(&ctx->mcp);
	for (int i = 0; i < ctx->config.mcp.server_count; i++) {
		struct mcp_server_config scfg;
		memset(&scfg, 0, sizeof(scfg));
		struct config_mcp_server *cs = &ctx->config.mcp.servers[i];

		strncpy(scfg.name, cs->name, MCP_NAME_MAX - 1);
		scfg.transport = (strcmp(cs->transport, "http") == 0)
			       ? MCP_TRANSPORT_STREAMABLE_HTTP
			       : MCP_TRANSPORT_STDIO;

		if (scfg.transport == MCP_TRANSPORT_STDIO) {
			strncpy(scfg.command, cs->command, sizeof(scfg.command) - 1);
			scfg.cmd_args_count = cs->args_count;
			for (int j = 0; j < cs->args_count; j++)
				strncpy(scfg.cmd_args[j], cs->args[j], MCP_CMD_ARG_MAX - 1);
			scfg.env_count = cs->env_count;
			for (int j = 0; j < cs->env_count; j++) {
				strncpy(scfg.env_keys[j], cs->env_keys[j], sizeof(scfg.env_keys[j]) - 1);
				strncpy(scfg.env_vals[j], cs->env_vals[j], MCP_ENV_VAL_MAX - 1);
			}
		} else {
			strncpy(scfg.http_url, cs->http_url, sizeof(scfg.http_url) - 1);
			strncpy(scfg.http_auth_token_env, cs->http_auth_token_env, sizeof(scfg.http_auth_token_env) - 1);
		}

		scfg.auto_connect = cs->auto_connect;
		scfg.connect_timeout = cs->connect_timeout;

		int rc = mcp_registry_add(&ctx->mcp, &scfg);
		if (rc == 0) {
			char msg[256];

			snprintf(msg, sizeof(msg), "%s registered%s",
				 scfg.name,
				 scfg.auto_connect ? " (auto_connect)" : "");
			log_info("mcp: registered server '%s'%s", scfg.name,
				 scfg.auto_connect ? " (auto_connect)" : "");
			cli_emit_mcp_event(ctx, "mcp.registered",
					   "begin", msg, scfg.name,
					   scfg.transport, scfg.auto_connect,
					   scfg.connect_timeout, -1, -1, -1, 0);
		}
	}

	for (int i = 0; i < ctx->mcp.count; i++) {
		struct mcp_client *mc = ctx->mcp.servers[i];
		if (!mc->config.auto_connect) {
			char msg[256];

			snprintf(msg, sizeof(msg), "%s skipped (lazy connect)",
				 mc->config.name);
			cli_emit_mcp_event(ctx, "mcp.skipped", "skipped",
					   msg, mc->config.name,
					   mc->config.transport, 0,
					   mc->config.connect_timeout,
					   -1, -1, -1, 0);
			continue;
		}
		int timeout = mc->config.connect_timeout;
		char connecting_msg[256];

		snprintf(connecting_msg, sizeof(connecting_msg),
			 "%s connecting%s", mc->config.name,
			 timeout > 0 ? " (timeout enabled)" : "");
		log_info("mcp: auto-connecting '%s'%s...",
			 mc->config.name,
			 timeout > 0 ? " (timeout enabled)" : "");
		cli_emit_mcp_event(ctx, "mcp.connecting", "begin",
				   connecting_msg, mc->config.name,
				   mc->config.transport, 1, timeout,
				   -1, -1, -1, 0);
		if (timeout > 0) {
			struct auto_connect_work *w = calloc(1, sizeof(*w));
			if (!w) {
				char msg[256];

				snprintf(msg, sizeof(msg),
					 "%s auto-connect allocation failed",
					 mc->config.name);
				cli_emit_mcp_event(ctx, "mcp.failed", "failed",
						   msg, mc->config.name,
						   mc->config.transport, 1,
						   timeout, -1, -1, -1,
						   -ENOMEM);
				log_warn("mcp: auto-connect allocation failed for '%s'",
					 mc->config.name);
				continue;
			}
			w->client = mc;
			w->result = -1;
			pthread_mutex_init(&w->lock, NULL);
			pthread_cond_init(&w->cond, NULL);
			pthread_t tid;
			int terr = pthread_create(&tid, NULL,
						  auto_connect_thread, w);
			if (terr != 0) {
				char msg[256];

				auto_connect_work_destroy(w);
				snprintf(msg, sizeof(msg),
					 "%s auto-connect thread failed",
					 mc->config.name);
				cli_emit_mcp_event(ctx, "mcp.failed", "failed",
						   msg, mc->config.name,
						   mc->config.transport, 1,
						   timeout, -1, -1, -1,
						   -terr);
				log_warn("mcp: auto-connect thread failed for '%s'",
					 mc->config.name);
				continue;
			}
			struct timespec ts;
			int wait_rc = 0;
			int done;
			int result;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += timeout;
			pthread_mutex_lock(&w->lock);
			while (!w->done && wait_rc != ETIMEDOUT)
				wait_rc = pthread_cond_timedwait(&w->cond,
								 &w->lock,
								 &ts);
			done = w->done;
			result = w->result;
			if (!done) {
				w->detached = 1;
				pthread_detach(tid);
			}
			pthread_mutex_unlock(&w->lock);

			if (!done) {
				char msg[256];

				snprintf(msg, sizeof(msg),
					 "%s still starting after %ds; continuing",
					 mc->config.name, timeout);
				cli_emit_mcp_event(ctx, "mcp.timeout", "timeout",
						   msg, mc->config.name,
						   mc->config.transport, 1,
						   timeout, -1, -1, -1,
						   -ETIMEDOUT);
				log_warn("mcp: auto-connect timed out for '%s'",
					 mc->config.name);
				continue;
			}

			pthread_join(tid, NULL);
			auto_connect_work_destroy(w);
			if (result == 0) {
				char msg[256];

				snprintf(msg, sizeof(msg), "%s connected",
					 mc->config.name);
				cli_emit_mcp_event(ctx, "mcp.connected", "end",
						   msg, mc->config.name,
						   mc->config.transport, 1,
						   timeout, -1, -1, -1, 0);
				cli_discover_mcp_server(ctx, mc, 1, timeout);
				log_info("mcp: auto-connected '%s'",
					 mc->config.name);
			} else {
				char msg[256];

				snprintf(msg, sizeof(msg),
					 "%s auto-connect failed: %s",
					 mc->config.name,
					 morph_strerror(result));
				cli_emit_mcp_event(ctx, "mcp.failed", "failed",
						   msg, mc->config.name,
						   mc->config.transport, 1,
						   timeout, -1, -1, -1,
						   result);
				log_warn("mcp: auto-connect failed for '%s': %d",
					 mc->config.name, result);
			}
		} else {
			int rc3 = mcp_ensure_connected(mc);
			if (rc3 == 0) {
				char msg[256];

				snprintf(msg, sizeof(msg), "%s connected",
					 mc->config.name);
				cli_emit_mcp_event(ctx, "mcp.connected", "end",
						   msg, mc->config.name,
						   mc->config.transport, 1,
						   timeout, -1, -1, -1, 0);
				cli_discover_mcp_server(ctx, mc, 1, timeout);
				log_info("mcp: auto-connected '%s'",
					 mc->config.name);
			} else {
				char msg[256];

				snprintf(msg, sizeof(msg),
					 "%s auto-connect failed: %s",
					 mc->config.name,
					 morph_strerror(rc3));
				cli_emit_mcp_event(ctx, "mcp.failed", "failed",
						   msg, mc->config.name,
						   mc->config.transport, 1,
						   timeout, -1, -1, -1,
						   rc3);
				log_warn("mcp: auto-connect failed for '%s': %d",
					 mc->config.name, rc3);
			}
		}
	}

	return 0;
}

/*
 * Initialize sub-agents from config.
 * Creates the sub-agent runtime, loads config, and registers tools.
 */
static int cli_init_sub_agents(struct cli_context *ctx)
{
	if (ctx->config.sub_agents.count == 0)
		return 0;
	ctx->sub_agents = sub_agent_runtime_create(
		&ctx->tools, ctx->llm, ctx->tokenizer,
		&ctx->react->compress);
	if (!ctx->sub_agents) {
		log_err("failed to create sub-agent runtime");
		return -ENOMEM;
	}
	sub_agent_runtime_set_event_callback(ctx->sub_agents, ctx->event_cb,
					     ctx->event_user_data);
	int rc = sub_agent_runtime_load_config(
		ctx->sub_agents, &ctx->config.sub_agents);
	if (rc < 0) {
		log_err("failed to load sub-agent config: %s",
			morph_strerror(rc));
		return rc;
	}
	sub_agent_tools_register_all(&ctx->tools, ctx->sub_agents);
	ctx->react->sub_agent_depth = 0;
	if (ctx->sub_agents->entry_count > 0) {
		ctx->react->sub_agent_info = calloc(
			(size_t)ctx->sub_agents->entry_count,
			sizeof(*ctx->react->sub_agent_info));
		if (ctx->react->sub_agent_info) {
			for (int i = 0; i < ctx->sub_agents->entry_count;
			     i++) {
				strncpy(
					ctx->react->sub_agent_info[i].name,
					ctx->sub_agents->entries[i].cfg.name,
					sizeof(ctx->react->sub_agent_info[i].name) - 1);
				strncpy(
					ctx->react->sub_agent_info[i].description,
					ctx->sub_agents->entries[i].cfg.description,
					sizeof(ctx->react->sub_agent_info[i].description) - 1);
			}
			ctx->react->sub_agent_info_count =
				ctx->sub_agents->entry_count;
		}
	}
	log_info("registered %d sub-agent(s)",
		 ctx->sub_agents->entry_count);
	return 0;
}

/*
 * Initialize the CLI context: load config, open database, create models,
 * register tools, discover extensions and MCP servers, and prepare session.
 * ctx - CLI context to initialize (must be zeroed by caller or here).
 * config_path - Path to config file, or NULL for default.
 * workdir - Override working directory (-w flag), or NULL for cwd.
 *
 * Priority:
 *   workdir (if -w given): both workdir and output_dir = resolved -w value
 *   no -w: workdir = cwd, output_dir = config value (default ~/.morph/output)
 *
 * Returns 0 on success, negative errno on failure.
 */
int cli_init(struct cli_context *ctx, const char *config_path,
	     const char *workdir, enum cli_event_mode event_mode)
{
	if (!ctx)
		return -EINVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->event_mode = event_mode;
	ctx->event_cb = cli_event_callback;
	ctx->event_user_data = ctx;
	(void)cli_commands_init();

	int rc;
	rc = pthread_mutex_init(&ctx->react_lock, NULL);
	if (rc != 0)
		return -rc;
	ctx->react_lock_ready = 1;

	cli_emit_startup_event(ctx, "startup.begin", "begin",
			       "startup started", "cli", 0);
	cli_emit_startup_event(ctx, "startup.component.begin", "begin",
			       "loading config", "config", 0);
	rc = cli_init_config(ctx, config_path);
	if (rc < 0) {
		cli_emit_startup_event(ctx, "startup.component.failed",
				       "failed", "config failed",
				       "config", rc);
		cli_emit_startup_event(ctx, "startup.failed", "failed",
				       "startup failed", "cli", rc);
		pthread_mutex_destroy(&ctx->react_lock);
		ctx->react_lock_ready = 0;
		return rc;
	}
	cli_emit_startup_event(ctx, "startup.component.ready", "ready",
			       "config ready", "config", 0);

	if (workdir && *workdir) {
		char *resolved = file_resolve_path(workdir);
		if (resolved) {
			strncpy(ctx->workdir, resolved,
				sizeof(ctx->workdir) - 1);
			strncpy(ctx->config.general.output_dir, resolved,
				sizeof(ctx->config.general.output_dir) - 1);
			free(resolved);
		} else {
			char *expanded = file_expand_path(workdir);
			if (expanded) {
				strncpy(ctx->workdir, expanded,
					sizeof(ctx->workdir) - 1);
				strncpy(ctx->config.general.output_dir, expanded,
					sizeof(ctx->config.general.output_dir) - 1);
				free(expanded);
			} else {
				strncpy(ctx->workdir, workdir,
					sizeof(ctx->workdir) - 1);
				strncpy(ctx->config.general.output_dir, workdir,
					sizeof(ctx->config.general.output_dir) - 1);
			}
		}
	} else {
		if (!getcwd(ctx->workdir, sizeof(ctx->workdir)))
			strncpy(ctx->workdir, ".", 2);
	}

	cli_emit_startup_event(ctx, "startup.component.begin", "begin",
			       "opening database", "database", 0);
	rc = cli_init_database(ctx);
	if (rc < 0) {
		cli_emit_startup_event(ctx, "startup.component.failed",
				       "failed", "database failed",
				       "database", rc);
		cli_emit_startup_event(ctx, "startup.failed", "failed",
				       "startup failed", "cli", rc);
		pthread_mutex_destroy(&ctx->react_lock);
		ctx->react_lock_ready = 0;
		return rc;
	}
	cli_emit_startup_event(ctx, "startup.component.ready", "ready",
			       "database ready", "database", 0);

	cli_emit_startup_event(ctx, "startup.component.begin", "begin",
			       "initializing models", "models", 0);
	rc = cli_init_models(ctx);
	if (rc < 0) {
		cli_emit_startup_event(ctx, "startup.component.failed",
				       "failed", "models failed",
				       "models", rc);
		goto fail;
	}
	cli_emit_startup_event(ctx, "startup.component.ready", "ready",
			       "models ready", "models", 0);

	cli_emit_startup_event(ctx, "startup.component.begin", "begin",
			       "registering tools", "tools", 0);
	rc = cli_init_tools(ctx);
	if (rc < 0) {
		cli_emit_startup_event(ctx, "startup.component.failed",
				       "failed", "tools failed",
				       "tools", rc);
		goto fail;
	}
	cli_emit_startup_event(ctx, "startup.component.ready", "ready",
			       "tools ready", "tools", 0);

	cli_emit_startup_event(ctx, "startup.component.begin", "begin",
			       "loading extensions", "extensions", 0);
	rc = cli_init_exts(ctx);
	if (rc < 0) {
		cli_emit_startup_event(ctx, "startup.component.failed",
				       "failed", "extensions failed",
				       "extensions", rc);
		goto fail;
	}
	cli_emit_startup_event(ctx, "startup.component.ready", "ready",
			       "extensions ready", "extensions", 0);

	cli_emit_startup_event(ctx, "startup.component.begin", "begin",
			       "loading dynamic tools", "dynamic_tools", 0);
	rc = dynamic_tools_init(&ctx->tools, ctx->tctx,
				&ctx->config.dynamic_tools,
				ctx->current_session.display_id[0]
					? ctx->current_session.display_id
					: ctx->current_session.name);
	if (rc < 0) {
		cli_emit_startup_event(ctx, "startup.component.failed",
				       "failed", "dynamic tools failed",
				       "dynamic_tools", rc);
		goto fail;
	}
	for (int i = 0; i < ctx->config.react.disabled_tools_count; i++)
		tool_disable(&ctx->tools, ctx->config.react.disabled_tools[i]);
	cli_emit_startup_event(ctx, "startup.component.ready", "ready",
			       "dynamic tools ready", "dynamic_tools", 0);

	cli_emit_startup_event(ctx, "startup.component.begin", "begin",
			       "initializing MCP", "mcp", 0);
	rc = cli_init_mcp(ctx);
	if (rc < 0) {
		cli_emit_startup_event(ctx, "startup.component.failed",
				       "failed", "MCP failed", "mcp", rc);
		goto fail;
	}
	cli_emit_startup_event(ctx, "startup.component.ready", "ready",
			       "MCP ready", "mcp", 0);

	cli_emit_startup_event(ctx, "startup.component.begin", "begin",
			       "initializing sub-agents", "sub_agents", 0);
	rc = cli_init_sub_agents(ctx);
	if (rc < 0) {
		cli_emit_startup_event(ctx, "startup.component.failed",
				       "failed", "sub-agents failed",
				       "sub_agents", rc);
		goto fail;
	}
	cli_emit_startup_event(ctx, "startup.component.ready", "ready",
			       "sub-agents ready", "sub_agents", 0);

	rc = session_create(&ctx->database, ctx->current_session.name,
			    ctx->config.models.text.model, &ctx->current_session);
	if (rc == -EEXIST) {
		rc = session_get_by_name(&ctx->database, ctx->current_session.name,
					&ctx->current_session);
		if (rc < 0) {
			log_err("failed to get default session");
			goto fail;
		}
		ctx->session_auto_named = 0;
		session_update_model(&ctx->database, ctx->current_session.id,
				    ctx->config.models.text.model);
		strncpy(ctx->current_session.model, ctx->config.models.text.model,
			sizeof(ctx->current_session.model) - 1);
		utf8_sanitize_inplace(ctx->current_session.name);
		session_load_history(ctx);
	} else {
		ctx->session_auto_named = 0;
	}

	session_ensure_display_id(&ctx->database, &ctx->current_session);
	cli_update_tool_runtime_context(ctx);

	ctx->running = 1;
	ctx->streaming = 0;
	ctx->image_path[0] = '\0';
	log_info("cli initialized");

	cli_emit_startup_event(ctx, "startup.ready", "ready",
			       "startup ready", "cli", 0);
	return 0;

fail:
	cli_emit_startup_event(ctx, "startup.failed", "failed",
			       "startup failed", "cli", rc);
	cli_shutdown(ctx);
	return rc;
}
