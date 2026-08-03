#include "runtime/bootstrap.h"

#include "agent/compress.h"
#include "agent/guardrail.h"
#include "agent/tokenizer.h"
#include "agent/tools/ask_user.h"
#include "agent/tools/bash_exec.h"
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
#include "agent/tools/sub_agent_tools.h"
#include "agent/tools/vid_gen.h"
#include "agent/memory.h"
#include "mcp/mcp.h"
#include "models/image_gen.h"
#include "models/video_gen.h"
#include "runtime/usage.h"
#include "util/data.h"
#include "util/error.h"
#include "util/file.h"
#include "util/log.h"

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

static void runtime_history_add_secret(struct react_context *react,
				       const char *value)
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
	static const char *const markers[] = {
		"KEY", "TOKEN", "SECRET", "PASSWORD"
	};
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

static void runtime_history_load_secrets(struct react_context *react,
					 const struct config *config)
{
	const struct config_model_entry *models[] = {
		&config->models.text, &config->models.vision,
		&config->models.image, &config->models.video
	};

	for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
		runtime_history_add_secret(react, runtime_api_key(models[i]));
	for (int i = 0; i < config->mcp.server_count; i++) {
		const struct config_mcp_server *server = &config->mcp.servers[i];
		const char *token = server->http_auth_token_env[0] ?
			getenv(server->http_auth_token_env) : NULL;

		runtime_history_add_secret(react, token);
		for (int j = 0; j < server->env_count; j++) {
			const char *key = server->env_keys[j];

			if (runtime_secret_env_name(key))
				runtime_history_add_secret(react,
					server->env_vals[j]);
		}
	}
}

static const struct config_permission_profile *active_permission_profile(
	const struct config_react *react)
{
	if (!react || !react->permission_active_profile[0])
		return NULL;
	for (int i = 0; i < react->permission_profile_count; i++)
		if (strcmp(react->permission_profiles[i].name,
			   react->permission_active_profile) == 0)
			return &react->permission_profiles[i];
	return NULL;
}

static void runtime_configure_model(struct model *model,
				    const struct config_model_entry *cfg)
{
	if (!model || !cfg)
		return;
	if (cfg->adapter[0])
		strncpy(model->adapter, cfg->adapter,
			sizeof(model->adapter) - 1);
	if (cfg->extra_body_json[0])
		strncpy(model->extra_body_json, cfg->extra_body_json,
			sizeof(model->extra_body_json) - 1);
	model->timeout_seconds = cfg->timeout_seconds;
	model->retry_count = cfg->retry_count;
	if (cfg->max_tokens > 0)
		model->max_tokens = cfg->max_tokens;
	if (cfg->context_limit > 0)
		model->context_limit = cfg->context_limit;
}

static struct model *runtime_create_model(const struct config_model_entry *cfg,
					  int nullable_model)
{
	const char *api_key;
	struct model *model;

	if (!cfg || (!nullable_model && !cfg->model[0]))
		return NULL;
	api_key = runtime_api_key(cfg);
	model = model_llm_create(cfg->provider,
				 cfg->model[0] ? cfg->model : NULL,
				 cfg->api_base[0] ? cfg->api_base : NULL,
				 api_key ? api_key : "");
	runtime_configure_model(model, cfg);
	return model;
}

