#include "runtime/bootstrap.h"
#include "nb/str.h"
#include "nb/utf8.h"

#include "nb/cleanup.h"
#include "agent/compress.h"
#include "agent/guardrail.h"
#include "agent/system_prompt.h"
#include "agent/tokenizer.h"
#include "agent/tools/ask_user.h"
#include "agent/tools/apply_patch.h"
#include "agent/tools/checkpoint.h"
#include "agent/tools/exec_tool.h"
#include "agent/tools/path_policy.h"
#include "agent/tools/config_write.h"
#include "agent/tools/dynamic_tools.h"
#include "agent/tools/file_info.h"
#include "agent/tools/file_list.h"
#include "agent/tools/file_read.h"
#include "agent/tools/img_compose.h"
#include "agent/tools/img_convert.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_inpaint.h"
#include "agent/tools/img_info.h"
#include "agent/tools/img_qa.h"
#include "agent/tools/img_resize.h"
#include "agent/tools/plan.h"
#include "agent/tools/runtime_query.h"
#include "agent/tools/request_permissions.h"
#include "agent/tools/skill_activate.h"
#include "agent/tools/multi_agents.h"
#include "agent/tools/vid_gen.h"
#include "agent/memory.h"
#include "mcp/mcp.h"
#include "models/image_gen.h"
#include "models/video_gen.h"
#include "runtime/usage.h"
#include "runtime/runtime_internal.h"
#include "runtime/resources.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *runtime_api_key(const struct config_model_entry *model)
{
	if (!model)
		return "";
	if (model->api_key[0])
		return model->api_key;
	return getenv(model->api_key_env);
}

static void runtime_history_add_secret(struct react_context *react, const char *value)
{
	if (!react || !value || strlen(value) < 4 ||
	    react->history_secret_count >= HISTORY_SECRET_MAX)
		return;
	for (int i = 0; i < react->history_secret_count; i++)
		if (strcmp(react->history_secrets[i], value) == 0)
			return;
	react->history_secrets[react->history_secret_count] = strdup(value);
	if (react->history_secrets[react->history_secret_count])
		react->history_secret_count++;
}

#define HISTORY_SECRET_ENV_NAME_MAX 256

static int runtime_secret_env_name(const char *name)
{
	static const char *const markers[] = {"KEY", "TOKEN", "SECRET", "PASSWORD"};
	char upper[HISTORY_SECRET_ENV_NAME_MAX];
	size_t len;

	if (!name)
		return 0;
	len = strlen(name);
	if (len >= sizeof(upper))
		len = sizeof(upper) - 1;
	for (size_t i = 0; i < len; i++)
		upper[i] = (char)toupper((unsigned char)name[i]);
	upper[len] = '\0';
	for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
		if (strstr(upper, markers[i]))
			return 1;
	return 0;
}

static void runtime_history_load_secrets(struct react_context *react, const struct config *config)
{
	const struct config_model_entry *models[] = {&config->models.text, &config->models.vision,
						     &config->models.image, &config->models.video,
						     &config->models.approval};

	for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
		runtime_history_add_secret(react, runtime_api_key(models[i]));
	for (int i = 0; i < config->mcp.server_count; i++) {
		const struct config_mcp_server *server = &config->mcp.servers[i];
		const char *token =
			server->http_auth_token_env[0] ? getenv(server->http_auth_token_env) : NULL;

		runtime_history_add_secret(react, token);
		for (int j = 0; j < server->env_count; j++) {
			const char *key = server->env_keys[j];

			if (runtime_secret_env_name(key))
				runtime_history_add_secret(react, server->env_vals[j]);
		}
	}
}

static const struct config_permission_profile *
active_permission_profile(const struct config_react *react)
{
	if (!react || !react->permission_active_profile[0])
		return NULL;
	for (int i = 0; i < react->permission_profile_count; i++)
		if (strcmp(react->permission_profiles[i].name, react->permission_active_profile) ==
		    0)
			return &react->permission_profiles[i];
	return NULL;
}

static void runtime_configure_model(struct model *model, const struct config_model_entry *cfg)
{
	if (!model || !cfg)
		return;
	if (cfg->adapter[0])
		(void)nb_str_copy_to(
			nb_str_from_parts(
				cfg->adapter,
				nb_utf8_clamp_bytes(cfg->adapter, sizeof(model->adapter) - 1)),
			model->adapter, (sizeof(model->adapter) - 1) + 1);
	if (cfg->extra_body_json[0])
		(void)nb_str_copy_to(
			nb_str_from_parts(cfg->extra_body_json,
					  nb_utf8_clamp_bytes(cfg->extra_body_json,
							      sizeof(model->extra_body_json) - 1)),
			model->extra_body_json, (sizeof(model->extra_body_json) - 1) + 1);
	model->timeout_seconds = cfg->timeout_seconds;
	model->retry_count = cfg->retry_count;
	model->poll_interval_seconds = cfg->poll_interval_seconds;
	model->poll_timeout_seconds = cfg->poll_timeout_seconds;
	if (cfg->max_tokens > 0)
		model->max_tokens = cfg->max_tokens;
	if (cfg->context_limit > 0)
		model->context_limit = cfg->context_limit;
}