static int runtime_apply_system_prompt(struct react_context *react,
				       const struct config *config)
{
	if (!react || !config)
		return -EINVAL;
	if (config->prompt.system_prompt_file[0]) {
		char *exp = file_expand_path(config->prompt.system_prompt_file);
		if (exp) {
			char *content = file_read_all(exp, NULL);
			if (content) {
				size_t len = strlen(content);
				while (len > 0 &&
				       (content[len - 1] == '\n' ||
					content[len - 1] == '\r' ||
					content[len - 1] == ' '))
					content[--len] = '\0';
				react->system_prompt = content;
				log_info("loaded system prompt: %s",
					 config->prompt.system_prompt_file);
			} else {
				log_warn("failed to read system prompt: %s",
					 config->prompt.system_prompt_file);
			}
			free(exp);
		}
	}
	if (config->prompt.system_prompt_dir[0]) {
		char *exp = file_expand_path(config->prompt.system_prompt_dir);
		char **files = NULL;
		int nfiles = 0;

		if (exp && file_list_files(exp, &files, &nfiles) == 0) {
			for (int i = 0; i < nfiles; i++) {
				char full[PATH_MAX];
				char *content;
				size_t clen;
				char *old;
				char *combined;
				size_t old_len;

				if (file_path_join(full, sizeof(full), exp,
						   files[i]) != 0)
					continue;
				content = file_read_all(full, NULL);
				if (!content)
					continue;
				clen = strlen(content);
				while (clen > 0 &&
				       (content[clen - 1] == '\n' ||
					content[clen - 1] == '\r' ||
					content[clen - 1] == ' '))
					content[--clen] = '\0';
				if (!clen) {
					free(content);
					continue;
				}
				old = react->system_prompt;
				old_len = old ? strlen(old) : 0;
				combined = malloc(old_len + 3 + clen + 1);
				if (combined) {
					if (old) {
						memcpy(combined, old, old_len);
						combined[old_len] = '\n';
						combined[old_len + 1] = '\n';
						memcpy(combined + old_len + 2,
						       content, clen + 1);
					} else {
						memcpy(combined, content,
						       clen + 1);
					}
					react->system_prompt = combined;
					free(old);
				}
				free(content);
			}
			file_free_list(files, nfiles);
			log_info("loaded %d prompt files from: %s", nfiles,
				 config->prompt.system_prompt_dir);
		}
		free(exp);
	}
	return 0;
}

static void runtime_apply_tool_flags(struct tool_registry *tools,
				     const struct config *config)
{
	if (!tools || !config)
		return;
	for (int i = 0; i < config->react.readonly_tools_count; i++) {
		struct tool_entry *entry =
			tool_lookup(tools, config->react.readonly_tools[i]);
		if (entry)
			entry->flags |= TOOL_FLAG_READONLY;
	}
}

static void runtime_apply_guardrails(struct react_context *react,
				     const struct config *config,
				     struct model *llm)
{
	if (!react || !config)
		return;
	guardrail_register_builtin_rules(&react->guardrail);
	for (int i = 0; i < config->react.guardrail_llm_rule_count; i++) {
		const struct config_guardrail_llm_rule *cr =
			&config->react.guardrail_llm_rules[i];
		enum guardrail_hook hook = strcmp(cr->hook, "input") == 0
			? GUARDRAIL_HOOK_INPUT : GUARDRAIL_HOOK_OUTPUT;
		guardrail_rule_register(&react->guardrail, cr->name, hook,
					GUARDRAIL_RULE_LLM, NULL,
					cr->description, NULL,
					cr->action_text);
	}
	for (int i = 0; i < config->react.guardrail_ext_rule_count; i++) {
		const struct config_guardrail_ext_rule *cr =
			&config->react.guardrail_ext_rules[i];
		enum guardrail_hook hook = strcmp(cr->hook, "input") == 0
			? GUARDRAIL_HOOK_INPUT : GUARDRAIL_HOOK_OUTPUT;
		guardrail_rule_register(&react->guardrail, cr->name, hook,
					GUARDRAIL_RULE_EXT, NULL,
					cr->ext_type[0] == '\0' ||
					strcmp(cr->ext_type, "exec") == 0 ?
					NULL : cr->ext_type,
					cr->ext_entry, cr->action_text);
		if (strcmp(cr->ext_type, "so") == 0) {
			struct guardrail_rule *r =
				guardrail_rule_lookup(&react->guardrail,
						      cr->name);
			if (r) {
				r->ext_type = GUARDRAIL_EXT_SO;
				guardrail_ext_so_load(r);
			}
		}
	}
	if (llm)
		guardrail_set_llm(&react->guardrail, llm);
	for (int i = 0; i < config->react.guardrail_disabled_rule_count; i++)
		guardrail_rule_disable(&react->guardrail,
			config->react.guardrail_disabled_rules[i]);
}

int runtime_bootstrap_models(struct runtime_bootstrap_profile *profile)
{
	struct compress_config compress_cfg;
	struct guardrail_config guardrail_cfg;
	struct runtime_models *models;
	struct config *config;

	if (!profile || !profile->config || !profile->tools ||
	    !profile->models)
		return -EINVAL;
	config = profile->config;
	models = profile->models;
	if (config->models.image.model[0] &&
	    !image_gen_adapter_supported(config->models.image.provider,
					 config->models.image.adapter)) {
		log_err("unsupported image adapter '%s' for provider '%s'",
			config->models.image.adapter[0]
				? config->models.image.adapter : "(auto)",
			config->models.image.provider);
		MORPH_RETURN(-EINVAL);
	}
	memset(models, 0, sizeof(*models));
	tool_registry_init(profile->tools);
	models->tokenizer = tokenizer_create(config->models.text.model,
					     config->models.text.context_limit);
	if (!models->tokenizer)
		return -ENOMEM;
	memset(&compress_cfg, 0, sizeof(compress_cfg));
	compress_cfg.max_context_tokens = config->models.text.context_limit;
	compress_cfg.max_history_rounds =
		config->context.keep_recent_rounds;
	compress_cfg.summarize_threshold_ratio =
		config->context.summarize_threshold_ratio;
	compress_cfg.compress_target_ratio =
		config->context.compress_target_ratio;
	compress_cfg.tool_result_max_tokens =
		config->context.tool_result_max_tokens;
	compress_cfg.compaction_user_message_tokens =
		config->context.compaction_user_message_tokens;
	compress_cfg.compaction_summary_max_tokens =
		config->context.compaction_summary_max_tokens;
	compress_cfg.compaction_warning_count =
		config->context.compaction_warning_count;
	memset(&guardrail_cfg, 0, sizeof(guardrail_cfg));
	guardrail_cfg.enabled = config->react.guardrail_enabled;
	guardrail_cfg.max_retries = config->react.guardrail_max_retries;
	guardrail_cfg.max_empty_rounds =
		config->react.guardrail_max_empty_rounds;
	models->react = react_context_create(profile->tools,
					     models->tokenizer,
					     &compress_cfg, &guardrail_cfg);
	if (!models->react)
		return -ENOMEM;
	react_set_event_callback(models->react, profile->event_cb,
				 profile->event_user_data);
	models->react->tool_timeout_seconds =
		config->react.tool_timeout_seconds;
	models->react->tool_max_retries = config->react.tool_max_retries;
	models->react->max_iterations = config->react.max_iterations;
	models->react->history_tool_result_tokens =
		config->context.tool_result_max_tokens;
	runtime_history_load_secrets(models->react, config);
	models->react->hitl.enabled = config->react.hitl_enabled;
	models->react->hitl.auto_approve_readonly =
		config->react.hitl_auto_approve_readonly;
	models->react->hitl.tools_count = config->react.hitl_tools_count;
	for (int i = 0; i < config->react.hitl_tools_count &&
	     i < HITL_TOOLS_MAX; i++)
		strncpy(models->react->hitl.tools[i],
			config->react.hitl_tools[i], HITL_TOOL_NAME_MAX - 1);
	if (profile->hitl_cb) {
		models->react->hitl.approval_cb = profile->hitl_cb;
		models->react->hitl.approval_user_data =
			profile->hitl_user_data;
	}
	if (profile->workdir && profile->workdir[0])
		models->react->workdir = strdup(profile->workdir);
	if (config->context.compaction_prompt_file[0]) {
		char *path = file_expand_path(
			config->context.compaction_prompt_file);

		if (path)
			models->react->history_compaction_prompt =
				file_read_all(path, NULL);
		if (!models->react->history_compaction_prompt)
			log_warn("cannot read compaction prompt '%s'; using built-in",
				 config->context.compaction_prompt_file);
		free(path);
	}
	(void)runtime_apply_system_prompt(models->react, config);
	models->text = runtime_create_model(&config->models.text, 0);
	models->react->llm_model = models->text;
	memory_set_llm(models->text);
	model_set_usage_callback(profile->usage_cb);
	model_set_usage_user_data(profile->usage_user_data);
	models->vision = runtime_create_model(&config->models.vision, 0);
	models->image = runtime_create_model(&config->models.image, 1);
	models->video = runtime_create_model(&config->models.video, 1);
	runtime_apply_guardrails(models->react, config, models->text);
#ifndef MORPH_NO_SHELL
	bash_exec_set_default_timeout(
		config->react.bash_exec_default_timeout);
#endif
	return 0;
}