static struct model *runtime_create_model(struct nb_log *logger,
					  const struct config_model_entry *cfg, int nullable_model)
{
	const char *api_key;
	struct model *model;

	if (!cfg || (!nullable_model && !cfg->model[0]))
		return NULL;
	api_key = runtime_api_key(cfg);
	model = model_llm_create(logger, cfg->provider, cfg->model,
				 cfg->api_base[0] ? cfg->api_base : NULL, api_key ? api_key : "");
	runtime_configure_model(model, cfg);
	return model;
}

static int runtime_apply_system_prompt(struct react_context *react, const struct config *config)
{
	char *content = NULL;
	int rc;

	if (!react || !config)
		return NB_ERR_INVAL;
	rc = morph_prompt_load(react ? react->logger : NULL, config->prompt.system_prompt_file,
			       config->prompt.system_prompt_dir, &content);
	if (rc != 0)
		return rc;
	free(react->system_prompt);
	react->system_prompt = content;
	react->system_prompt_replace = strcmp(config->prompt.mode, "replace") == 0;
	return 0;
}

void runtime_bootstrap_tool_flags(struct tool_registry *tools, const struct config *config)
{
	if (!tools || !config)
		return;
	tool_registry_lock(tools);
	for (int i = 0; i < config->react.readonly_tools_count; i++) {
		struct tool_entry *entry = tool_borrow(tools, config->react.readonly_tools[i]);
		if (entry)
			entry->flags |= TOOL_FLAG_READONLY | TOOL_FLAG_READONLY_CONFIGURED;
	}
	tool_registry_unlock(tools);
}

static enum guardrail_hook runtime_guardrail_hook(const char *name)
{
	return !strcmp(name, "input")	      ? GUARDRAIL_HOOK_INPUT
	       : !strcmp(name, "tool_output") ? GUARDRAIL_HOOK_TOOL_OUTPUT
					      : GUARDRAIL_HOOK_OUTPUT;
}

static int runtime_apply_guardrails(struct react_context *react, const struct config *config,
				    struct model *llm)
{
	if (!react || !config)
		return NB_ERR_INVAL;
	react->guardrail.logger = react->logger;
	react->guardrail.enabled = config->react.guardrail_enabled;
	react->guardrail.max_retries = config->react.guardrail_max_retries;
	react->guardrail.max_empty_rounds = config->react.guardrail_max_empty_rounds;
	if (react->guardrail.rule_count == 0)
		guardrail_register_builtin_rules(&react->guardrail);
	for (int i = 0; i < config->react.guardrail_llm_rule_count; i++) {
		const struct config_guardrail_llm_rule *cr = &config->react.guardrail_llm_rules[i];
		int rc = guardrail_rule_register(
			&react->guardrail, cr->name, runtime_guardrail_hook(cr->hook),
			GUARDRAIL_RULE_LLM, NULL, cr->description, NULL, cr->action_text);
		if (rc != 0)
			return rc;
	}
	for (int i = 0; i < config->react.guardrail_ext_rule_count; i++) {
		const struct config_guardrail_ext_rule *cr = &config->react.guardrail_ext_rules[i];
		int rc = guardrail_rule_register(
			&react->guardrail, cr->name, runtime_guardrail_hook(cr->hook),
			GUARDRAIL_RULE_EXT, NULL, cr->name, cr->ext_entry, cr->action_text);
		if (rc != 0)
			return rc;
		struct guardrail_rule *configured =
			guardrail_rule_borrow(&react->guardrail, cr->name);
		configured->block_on_error = !strcmp(cr->on_error, "block");
		if (!strcmp(cr->ext_type, "so")) {
			configured->ext_type = GUARDRAIL_EXT_SO;
			rc = guardrail_ext_so_load(react->logger, configured);
			if (rc != 0)
				return rc;
		}
	}
	if (llm && config->react.guardrail_llm_model[0]) {
		react->guardrail_model = model_llm_clone(llm, react->logger);
		if (!react->guardrail_model)
			return NB_ERR_NOMEM;
		snprintf(react->guardrail_model->model_id, sizeof(react->guardrail_model->model_id),
			 "%s", config->react.guardrail_llm_model);
		react->guardrail_model->usage_sink = llm->usage_sink;
		react->guardrail_model->budget_sink = llm->budget_sink;
	}
	if (llm)
		guardrail_set_llm(&react->guardrail,
				  react->guardrail_model ? react->guardrail_model : llm);
	for (int i = 0; i < config->react.guardrail_disabled_rule_count; i++)
		(void)guardrail_rule_disable(&react->guardrail,
					     config->react.guardrail_disabled_rules[i]);
	return 0;
}