static int runtime_discover_skills(struct runtime_bootstrap_profile *profile)
{
	struct skill_registry *skills;

	if (!profile || !profile->skills || !profile->models)
		return -EINVAL;
	if (profile->allocate_skill_registry) {
		*profile->skills = calloc(1, sizeof(**profile->skills));
		if (!*profile->skills)
			return -ENOMEM;
	}
	if (!*profile->skills)
		return 0;
	skills = *profile->skills;
	skill_registry_init(skills);
	if (profile->config->skill.dir[0]) {
		char *dir = file_expand_path(profile->config->skill.dir);
		if (dir) {
			if (file_exists(dir))
				skill_discover(skills, dir);
			free(dir);
		}
	} else {
		char *morph = file_expand_path("~/.morph/skills");
		char *agents = file_expand_path("~/.agents/skills");
		if (morph) {
			if (!file_exists(morph))
				file_ensure_dir(morph);
			skill_discover(skills, morph);
			free(morph);
		}
		if (agents) {
			if (!file_exists(agents))
				file_ensure_dir(agents);
			skill_discover(skills, agents);
			free(agents);
		}
	}
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
	struct config *config;
	struct tool_context *tctx;
	struct runtime_models *models;
	int rc = 0;

	if (!profile || !profile->config || !profile->db ||
	    !profile->tools || !profile->tool_context || !profile->models)
		return -EINVAL;
	config = profile->config;
	models = profile->models;
	if (!*profile->tool_context) {
		*profile->tool_context =
			tool_context_create(profile->workdir,
					    config->general.output_dir);
		if (!*profile->tool_context)
			return -ENOMEM;
	}
	tctx = *profile->tool_context;
	rc = tool_context_set_grant_store(tctx, profile->db,
					   profile->workdir);
	if (rc != 0)
		log_warn("failed to load persistent permission grants: %s",
			 morph_strerror(rc));
	if (profile->operation_approval_cb)
		tool_context_set_operation_approval(
			tctx, profile->operation_approval_cb,
			profile->operation_approval_user_data);
	tool_context_set_default_timeout(tctx,
					 config->react.tool_timeout_seconds);
	runtime_query_tools_init(profile->tools);
	if (config->models.vision.model[0] && models->vision)
		img_qa_init(profile->tools, models->vision, tctx);
	else
		log_info("img_qa unavailable: [model.vision] is not configured");
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
#ifndef MORPH_NO_SHELL
	if (profile->enable_config_write)
		config_write_init(profile->tools, tctx, profile->config_path);
	if (profile->enable_bash && config->react.bash_exec_enabled) {
		const struct config_permission_profile *permission_profile;

		tool_context_set_bash_exec_mode(
			tctx, config->react.bash_exec_mode);
		permission_profile = active_permission_profile(&config->react);
		if (config->react.permission_active_profile[0] &&
		    !permission_profile) {
			log_err("active permission profile not found: %s",
				  config->react.permission_active_profile);
			MORPH_RETURN(-ENOENT);
		}
		if (permission_profile &&
		    strcmp(config->react.bash_exec_mode, "local") != 0) {
			log_err("permission profiles require bash_exec_mode=local");
			MORPH_RETURN(-EINVAL);
		}
		if (permission_profile) {
			for (int i = 0;
			     i < permission_profile->workspace_roots_count; i++) {
				rc = tool_context_add_bash_exec_profile_path(
					tctx, TOOL_PATH_WRITE,
					permission_profile->workspace_roots[i]);
				if (rc == 0)
					rc = tool_context_add_bash_exec_profile_path(
						tctx, TOOL_PATH_DELETE,
						permission_profile->workspace_roots[i]);
				if (rc != 0)
					return rc;
			}
			for (int i = 0; i < permission_profile->write_paths_count; i++) {
				rc = tool_context_add_bash_exec_profile_path(
					tctx, TOOL_PATH_WRITE,
					permission_profile->write_paths[i]);
				if (rc != 0)
					return rc;
			}
			for (int i = 0; i < permission_profile->delete_paths_count; i++) {
				rc = tool_context_add_bash_exec_profile_path(
					tctx, TOOL_PATH_DELETE,
					permission_profile->delete_paths[i]);
				if (rc != 0)
					return rc;
			}
		}
		tool_context_set_bash_exec_server_network(
			tctx, config->react.bash_exec_server_network_access);
		for (int i = 0;
		     i < config->react.bash_exec_server_allowed_env_count; i++) {
			rc = tool_context_add_bash_exec_server_env(
				tctx, config->react.bash_exec_server_allowed_env[i]);
			if (rc != 0)
				return rc;
		}
		for (int i = 0;
		     i < config->react.bash_exec_allowed_commands_count; i++) {
			rc = tool_context_allow_command_pattern(
				tctx, config->react.bash_exec_allowed_commands[i]);
			if (rc != 0)
				return rc;
		}
		for (int i = 0;
		     i < config->react.bash_exec_server_read_paths_count; i++) {
			rc = tool_context_add_bash_exec_server_path(
				tctx, TOOL_PATH_READ,
				config->react.bash_exec_server_read_paths[i]);
			if (rc != 0)
				return rc;
		}
		for (int i = 0;
		     i < config->react.bash_exec_server_write_paths_count; i++) {
			rc = tool_context_add_bash_exec_server_path(
				tctx, TOOL_PATH_WRITE,
				config->react.bash_exec_server_write_paths[i]);
			if (rc != 0)
				return rc;
		}
		for (int i = 0;
		     i < config->react.bash_exec_server_delete_paths_count; i++) {
			rc = tool_context_add_bash_exec_server_path(
				tctx, TOOL_PATH_DELETE,
				config->react.bash_exec_server_delete_paths[i]);
			if (rc != 0)
				return rc;
		}
		if (config->react.bash_exec_allowed_cwds_count > 0)
			log_warn("react.bash_exec_allowed_cwds is deprecated and "
				 "ignored");
		bash_exec_init(profile->tools, tctx);
		if (config->react.request_permissions_enabled &&
		    strcmp(config->react.bash_exec_mode, "local") == 0) {
			rc = request_permissions_init(profile->tools, tctx);
			if (rc != 0)
				return rc;
		}
		tool_set_timeout(profile->tools, "bash_exec",
				 config->react.bash_exec_default_timeout);
	}
#else
	if (profile->enable_config_write)
		log_info("config_write disabled for this runtime build");
	if (profile->enable_bash)
		log_info("bash_exec disabled for this runtime build");
#endif
	if (profile->ask_user_cb) {
		ask_user_init(profile->tools, profile->ask_user_cb,
			      profile->ask_user_user_data);
		models->react->ask_user_fn = profile->ask_user_cb;
		models->react->ask_user_data = profile->ask_user_user_data;
	}
	if (profile->plans) {
		plan_registry_init(profile->plans);
		rc = plan_tool_init(profile->tools, profile->plans,
				    models->text);
		if (rc < 0)
			log_warn("failed to register plan tool: %s",
				 morph_strerror(rc));
	}
	if (profile->task_events)
		rc = scheduled_tasks_tool_init_events(profile->tools,
						      profile->db,
						      profile->task_events);
	else
		rc = scheduled_tasks_tool_init(profile->tools, profile->db);
	if (rc < 0)
		log_warn("failed to register tasks tool: %s",
			 morph_strerror(rc));
#ifndef MORPH_NO_SHELL
	if (profile->enable_img_annotate) {
		img_annotate_init(profile->tools,
				  profile->img_annotate_pause_cb,
				  profile->img_annotate_resume_cb,
				  profile->img_annotate_user_data, tctx);
	} else {
		log_info("img_annotate disabled for this runtime profile");
	}
#else
	if (profile->enable_img_annotate)
		log_info("img_annotate disabled for this runtime build");
	else
		log_info("img_annotate disabled for this runtime profile");
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
	for (int i = 0; i < config->react.disabled_tools_count; i++)
		tool_disable(profile->tools, config->react.disabled_tools[i]);
	runtime_apply_tool_flags(profile->tools, config);
	return 0;
}

int runtime_bootstrap_dynamic_tools(struct runtime_bootstrap_profile *profile,
				    const char *session_id)
{
	if (!profile || !profile->tool_context || !*profile->tool_context ||
	    !profile->tools || !profile->config || !session_id)
		return -EINVAL;
	return dynamic_tools_init(profile->tools, *profile->tool_context,
				  &profile->config->dynamic_tools, session_id);
}

int runtime_bootstrap_sub_agents(struct runtime_bootstrap_profile *profile,
				 struct sub_agent_runtime **out)
{
	struct sub_agent_runtime *rt;
	int rc;

	if (!profile || !profile->config || !profile->models || !out)
		return -EINVAL;
	if (!profile->enable_sub_agents ||
	    profile->config->sub_agents.count == 0)
		return 0;
	rt = sub_agent_runtime_create(profile->tools, profile->models->text,
				      profile->models->tokenizer,
				      &profile->models->react->compress);
	if (!rt)
		return -ENOMEM;
	sub_agent_runtime_set_event_callback(rt, profile->event_cb,
					     profile->event_user_data);
	rc = sub_agent_runtime_load_config(rt, &profile->config->sub_agents);
	if (rc < 0)
		return rc;
	sub_agent_tools_register_all(profile->tools, rt);
	profile->models->react->sub_agent_depth = 0;
	if (rt->entry_count > 0) {
		profile->models->react->sub_agent_info =
			calloc((size_t)rt->entry_count,
			       sizeof(*profile->models->react->sub_agent_info));
		if (profile->models->react->sub_agent_info) {
			for (int i = 0; i < rt->entry_count; i++) {
				strncpy(profile->models->react
						->sub_agent_info[i].name,
					rt->entries[i].cfg.name,
					sizeof(profile->models->react
						->sub_agent_info[i].name) - 1);
				strncpy(profile->models->react
						->sub_agent_info[i].description,
					rt->entries[i].cfg.description,
					sizeof(profile->models->react
						->sub_agent_info[i].description) - 1);
			}
			profile->models->react->sub_agent_info_count =
				rt->entry_count;
		}
	}
	*out = rt;
	return 0;
}