int runtime_bootstrap_models(struct runtime_bootstrap_profile *profile)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct compress_config compress_cfg;
	struct guardrail_config guardrail_cfg;
	struct runtime_models *models;
	struct config *config;

	if (!profile || !profile->config || !profile->tools || !profile->models)
		return NB_ERR_INVAL;
	config = profile->config;
	models = profile->models;
	if (config->models.image.model[0] &&
	    !image_gen_adapter_supported(config->models.image.provider,
					 config->models.image.adapter)) {
		nb_log_write(profile ? profile->logger : NULL, NB_LOG_ERROR,
			     "unsupported image adapter '%s' for provider '%s'",
			     config->models.image.adapter[0] ? config->models.image.adapter
							     : "(auto)",
			     config->models.image.provider);
		return NB_ERR_INVAL;
	}
	if (config->models.video.model[0] &&
	    !video_gen_adapter_supported(config->models.video.provider,
					 config->models.video.adapter)) {
		nb_log_write(profile ? profile->logger : NULL, NB_LOG_ERROR,
			     "unsupported video adapter '%s' for provider '%s'",
			     config->models.video.adapter[0] ? config->models.video.adapter
							     : "(auto)",
			     config->models.video.provider);
		return NB_ERR_INVAL;
	}
	memset(models, 0, sizeof(*models));
	tool_registry_init(profile ? profile->logger : NULL, profile->tools);
	models->tokenizer =
		tokenizer_create(profile ? profile->logger : NULL, config->models.text.model,
				 config->models.text.context_limit);
	if (!models->tokenizer)
		return NB_ERR_NOMEM;
	memset(&compress_cfg, 0, sizeof(compress_cfg));
	compress_cfg.project_doc_max_bytes = config->context.project_doc_max_bytes;
	compress_cfg.max_context_tokens = config->models.text.context_limit;
	compress_cfg.max_history_rounds = config->context.keep_recent_rounds;
	compress_cfg.in_turn_compaction = config->context.in_turn_compaction;
	compress_cfg.protocol_reserve_tokens = config->context.protocol_reserve_tokens;
	compress_cfg.summarize_threshold_ratio = config->context.summarize_threshold_ratio;
	compress_cfg.compress_target_ratio = config->context.compress_target_ratio;
	compress_cfg.tool_result_max_tokens = config->context.tool_result_max_tokens;
	compress_cfg.compaction_user_message_tokens =
		config->context.compaction_user_message_tokens;
	compress_cfg.compaction_summary_max_tokens = config->context.compaction_summary_max_tokens;
	compress_cfg.compaction_warning_count = config->context.compaction_warning_count;
	memset(&guardrail_cfg, 0, sizeof(guardrail_cfg));
	guardrail_cfg.enabled = config->react.guardrail_enabled;
	guardrail_cfg.max_retries = config->react.guardrail_max_retries;
	guardrail_cfg.max_empty_rounds = config->react.guardrail_max_empty_rounds;
	models->react = react_context_create(profile->tools, models->tokenizer, &compress_cfg,
					     &guardrail_cfg);
	if (!models->react)
		return NB_ERR_NOMEM;
	react_set_event_callback(models->react, profile->event_cb, profile->event_user_data);
	models->react->tool_timeout_seconds = config->react.tool_timeout_seconds;
	models->react->tool_max_retries = config->react.tool_max_retries;
	models->react->max_iterations = config->react.max_iterations;
	models->react->history_tool_result_tokens = config->context.tool_result_max_tokens;
	runtime_history_load_secrets(models->react, config);
	/* Runtime authorization is supplied by morph-permission, not legacy HITL knobs. */
	if (profile->hitl_cb) {
		models->react->hitl.approval_cb = profile->hitl_cb;
		models->react->hitl.approval_user_data = profile->hitl_user_data;
	}
	if (profile->workdir && profile->workdir[0])
		models->react->workdir = strdup(profile->workdir);
	if (config->context.compaction_prompt_file[0]) {
		char *path = (nb_path_expand(&file_value_1, config->context.compaction_prompt_file,
					     NULL) == NB_OK
				      ? strdup(nb_buf_cstr(&file_value_1))
				      : NULL);

		if (path)
			models->react->history_compaction_prompt =
				(nb_file_read_all(path, &file_value_2, 0, NULL) == NB_OK &&
						 (file_copy_2 = (char *)malloc(
							  nb_buf_len(&file_value_2) + 1)) != NULL
					 ? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
						   nb_buf_len(&file_value_2) + 1),
					    nb_charp_steal(&file_copy_2))
					 : NULL);
		if (!models->react->history_compaction_prompt)
			nb_log_write(profile ? profile->logger : NULL, NB_LOG_WARN,
				     "cannot read compaction prompt '%s'; using built-in",
				     config->context.compaction_prompt_file);
		free(path);
	}
	int prompt_rc = runtime_apply_system_prompt(models->react, config);
	if (prompt_rc != 0) {
		runtime_bootstrap_cleanup_models(models);
		return prompt_rc;
	}
	models->text = runtime_create_model(profile->logger, &config->models.text, 0);
	/* Background extraction must not mutate the foreground model or bill its turn. */
	models->memory_text = runtime_create_model(profile->logger, &config->models.text, 0);
	/* Durable jobs own retry/backoff; a failed background request returns promptly. */
	if (models->memory_text)
		models->memory_text->retry_count = 0;
	models->react->logger = profile->logger;
	models->react->llm_model = models->text;
	models->vision = runtime_create_model(profile->logger, &config->models.vision, 0);
	models->image = runtime_create_model(profile->logger, &config->models.image, 1);
	models->video = runtime_create_model(profile->logger, &config->models.video, 1);
	struct model *all[] = {models->text, models->vision, models->image, models->video};

	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		if (all[i]) {
			all[i]->usage_sink = profile->usage_sink;
			all[i]->http.logger = profile->logger;
		}
	}
	return runtime_apply_guardrails(models->react, config, models->text);
}