void runtime_bootstrap_cleanup_models(struct runtime_models *models)
{
	if (!models)
		return;
	if (models->text)
		model_destroy(models->text);
	if (models->vision)
		model_destroy(models->vision);
	if (models->image)
		model_destroy(models->image);
	if (models->video)
		model_destroy(models->video);
	if (models->react)
		react_context_destroy(models->react);
	if (models->tokenizer)
		tokenizer_destroy(models->tokenizer);
	memset(models, 0, sizeof(*models));
}

void runtime_bootstrap_cleanup(struct runtime_shutdown_resources *r)
{
	if (!r)
		return;

	if (r->shutdown_memory)
		memory_async_shutdown();

	if (r->react && *r->react) {
		free((*r->react)->sub_agent_info);
		(*r->react)->sub_agent_info = NULL;
		(*r->react)->sub_agent_info_count = 0;
		react_context_destroy(*r->react);
		*r->react = NULL;
	}
	if (r->tokenizer && *r->tokenizer) {
		tokenizer_destroy(*r->tokenizer);
		*r->tokenizer = NULL;
	}
	if (r->tools)
		tool_registry_cleanup(r->tools);
	if (r->tool_context && *r->tool_context) {
		tool_context_destroy(*r->tool_context);
		*r->tool_context = NULL;
	}

	if (r->reset_usage_callbacks) {
		model_set_usage_callback(NULL);
		runtime_usage_restore(NULL);
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
	if (r->sub_agents && *r->sub_agents) {
		sub_agent_runtime_destroy(*r->sub_agents);
		*r->sub_agents = NULL;
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