static int runtime_discover_skills(struct runtime_bootstrap_profile *profile)
{
	struct skill_registry *skills;

	if (!profile || !profile->skills || !profile->models)
		return NB_ERR_INVAL;
	if (profile->allocate_skill_registry) {
		*profile->skills = calloc(1, sizeof(**profile->skills));
		if (!*profile->skills)
			return NB_ERR_NOMEM;
	}
	if (!*profile->skills)
		return 0;
	skills = *profile->skills;
	skill_registry_init(profile ? profile->logger : NULL, skills);
	int discover_rc = skill_discover_configured(
		skills, profile->workdir ? profile->workdir : ".", profile->config->skill.dir);

	if (discover_rc < 0)
		return discover_rc;

	{
		char *builtin = morph_data_find_alloc("skills");

		if (builtin) {
			skill_discover(skills, builtin);
			free(builtin);
		}
	}
	if (skills->count > 0)
		skill_activate_init(profile->tools, skills);
	profile->models->react->skills = skills;
	return 0;
}

int runtime_bootstrap_tools(struct runtime_bootstrap_profile *profile)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_3 = NB_OK;

	struct config *config;
	struct tool_context *tctx;
	struct runtime_models *models;
	int rc = 0;

	if (!profile || !profile->config || !profile->db || !profile->tools ||
	    !profile->tool_context || !profile->models)
		return NB_ERR_INVAL;
	config = profile->config;
	models = profile->models;
	if (!*profile->tool_context) {
		*profile->tool_context =
			tool_context_create(profile ? profile->logger : NULL, profile->workdir,
					    config->general.output_dir);
		if (!*profile->tool_context)
			return NB_ERR_NOMEM;
	}
	tctx = *profile->tool_context;
	profile->tools->permission_context = tctx;
	rc = tool_context_set_permission_policy(tctx, &config->permissions);
	if (rc != 0)
		return rc;
	rc = tool_context_set_grant_store(tctx, profile->db, profile->workdir);
	if (rc != 0)
		nb_log_write(profile ? profile->logger : NULL, NB_LOG_WARN,
			     "failed to load persistent permission grants: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	if (profile->operation_approval_cb)
		tool_context_set_operation_approval(tctx, profile->operation_approval_cb,
						    profile->operation_approval_user_data);
	tool_context_set_default_timeout(tctx, config->react.tool_timeout_seconds);
	rc = tool_path_policy_init(tctx);
	if (rc != 0)
		return rc;
	const struct config_permission_profile *permission_profile;

	permission_profile = active_permission_profile(&config->react);
	if (config->react.permission_active_profile[0] && !permission_profile) {
		nb_log_write(profile ? profile->logger : NULL, NB_LOG_ERROR,
			     "active permission profile not found: %s",
			     config->react.permission_active_profile);
		return NB_ERR_NOT_FOUND;
	}
	if (permission_profile) {
		for (int i = 0; i < permission_profile->workspace_roots_count; i++) {
			rc = tool_context_add_exec_profile_path(
				tctx, TOOL_PATH_WRITE, permission_profile->workspace_roots[i]);
			if (rc == 0)
				rc = tool_context_add_exec_profile_path(
					tctx, TOOL_PATH_DELETE,
					permission_profile->workspace_roots[i]);
			if (rc != 0)
				return rc;
		}
		for (int i = 0; i < permission_profile->write_paths_count; i++) {
			rc = tool_context_add_exec_profile_path(tctx, TOOL_PATH_WRITE,
								permission_profile->write_paths[i]);
			if (rc != 0)
				return rc;
		}
		for (int i = 0; i < permission_profile->delete_paths_count; i++) {
			rc = tool_context_add_exec_profile_path(
				tctx, TOOL_PATH_DELETE, permission_profile->delete_paths[i]);
			if (rc != 0)
				return rc;
		}
	}
	if (config->skill.dir[0]) {
		char *dir = ((expand_status_3 = nb_path_expand(&file_value_3, config->skill.dir,
			NULL)) == NB_OK
				     ? strdup(nb_buf_cstr(&file_value_3))
				     : NULL);

		if (!dir)
			return expand_status_3 != NB_OK ? expand_status_3 : NB_ERR_NOMEM;
		rc = nb_file_mkdir(dir, true, NULL);
		if (rc == 0)
			rc = tool_context_add_builtin_path(tctx, TOOL_PATH_READ, dir);
		if (rc == 0)
			rc = tool_context_add_builtin_path(tctx, TOOL_PATH_WRITE, dir);
		if (rc == 0)
			rc = tool_context_add_builtin_path(tctx, TOOL_PATH_DELETE, dir);
		free(dir);
		if (rc != 0)
			return rc;
	}
	runtime_query_tools_init(profile->tools);
	if (config->models.vision.model[0] && models->vision)
		img_qa_init(profile->tools, models->vision, tctx);
	else
		nb_log_write(profile ? profile->logger : NULL, NB_LOG_INFO,
			     "img_qa unavailable: [model.vision] is not configured");
	img_gen_init(profile->tools, models->image, tctx);
	img_inpaint_init(profile->tools, models->image, tctx);
	img_compose_init(profile->tools, models->image, tctx);
	img_info_init(profile->tools, tctx);
	img_resize_init(profile->tools, tctx);
	img_convert_init(profile->tools, tctx);
	vid_gen_init(profile->tools, models->video, tctx);
	file_read_init(profile->tools, tctx);
	file_list_init(profile->tools, tctx);
	file_info_init(profile->tools, tctx);
	if (profile->enable_apply_patch) {
		rc = apply_patch_init(profile->tools, tctx);
		if (rc != 0)
			return rc;
		rc = checkpoint_init(profile->tools);
		if (rc != 0)
			return rc;
	}
#ifndef MORPH_NO_SHELL
	if (profile->enable_config_write)
		config_edit_init(profile->tools, tctx, profile->config_path);
	if (profile->enable_exec) {
		rc = exec_tool_init(profile->tools, tctx, &config->exec);
		if (rc != 0)
			return rc;
		if (config->react.request_permissions_enabled) {
			rc = request_permissions_init(profile->tools, tctx);
			if (rc != 0)
				return rc;
		}
		tool_set_timeout(profile->tools, "exec", config->react.tool_timeout_seconds);
	}
#else
	if (profile->enable_config_write)
		nb_log_write(profile->logger, NB_LOG_INFO,
			     "config_edit disabled for this runtime build");
	if (profile->enable_exec)
		nb_log_write(profile->logger, NB_LOG_INFO, "exec disabled for this runtime build");
#endif
	if (profile->ask_user_cb)
		ask_user_init(profile->tools, profile->ask_user_cb, profile->ask_user_user_data);
	else
		ask_user_init_deferred(profile->tools);
	if (profile->ask_user_cb) {
		models->react->ask_user_fn = profile->ask_user_cb;
		models->react->ask_user_data = profile->ask_user_user_data;
	}
	if (profile->plans) {
		plan_registry_init(profile->plans);
		rc = plan_tool_init(profile->tools, profile->plans, models->text);
		if (rc == 0 && profile->tool_context)
			plan_tool_set_context(profile->tools, *profile->tool_context);
		if (rc < 0)
			nb_log_write(profile ? profile->logger : NULL, NB_LOG_WARN,
				     "failed to register plan tool: %s",
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	}
	if (profile->task_events)
		rc = scheduled_tasks_tool_init_events(profile->tools, profile->db,
						      profile->task_events);
	else
		rc = scheduled_tasks_tool_init(profile->tools, profile->db);
	if (rc < 0)
		nb_log_write(profile ? profile->logger : NULL, NB_LOG_WARN,
			     "failed to register tasks tool: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
#ifndef MORPH_NO_SHELL
	if (profile->enable_img_annotate) {
		img_annotate_init(profile->tools, profile->img_annotate_pause_cb,
				  profile->img_annotate_resume_cb, profile->img_annotate_user_data,
				  tctx);
	} else {
		nb_log_write(profile ? profile->logger : NULL, NB_LOG_INFO,
			     "img_annotate disabled for this runtime profile");
	}
#else
	if (profile->enable_img_annotate)
		nb_log_write(profile->logger, NB_LOG_INFO,
			     "img_annotate disabled for this runtime build");
	else
		nb_log_write(profile->logger, NB_LOG_INFO,
			     "img_annotate disabled for this runtime profile");
#endif
	rc = runtime_discover_skills(profile);
	if (rc < 0)
		return rc;
	if (profile->platform_tools_cb) {
		rc = profile->platform_tools_cb(profile->tools, tctx,
						profile->platform_tools_user_data);
		if (rc < 0)
			return rc;
	}
	for (int i = 0; i < config->react.disabled_tools_count; i++) {
		const char *name = config->react.disabled_tools[i];
		/* Preserve explicit denials written with the deprecated tool name. */
		if (!strcmp(name, "process")) {
			name = "background";
			nb_log_write(
				profile->logger, NB_LOG_WARN,
				"Disabled tool 'process' is deprecated; use 'background' instead");
		}
		rc = tool_disable(profile->tools, name);
		if (rc)
			return rc;
	}
	runtime_bootstrap_tool_flags(profile->tools, config);
	return 0;
}

int runtime_bootstrap_dynamic_tools(struct runtime_bootstrap_profile *profile,
				    const char *session_id)
{
	if (!profile || !profile->tool_context || !*profile->tool_context || !profile->tools ||
	    !profile->config || !session_id)
		return NB_ERR_INVAL;
	return dynamic_tools_init(profile->tools, *profile->tool_context,
				  &profile->config->dynamic_tools, session_id);
}

struct runtime_child_state {
	struct skill_registry skills;
	struct model_usage_sink usage_sink;
	struct db *usage_db;
	const struct config *usage_config;
	runtime_usage_observer_fn usage_observer;
	void *usage_observer_user_data;
	char *user_id;
	char *credit_session_id;
	struct model *vision;
	struct model *image;
	struct model *video;
	int discovered;
};

static void runtime_child_state_destroy(void *opaque)
{
	struct runtime_child_state *state = opaque;
	if (!state)
		return;
	skill_registry_cleanup(&state->skills);
	free(state->user_id);
	free(state->credit_session_id);
	model_destroy(state->vision);
	model_destroy(state->image);
	model_destroy(state->video);
	free(state);
}

static void runtime_child_usage(const struct model_usage *usage, void *opaque)
{
	struct runtime_child_state *state = opaque;
	if (!runtime_model_usage_is_billable(usage))
		return;
	(void)runtime_record_model_usage_for_user(state->usage_db, state->usage_config,
						  state->user_id, state->credit_session_id, usage);
	if (state->usage_observer)
		state->usage_observer(usage, state->usage_observer_user_data);
}

static int runtime_child_usage_bind(struct runtime_child_state *state, struct model *model,
				    const struct tool_runtime_context *context,
				    struct runtime *owner)
{
	char *user NB_CLEANUP(nb_charp_cleanup) =
		strdup(context->user_id && *context->user_id ? context->user_id : "local");
	char *session NB_CLEANUP(nb_charp_cleanup) =
		strdup(context->credit_session_id && *context->credit_session_id
			       ? context->credit_session_id
			       : "local");
	if (!user || !session)
		return NB_ERR_NOMEM;
	free(state->user_id);
	free(state->credit_session_id);
	state->user_id = nb_charp_steal(&user);
	state->credit_session_id = nb_charp_steal(&session);
	state->usage_db = owner ? &owner->context.database : context->db;
	state->usage_config = context->config;
	state->usage_observer = owner ? owner->options.usage_observer : NULL;
	state->usage_observer_user_data = owner ? owner->options.usage_observer_user_data : NULL;
	state->usage_sink = (struct model_usage_sink){
		.callback = runtime_child_usage,
		.user_data = state,
	};
	model->usage_sink = &state->usage_sink;
	return 0;
}

typedef int (*runtime_model_tool_init_fn)(struct tool_registry *tools, struct model *model,
					  struct tool_context *context);

static int runtime_child_model_tool(struct tool_registry *tools, const char *name,
				    runtime_model_tool_init_fn initialize, struct model *model)
{
	struct tool_entry *entry = tool_borrow(tools, name);
	if (!entry)
		return 0;
	unsigned int flags = entry->flags;
	int timeout = entry->timeout_seconds;
	int disabled = tool_is_disabled(tools, name);
	int rc = tool_unregister(tools, name);
	if (rc == 0)
		rc = initialize(tools, model, tools->permission_context);
	if (rc != 0)
		return rc;
	entry = tool_borrow(tools, name);
	entry->flags = flags;
	entry->timeout_seconds = timeout;
	return disabled ? tool_disable(tools, name) : 0;
}

static int runtime_child_tools(struct runtime_child_state *state, struct tool_registry *tools,
			       struct model *model, const struct tool_runtime_context *runtime)
{
	model_destroy(state->vision);
	model_destroy(state->image);
	model_destroy(state->video);
	state->vision =
		runtime_create_model(model->http.logger, &runtime->config->models.vision, 1);
	state->image = runtime_create_model(model->http.logger, &runtime->config->models.image, 1);
	state->video = runtime_create_model(model->http.logger, &runtime->config->models.video, 1);
	if ((runtime->config->models.vision.model[0] && !state->vision) ||
	    (runtime->config->models.image.model[0] && !state->image) ||
	    (runtime->config->models.video.model[0] && !state->video))
		return NB_ERR_NOMEM;
	struct model *clients[] = {state->vision, state->image, state->video};
	for (size_t i = 0; i < sizeof(clients) / sizeof(clients[0]); i++) {
		if (clients[i]) {
			clients[i]->usage_sink = model->usage_sink;
			clients[i]->budget_sink = model->budget_sink;
		}
	}
	int rc = runtime_child_model_tool(tools, "img_qa", img_qa_init, state->vision);
	if (rc == 0)
		rc = runtime_child_model_tool(tools, "img_gen", img_gen_init, state->image);
	if (rc == 0)
		rc = runtime_child_model_tool(tools, "img_inpaint", img_inpaint_init, state->image);
	if (rc == 0)
		rc = runtime_child_model_tool(tools, "img_compose", img_compose_init, state->image);
	if (rc == 0)
		rc = runtime_child_model_tool(tools, "vid_gen", vid_gen_init, state->video);
	if (rc == 0 && tool_borrow(tools, "tasks"))
		rc = scheduled_tasks_tool_rebind(tools, runtime->db, runtime->memory_session_id);
	if (rc == 0 && tool_borrow(tools, "tool_create")) {
		char session[32];
		snprintf(session, sizeof(session), "%lld", (long long)runtime->memory_session_id);
		rc = dynamic_tools_rebind(tools, tools->permission_context,
					  &runtime->config->dynamic_tools, session);
	}
	/* Child questions are routed to their parent; they must not borrow a frontend. */
	if (rc == 0 && tool_borrow(tools, "ask_user")) {
		unsigned int flags = tool_borrow(tools, "ask_user")->flags;
		int timeout = tool_borrow(tools, "ask_user")->timeout_seconds;
		int disabled = tool_is_disabled(tools, "ask_user");
		rc = tool_unregister(tools, "ask_user");
		if (rc == 0)
			rc = ask_user_init_deferred(tools);
		if (rc == 0) {
			tool_borrow(tools, "ask_user")->flags = flags;
			tool_borrow(tools, "ask_user")->timeout_seconds = timeout;
		}
		if (rc == 0 && disabled)
			rc = tool_disable(tools, "ask_user");
	}
	if (rc == 0 && tool_borrow(tools, "img_annotate"))
		rc = tool_disable(tools, "img_annotate");
	return rc;
}

int runtime_bootstrap_child(struct tool_registry *tools, struct plan_registry *plans,
			    struct react_context *react, struct model *model,
			    const struct tool_runtime_context *runtime,
			    struct agent_child_state *owner, void *user_data)
{
	if (!tools || !react || !model || !runtime || !owner || !runtime->config)
		return NB_ERR_INVAL;
	if (!owner->data) {
		struct runtime_child_state *state = calloc(1, sizeof(*state));
		if (!state)
			return NB_ERR_NOMEM;
		skill_registry_init(react->logger, &state->skills);
		owner->data = state;
		owner->destroy = runtime_child_state_destroy;
	}
	if (owner->destroy != runtime_child_state_destroy)
		return NB_ERR_INVAL;
	struct runtime_child_state *state = owner->data;
	int usage_rc = runtime_child_usage_bind(state, model, runtime, user_data);
	if (usage_rc != 0)
		return usage_rc;
	if (!state->discovered) {
		int rc = skill_discover_configured(&state->skills,
						   runtime->workdir ? runtime->workdir : ".",
						   runtime->config->skill.dir);
		if (rc < 0)
			return rc;
		char *builtin NB_CLEANUP(nb_charp_cleanup) = morph_data_find_alloc("skills");
		if (builtin) {
			rc = skill_discover(&state->skills, builtin);
			if (rc < 0)
				return rc;
		}
		rc = skill_restore_session(&state->skills, runtime->db, runtime->memory_session_id);
		if (rc != 0)
			return rc;
		state->discovered = 1;
	}
	int rc = plans ? plan_tool_rebind(tools, plans, model) : 0;
	if (rc == 0 && (state->skills.count || tool_borrow(tools, "activate_skill")))
		rc = skill_activate_rebind(tools, &state->skills);
	if (rc != 0)
		return rc;
	if (rc == 0)
		rc = runtime_child_tools(state, tools, model, runtime);
	if (rc != 0)
		return rc;
	react->skills = &state->skills;
	react->tool_timeout_seconds = runtime->config->react.tool_timeout_seconds;
	react->tool_max_retries = runtime->config->react.tool_max_retries;
	return runtime_apply_guardrails(react, runtime->config, model);
}

int runtime_bootstrap_agents(struct runtime_bootstrap_profile *profile, struct agent_control **out)
{
	if (!profile || !profile->config || !profile->models || !out || *out)
		return NB_ERR_INVAL;
	if (!profile->enable_multi_agent || !profile->config->agents.enabled)
		return 0;
	*out = agent_control_create(profile->tools, profile->models->text,
				    profile->models->tokenizer, &profile->models->react->compress,
				    &profile->config->agents, profile->db);
	if (!*out)
		return NB_ERR_NOMEM;
	agent_control_set_event_callback(*out, profile->event_cb, profile->event_user_data);
	profile->models->react->agents = *out;
	int rc = agent_control_set_child_initializer(*out, runtime_bootstrap_child, profile->owner);
	if (rc != 0)
		return rc;
	return multi_agents_register(profile->tools, *out);
}

void runtime_bootstrap_cleanup_models(struct runtime_models *models)
{
	if (!models)
		return;
	if (models->react) {
		react_context_destroy(models->react);
	}
	if (models->memory_text)
		model_destroy(models->memory_text);
	if (models->text)
		model_destroy(models->text);
	if (models->vision)
		model_destroy(models->vision);
	if (models->image)
		model_destroy(models->image);
	if (models->video)
		model_destroy(models->video);
	if (models->tokenizer)
		tokenizer_destroy(models->tokenizer);
	memset(models, 0, sizeof(*models));
}

void runtime_bootstrap_cleanup(struct runtime_shutdown_resources *r)
{
	if (!r)
		return;

	if (r->memory_worker) {
		memory_worker_destroy(*r->memory_worker);
		*r->memory_worker = NULL;
	}

	if (r->react && *r->react) {
		react_context_destroy(*r->react);
		*r->react = NULL;
	}
	if (r->agents && *r->agents) {
		agent_control_destroy(*r->agents);
		*r->agents = NULL;
	}
	if (r->tokenizer && *r->tokenizer) {
		tokenizer_destroy(*r->tokenizer);
		*r->tokenizer = NULL;
	}
	if (r->tools)
		tool_registry_cleanup(r->tools);
	/* Join process workers while their callback context and database remain alive. */
	exec_tool_jobs_release(r->jobs_owner);
	if (r->tool_context && *r->tool_context) {
		tool_context_destroy(*r->tool_context);
		*r->tool_context = NULL;
	}

	if (r->memory_text && *r->memory_text) {
		model_destroy(*r->memory_text);
		*r->memory_text = NULL;
	}
	if (r->text && *r->text) {
		model_destroy(*r->text);
		*r->text = NULL;
	}
	if (r->vision && *r->vision) {
		model_destroy(*r->vision);
		*r->vision = NULL;
	}
	if (r->image && *r->image) {
		model_destroy(*r->image);
		*r->image = NULL;
	}
	if (r->video && *r->video) {
		model_destroy(*r->video);
		*r->video = NULL;
	}

	if (r->skills && *r->skills) {
		skill_registry_cleanup(*r->skills);
		if (r->free_skill_registry) {
			free(*r->skills);
			*r->skills = NULL;
		}
	}
	if (r->mcp)
		mcp_registry_cleanup(r->mcp);
	if (r->db)
		db_close(r->db);
}
