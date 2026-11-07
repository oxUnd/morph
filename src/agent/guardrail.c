#include "util/signals.h"
#include "util/child_io.h"
#include <spawn.h>
#include "guardrail.h"
#include "react.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
#include "ipc/jsonrpc.h"
#include "cJSON.h"
#include "re.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dlfcn.h>

/* ── internal helpers ── */

static int is_creative_tool(const char *name)
{
	if (!name) return 0;
	static const char *const creative[] = {
		"img_gen", "img_inpaint", "img_compose",
		"img_resize", "img_convert",
		"vid_gen", NULL
	};
	for (const char *const *t = creative; *t; t++)
		if (strcmp(name, *t) == 0) return 1;
	return 0;
}

struct tool_outcome {
	char name[64];
	int succeeded;
	int is_creative;
	int has_file_output;
	int terminal_failure;
};

/* Explicit policy/permission blockers must not trigger forced tool retries. */
enum guardrail_retry_policy guardrail_tool_retry_policy(const struct react_step *observation)
{
	const char *text = observation ? observation->content : NULL;
	static const char prefix[] = "tool error: ";
	cJSON *payload;
	const cJSON *retry;
	const cJSON *reason;
	int terminal;
	int blocked;

	if (!observation || observation->error_code >= 0)
		return GUARDRAIL_RETRY_UNSPECIFIED;
	if (text && strncmp(text, prefix, sizeof(prefix) - 1) == 0)
		text += sizeof(prefix) - 1;
	payload = text ? cJSON_ParseWithOpts(text, NULL, 1) : NULL;
	retry = cJSON_GetObjectItemCaseSensitive(payload, "retry");
	terminal = cJSON_IsString(retry) && strcmp(retry->valuestring, "never") == 0;
	reason = cJSON_GetObjectItemCaseSensitive(payload, "reason");
	blocked = terminal && cJSON_IsString(reason) &&
		strcmp(reason->valuestring, "screen_locked") == 0;
	cJSON_Delete(payload);
	if (blocked)
		return GUARDRAIL_RETRY_BLOCKED;
	if (terminal)
		return GUARDRAIL_RETRY_NEVER;
	if (observation->error_code == -EPERM ||
	    observation->error_code == -EACCES ||
	    observation->error_code == -ECANCELED ||
	    observation->error_code == MORPH_ERR_NOT_CONFIGURED)
		return GUARDRAIL_RETRY_NEEDS_USER;
	return GUARDRAIL_RETRY_UNSPECIFIED;
}

static int build_tool_outcomes(const struct react_step *steps,
			       struct tool_outcome *out,
			       int max_out)
{
	int count = 0;
	const struct react_step *cur = steps;
	const struct react_step *obs_cursor = steps;
	while (cur && count < max_out) {
		if (cur->type == REACT_STEP_ACTION && cur->tool_name) {
			struct tool_outcome *o = &out[count++];
			strncpy(o->name, cur->tool_name, sizeof(o->name) - 1);
			o->name[sizeof(o->name) - 1] = '\0';
			o->is_creative = is_creative_tool(cur->tool_name);
			o->succeeded = 0;
			o->has_file_output = 0;
			o->terminal_failure = 0;
			const struct react_step *obs = obs_cursor;
			while (obs) {
				if (obs->type == REACT_STEP_OBSERVATION) {
					if (obs->error_code < 0) {
						enum guardrail_retry_policy policy =
							guardrail_tool_retry_policy(obs);

						o->succeeded = 0;
						o->terminal_failure =
							policy != GUARDRAIL_RETRY_UNSPECIFIED;
					} else {
						o->succeeded = 1;
						if (obs->artifact_count > 0)
							o->has_file_output = 1;
					}
					break;
				}
				obs = obs->next;
			}
			obs_cursor = obs ? obs->next : NULL;
		}
		cur = cur->next;
	}
	return count;
}

/* ── 5 built-in C rules ── */

static enum guardrail_verdict
gr_empty_answer(const struct guardrail_eval_ctx *ctx,
		char *reason_out, size_t reason_cap)
{
	if (!ctx->proposed_answer || !*ctx->proposed_answer ||
	    strcmp(ctx->proposed_answer, "(no response)") == 0) {
		snprintf(reason_out, reason_cap, "Empty answer.");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

static enum guardrail_verdict
gr_consecutive_empty(const struct guardrail_eval_ctx *ctx,
		     char *reason_out, size_t reason_cap)
{
	if (ctx->max_empty_rounds > 0 &&
	    ctx->empty_round_count >= ctx->max_empty_rounds) {
		snprintf(reason_out, reason_cap,
			 "Multiple empty responses. Provide a substantive answer.");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

static enum guardrail_verdict
gr_tools_all_failed(const struct guardrail_eval_ctx *ctx,
		    char *reason_out, size_t reason_cap)
{
	struct tool_outcome outcomes[64];
	int count = build_tool_outcomes((const struct react_step *)ctx->steps,
					outcomes, 64);
	if (count == 0) return GUARDRAIL_PASS;
	int all_failed = 1;
	char failed_names[256] = {0};
	size_t fn_len = 0;
	for (int i = 0; i < count; i++) {
		if (outcomes[i].terminal_failure)
			return GUARDRAIL_PASS;
		if (outcomes[i].succeeded) {
			all_failed = 0;
		} else {
			size_t nlen = strlen(outcomes[i].name);
			if (fn_len + nlen + 2 < sizeof(failed_names)) {
				if (fn_len > 0) {
					strcat(failed_names, ", ");
					fn_len += 2;
				}
				strcat(failed_names, outcomes[i].name);
				fn_len += nlen;
			}
		}
	}
	if (all_failed) {
		snprintf(reason_out, reason_cap,
			 "All tool calls failed (%s). Check arguments or try different tools.",
			 failed_names);
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

static enum guardrail_verdict
gr_creative_no_media(const struct guardrail_eval_ctx *ctx,
		     char *reason_out, size_t reason_cap)
{
	struct tool_outcome outcomes[64];
	int count = build_tool_outcomes((const struct react_step *)ctx->steps,
					outcomes, 64);
	int any_creative = 0;
	int creative_all_no_file = 1;
	int creative_terminal_failure = 0;
	int creative_succeeded = 0;
	for (int i = 0; i < count; i++) {
		if (outcomes[i].is_creative) {
			any_creative = 1;
			creative_terminal_failure |= outcomes[i].terminal_failure;
			creative_succeeded |= outcomes[i].succeeded;
			if (outcomes[i].has_file_output)
				creative_all_no_file = 0;
		}
	}
	if (creative_terminal_failure && !creative_succeeded)
		return GUARDRAIL_PASS;
	if (any_creative && creative_all_no_file) {
		snprintf(reason_out, reason_cap,
			 "Creative tools were called but no output files were produced.");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

static enum guardrail_verdict
gr_creative_file_missing(const struct guardrail_eval_ctx *ctx,
			 char *reason_out, size_t reason_cap)
{
	if (!ctx->tool_name || !is_creative_tool(ctx->tool_name))
		return GUARDRAIL_PASS;
	if (ctx->tool_error_code < 0)
		return GUARDRAIL_PASS;
	if (!ctx->tool_artifacts)
		return GUARDRAIL_PASS;
	for (int i = 0; i < ctx->tool_artifacts->count; i++) {
		const struct tool_artifact *artifact =
			&ctx->tool_artifacts->items[i];
		if (artifact->path[0] && !file_exists(artifact->path)) {
			snprintf(reason_out, reason_cap,
				 "Creative tool '%s' reported output '%s' "
				 "but the file does not exist on disk.",
				 ctx->tool_name, artifact->path);
			return GUARDRAIL_FAIL;
		}
	}
	return GUARDRAIL_PASS;
}

static int markdown_url_has_scheme(const char *url)
{
	const unsigned char *p;

	if (!url || !*url)
		return 0;

	for (p = (const unsigned char *)url; *p; p++) {
		if (*p == ':')
			return p != (const unsigned char *)url;
		if (*p == '/' || *p == '?' || *p == '#')
			return 0;
		if (!((*p >= 'A' && *p <= 'Z') ||
		      (*p >= 'a' && *p <= 'z') ||
		      (*p >= '0' && *p <= '9') ||
		      *p == '+' || *p == '-' || *p == '.'))
			return 0;
	}
	return 0;
}

static int markdown_url_is_local(const char *url)
{
	if (!url || !*url)
		return 0;
	if (url[0] == '#')
		return 0;
	if (strncmp(url, "//", 2) == 0)
		return 0;
	if (strncmp(url, "file://", 7) == 0)
		return 1;
	if (markdown_url_has_scheme(url))
		return 0;
	return 1;
}

static char *markdown_local_path_from_url(const char *url)
{
	const char *path;

	if (!url)
		return NULL;

	path = url;
	if (strncmp(path, "file://", 7) == 0)
		path += 7;

	if (path[0] == '~')
		return file_expand_path(path);
	return strdup(path);
}

static char *markdown_link_target_dup(const char *start, size_t len)
{
	const char *p = start;
	const char *end = start + len;
	const char *target_start;
	const char *target_end;
	char *target;
	size_t target_len;

	while (p < end && (*p == ' ' || *p == '\t' ||
			  *p == '\r' || *p == '\n'))
		p++;
	while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\r' || end[-1] == '\n'))
		end--;

	if (p >= end)
		return NULL;

	if (*p == '<') {
		p++;
		target_start = p;
		while (p < end && *p != '>')
			p++;
		target_end = p;
	} else {
		target_start = p;
		while (p < end && *p != ' ' && *p != '\t' &&
		       *p != '\r' && *p != '\n')
			p++;
		target_end = p;
	}

	if (target_end <= target_start)
		return NULL;

	target_len = (size_t)(target_end - target_start);
	target = malloc(target_len + 1);
	if (!target)
		return NULL;
	memcpy(target, target_start, target_len);
	target[target_len] = '\0';
	return target;
}

static enum guardrail_verdict
gr_final_local_file_missing(const struct guardrail_eval_ctx *ctx,
			    char *reason_out, size_t reason_cap)
{
	const char *scan;
	const char *answer;
	int match_len;

	answer = ctx->proposed_answer;
	if (!answer || !*answer)
		return GUARDRAIL_PASS;

	scan = answer;
	while (*scan) {
		int idx = re_match("\\]\\(", scan, &match_len);
		const char *url_start;
		const char *url_end;
		char *url;
		char *path;

		if (idx < 0)
			break;

		url_start = scan + idx + match_len;
		url_end = strchr(url_start, ')');
		if (!url_end)
			break;

		url = markdown_link_target_dup(url_start,
					       (size_t)(url_end - url_start));
		if (!url) {
			scan = url_end + 1;
			continue;
		}

		if (!markdown_url_is_local(url)) {
			free(url);
			scan = url_end + 1;
			continue;
		}

		path = markdown_local_path_from_url(url);
		if (path && !file_exists(path)) {
			snprintf(reason_out, reason_cap,
				 "Referenced local file does not exist: %s",
				 path);
			free(path);
			free(url);
			return GUARDRAIL_FAIL;
		}

		free(path);
		free(url);
		scan = url_end + 1;
	}

	return GUARDRAIL_PASS;
}

/* ── registration API ── */

int guardrail_rule_register(struct guardrail_config *cfg,
			     const char *name,
			     enum guardrail_hook hook,
			     enum guardrail_rule_type type,
			     guardrail_rule_fn check,
			     const char *description,
			     const char *ext_entry,
			     const char *action_text)
{
	if (!cfg || !name) return -EINVAL;
	if (cfg->rule_count >= GUARDRAIL_RULES_MAX) return -ENOSPC;
	if (guardrail_rule_lookup(cfg, name)) return -EEXIST;
	struct guardrail_rule *r = &cfg->rules[cfg->rule_count];
	memset(r, 0, sizeof(*r));
	strncpy(r->name, name, sizeof(r->name) - 1);
	r->hook = hook;
	r->type = type;
	r->enabled = 1;
	r->check = (type == GUARDRAIL_RULE_C) ? check : NULL;
	if (description)
		strncpy(r->description, description, sizeof(r->description) - 1);
	if (ext_entry)
		strncpy(r->ext_entry, ext_entry, sizeof(r->ext_entry) - 1);
	if (action_text)
		strncpy(r->action_text, action_text, sizeof(r->action_text) - 1);
	cfg->rule_count++;
	log_dbg(cfg ? cfg->logger : NULL, "guardrail: registered rule '%s' (hook=%d, type=%d)",
		name, hook, type);
	return 0;
}

int guardrail_rule_disable(struct guardrail_config *cfg, const char *name)
{
	struct guardrail_rule *r = guardrail_rule_lookup(cfg, name);
	if (!r) return -ENOENT;
	r->enabled = 0;
	return 0;
}

int guardrail_rule_enable(struct guardrail_config *cfg, const char *name)
{
	struct guardrail_rule *r = guardrail_rule_lookup(cfg, name);
	if (!r) return -ENOENT;
	r->enabled = 1;
	return 0;
}

struct guardrail_rule *guardrail_rule_lookup(struct guardrail_config *cfg,
					      const char *name)
{
	if (!cfg || !name) return NULL;
	for (int i = 0; i < cfg->rule_count; i++)
		if (strcmp(cfg->rules[i].name, name) == 0)
			return &cfg->rules[i];
	return NULL;
}

/* ── LLM-based rule backend ── */

static int llm_collect_cb(const char *token, void *user_data)
{
	return morph_buf_append_cb(token, user_data);
}

static enum guardrail_verdict
guardrail_llm_check(const struct guardrail_rule *rule,
		    const struct guardrail_config *cfg,
		    const struct guardrail_eval_ctx *ctx,
		    char *reason_out, size_t reason_cap)
{
	if (!cfg->llm) {
		log_warn(cfg ? cfg->logger : NULL,
			 "guardrail: LLM rule '%s' has no LLM model, skipping", rule->name);
		return GUARDRAIL_PASS;
	}

	const char *check_text = NULL;
	switch (rule->hook) {
	case GUARDRAIL_HOOK_INPUT:
		check_text = ctx->user_input; break;
	case GUARDRAIL_HOOK_TOOL_OUTPUT:
		check_text = ctx->tool_result; break;
	case GUARDRAIL_HOOK_OUTPUT:
		check_text = ctx->proposed_answer; break;
	}
	if (!check_text || !*check_text) return GUARDRAIL_PASS;

	char prompt[2048];
	snprintf(prompt, sizeof(prompt),
		"Check if the following text passes this rule: \"%s\"\n\n"
		"Text: \"%s\"\n\n"
		"Respond with ONLY a JSON object: "
		"{\"pass\": true/false, \"reason\": \"brief explanation\"}\n"
		"Do not include any other text.",
		rule->description, check_text);

	morph_buf_t collect;
	if (morph_buf_init(&collect, 4096) != 0)
		return GUARDRAIL_PASS;

	const char *msgs[1] = { prompt };
	int status = cfg->llm->chat(cfg->llm, ctx->arena, NULL,
				    msgs, 1, NULL,
				    llm_collect_cb, &collect);
	if (status < 0 || collect.len == 0) {
		log_warn(cfg ? cfg->logger : NULL,
			 "guardrail: LLM call failed for rule '%s', defaulting PASS", rule->name);
		morph_buf_cleanup(&collect);
		return GUARDRAIL_PASS;
	}

	char *result_str = morph_buf_detach(&collect);
	cJSON *root = cJSON_Parse(result_str);
	free(result_str);
	if (!root) {
		log_warn(cfg ? cfg->logger : NULL,
			 "guardrail: LLM response not JSON for rule '%s', defaulting PASS",
			 rule->name);
		return GUARDRAIL_PASS;
	}

	cJSON *pass_item = cJSON_GetObjectItem(root, "pass");
	int passed = cJSON_IsBool(pass_item) ? cJSON_IsTrue(pass_item) : 1;

	if (!passed) {
		cJSON *reason_item = cJSON_GetObjectItem(root, "reason");
		const char *reason = cJSON_IsString(reason_item)
			? reason_item->valuestring : rule->description;
		snprintf(reason_out, reason_cap, "%s", reason);
	}

	cJSON_Delete(root);
	return passed ? GUARDRAIL_PASS : GUARDRAIL_FAIL;
}

/* ── Ext-based rule backend ── */

static const char *gr_check_text(const struct guardrail_rule *rule,
				 const struct guardrail_eval_ctx *ctx)
{
	switch (rule->hook) {
	case GUARDRAIL_HOOK_INPUT:
		return ctx->user_input;
	case GUARDRAIL_HOOK_TOOL_OUTPUT:
		return ctx->tool_result;
	case GUARDRAIL_HOOK_OUTPUT:
		return ctx->proposed_answer;
	}
	return NULL;
}

static enum guardrail_verdict guardrail_parse_check_result(struct morph_log *logger,
							   const char *result_json,
							   const struct guardrail_rule *rule,
							   char *reason_out, size_t reason_cap)
{
	cJSON *root = cJSON_Parse(result_json);
	if (!root) {
		log_warn(logger,
			 "guardrail: ext rule '%s' returned invalid JSON, "
			 "check unavailable",
			 rule->name);
		return GUARDRAIL_ERROR;
	}
	cJSON *pass_item = cJSON_GetObjectItem(root, "pass");
	if (!cJSON_IsBool(pass_item)) {
		cJSON_Delete(root);
		return GUARDRAIL_ERROR;
	}
	int passed = cJSON_IsTrue(pass_item);
	if (!passed) {
		cJSON *reason_item = cJSON_GetObjectItem(root, "reason");
		const char *reason = cJSON_IsString(reason_item)
			? reason_item->valuestring : rule->description;
		snprintf(reason_out, reason_cap, "%s", reason);
	}
	cJSON_Delete(root);
	return passed ? GUARDRAIL_PASS : GUARDRAIL_FAIL;
}

int guardrail_ext_so_load(struct morph_log *logger, struct guardrail_rule *rule)
{
	if (!rule || !rule->ext_entry[0]) return -EINVAL;
	void *handle = dlopen(rule->ext_entry, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		log_err(logger, "guardrail: dlopen %s failed: %s", rule->ext_entry, dlerror());
		return MORPH_ERR_LOAD;
	}
	void *sym = dlsym(handle, "guardrail_check");
	guardrail_ext_check_fn fn = NULL;
	memcpy(&fn, &sym, sizeof(fn));
	if (!fn) {
		log_err(logger, "guardrail: no 'guardrail_check' symbol in %s", rule->ext_entry);
		dlclose(handle);
		return MORPH_ERR_LOAD;
	}
	rule->dl_handle = handle;
	rule->ext_check = fn;
	log_info(logger, "guardrail: loaded .so ext rule '%s' from %s", rule->name,
		 rule->ext_entry);
	return 0;
}

void guardrail_ext_so_unload(struct guardrail_rule *rule)
{
	if (!rule || !rule->dl_handle) return;
	dlclose(rule->dl_handle);
	rule->dl_handle = NULL;
	rule->ext_check = NULL;
}

static enum guardrail_verdict
guardrail_ext_so_check(const struct guardrail_rule *rule,
		       const struct guardrail_eval_ctx *ctx,
		       char *reason_out, size_t reason_cap)
{
	if (!rule->ext_check) {
		log_warn(ctx ? ctx->logger : NULL, "guardrail: .so rule '%s' has no check fn",
			 rule->name);
		return GUARDRAIL_ERROR;
	}
	const char *text = gr_check_text(rule, ctx);
	if (!text) text = "";
	char *result_json = NULL;
	int rc = rule->ext_check(text, rule->name, rule->description,
				 &result_json);
	if (rc != 0 || !result_json) {
		log_warn(ctx ? ctx->logger : NULL,
			 "guardrail: .so rule '%s' returned %d, "
			 "check unavailable",
			 rule->name, rc);
		free(result_json);
		return GUARDRAIL_ERROR;
	}
	enum guardrail_verdict v = guardrail_parse_check_result(
		ctx ? ctx->logger : NULL, result_json, rule, reason_out, reason_cap);
	free(result_json);
	return v;
}

/** \brief Adapt a borrowed checker cancellation token. */
static int guardrail_cancelled(void *opaque)
{
	const struct guardrail_eval_ctx *ctx = opaque;
	return ctx != NULL && morph_cancel_token_is_cancelled(ctx->cancel_token);
}

/** \brief Run an external checker with bounded I/O and a single deadline. */
static enum guardrail_verdict guardrail_ext_exec_check(const struct guardrail_rule *rule,
						       const struct guardrail_eval_ctx *ctx,
						       char *reason_out, size_t reason_cap)
{
	int input_pipe[2], output_pipe[2];
	int status = 0;
	pid_t pid;
	morph_buf_t request = {0}, output = {0}, errors = {0};
	struct jsonrpc_response response = {0};
	cJSON *params;
	char *params_json;
	char *json;
	int rc;
	posix_spawn_file_actions_t actions;
	extern char **environ;

	if (rule->ext_entry[0] == '\0') {
		return GUARDRAIL_ERROR;
	}
	params = cJSON_CreateObject();
	if (params == NULL) {
		return GUARDRAIL_ERROR;
	}
	const char *text = gr_check_text(rule, ctx);
	if (!cJSON_AddStringToObject(params, "text", text ? text : "") ||
	    !cJSON_AddStringToObject(params, "rule", rule->name) ||
	    !cJSON_AddStringToObject(params, "description", rule->description)) {
		cJSON_Delete(params);
		return GUARDRAIL_ERROR;
	}
	params_json = cJSON_PrintUnformatted(params);
	cJSON_Delete(params);
	if (params_json == NULL) {
		return GUARDRAIL_ERROR;
	}
	struct jsonrpc_request wire = {.id = 1, .method = "check", .params_json = params_json};
	json = jsonrpc_build_request(&wire);
	free(params_json);
	if (json == NULL) {
		return GUARDRAIL_ERROR;
	}
	rc = morph_buf_init(&request, strlen(json) + 2);
	if (rc == 0) {
		rc = morph_buf_puts(&request, json);
	}
	if (rc == 0) {
		rc = morph_buf_putc(&request, '\n');
	}
	free(json);
	if (rc == 0) {
		rc = morph_buf_init(&output, BUFSIZ);
	}
	if (rc == 0) {
		rc = morph_buf_init(&errors, BUFSIZ);
	}
	if (rc != 0) {
		goto out_free_buffers;
	}
	rc = morph_io_pipe(input_pipe);
	if (rc != 0) {
		goto out_free_buffers;
	}
	rc = morph_io_pipe(output_pipe);
	if (rc != 0) {
		close(input_pipe[0]);
		close(input_pipe[1]);
		goto out_free_buffers;
	}
	rc = posix_spawn_file_actions_init(&actions);
	if (rc == 0) {
		rc = posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
		if (rc == 0) {
			rc = posix_spawn_file_actions_adddup2(&actions, output_pipe[1],
							      STDOUT_FILENO);
		}
		if (rc == 0) {
			rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
							      O_WRONLY, 0);
		}
		for (int i = 0; i < 2 && rc == 0; i++) {
			rc = posix_spawn_file_actions_addclose(&actions, input_pipe[i]);
			if (rc == 0) {
				rc = posix_spawn_file_actions_addclose(&actions, output_pipe[i]);
			}
		}
		char *argv[] = {(char *)rule->ext_entry, NULL};
		posix_spawnattr_t attributes;
		int attribute_rc = posix_spawnattr_init(&attributes);
		if (attribute_rc == 0) {
			sigset_t empty;
			sigemptyset(&empty);
			attribute_rc = posix_spawnattr_setsigmask(&attributes, &empty);
			if (attribute_rc == 0) {
				attribute_rc = posix_spawnattr_setpgroup(&attributes, 0);
			}
			if (attribute_rc == 0) {
				short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
				flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
				attribute_rc = posix_spawnattr_setflags(&attributes, flags);
			}
			if (rc == 0 && attribute_rc == 0) {
				rc = posix_spawnp(&pid, rule->ext_entry, &actions, &attributes,
						  argv, environ);
			}
			posix_spawnattr_destroy(&attributes);
		}
		if (rc == 0) {
			rc = attribute_rc;
		}
		posix_spawn_file_actions_destroy(&actions);
	}
	close(input_pipe[0]);
	close(output_pipe[1]);
	if (rc != 0) {
		close(input_pipe[1]);
		close(output_pipe[0]);
		rc = -rc;
		goto out_free_buffers;
	}
	rc = morph_child_exchange(pid, input_pipe[1], output_pipe[0], -1, morph_buf_cstr(&request),
				  request.len, 30000, 1024 * 1024, guardrail_cancelled, (void *)ctx,
				  &output, &errors, &status);
	close(output_pipe[0]);
	if (rc == 0 && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
		rc = -ECHILD;
	}
	if (rc == 0) {
		rc = jsonrpc_parse_response(morph_buf_cstr(&output), &response);
	}
	if (rc == 0 && (response.has_error || response.result_json == NULL)) {
		rc = MORPH_ERR_PROTOCOL;
	}
	if (rc == 0) {
		enum guardrail_verdict verdict =
			guardrail_parse_check_result(ctx ? ctx->logger : NULL, response.result_json,
						     rule, reason_out, reason_cap);
		jsonrpc_response_free(&response);
		morph_buf_cleanup(&request);
		morph_buf_cleanup(&output);
		morph_buf_cleanup(&errors);
		return verdict;
	}
	jsonrpc_response_free(&response);
out_free_buffers:
	if (ctx != NULL && ctx->check_error != NULL) {
		*ctx->check_error = rc;
	}
	morph_buf_cleanup(&request);
	morph_buf_cleanup(&output);
	morph_buf_cleanup(&errors);
	return GUARDRAIL_ERROR;
}

static enum guardrail_verdict
guardrail_ext_check(const struct guardrail_rule *rule,
		    const struct guardrail_eval_ctx *ctx,
		    char *reason_out, size_t reason_cap)
{
	if (rule->ext_type == GUARDRAIL_EXT_SO)
		return guardrail_ext_so_check(rule, ctx, reason_out, reason_cap);
	return guardrail_ext_exec_check(rule, ctx, reason_out, reason_cap);
}

/* ── core engine ── */

struct guardrail_result guardrail_run_hook(const struct guardrail_config *cfg,
					   enum guardrail_hook hook,
					   const struct guardrail_eval_ctx *eval)
{
	struct guardrail_result r = {
		.verdict = GUARDRAIL_PASS,
		.triggered_rule = NULL
	};
	if (!cfg || !cfg->enabled) return r;
	if (eval == NULL) {
		r.check_error = -EINVAL;
		return r;
	}
	int check_error = 0;
	struct guardrail_eval_ctx evaluation = *eval;
	evaluation.check_error = &check_error;

	evaluation.logger = cfg->logger;
	eval = &evaluation;

	for (int i = 0; i < cfg->rule_count; i++) {
		const struct guardrail_rule *rule = &cfg->rules[i];
		if (!rule->enabled || rule->hook != hook) continue;

		enum guardrail_verdict v = GUARDRAIL_PASS;
		check_error = 0;
		switch (rule->type) {
		case GUARDRAIL_RULE_C:
			if (rule->check)
				v = rule->check(eval, r.reason, sizeof(r.reason));
			break;
		case GUARDRAIL_RULE_LLM:
			v = guardrail_llm_check(rule, cfg, eval,
						r.reason, sizeof(r.reason));
			break;
		case GUARDRAIL_RULE_EXT:
			v = guardrail_ext_check(rule, eval,
						r.reason, sizeof(r.reason));
			break;
		}

		if (v == GUARDRAIL_ERROR) {
			r.check_error = check_error < 0 ? check_error : MORPH_ERR_PROCESSING;
			log_warn(cfg->logger, "guardrail check unavailable: %s", rule->name);
			if (rule->block_on_error) {
				snprintf(r.reason, sizeof(r.reason),
					 "Guardrail check unavailable: %s", rule->name);
				v = GUARDRAIL_FAIL;
			}
		}
		if (v == GUARDRAIL_FAIL) {
			r.verdict = GUARDRAIL_FAIL;
			r.triggered_rule = rule;
			log_info(cfg ? cfg->logger : NULL, "guardrail[%s]: FAIL - %s", rule->name,
				 r.reason);
			return r;
		}
	}
	return r;
}

void guardrail_register_builtin_rules(struct guardrail_config *cfg)
{
	if (!cfg) return;

	guardrail_rule_register(cfg, "consecutive_empty",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C,
		gr_consecutive_empty, NULL, NULL,
		"Multiple empty responses. Provide a substantive answer.");

	guardrail_rule_register(cfg, "empty_answer",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C,
		gr_empty_answer, NULL, NULL,
		"Provide a substantive answer.");

	guardrail_rule_register(cfg, "tools_all_failed",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C,
		gr_tools_all_failed, NULL, NULL,
		"All your tool calls failed. Check the error messages "
		"above, fix your tool arguments, and try again.");

	guardrail_rule_register(cfg, "creative_no_media",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C,
		gr_creative_no_media, NULL, NULL,
		"Your creative tool calls did not produce output files. "
		"Verify the tool parameters are correct.");

	guardrail_rule_register(cfg, "creative_file_missing",
		GUARDRAIL_HOOK_TOOL_OUTPUT, GUARDRAIL_RULE_C,
		gr_creative_file_missing, NULL, NULL,
		"The creative tool reported an output file but it does "
		"not exist. The generation may have failed silently. "
		"Try calling the tool again.");

	guardrail_rule_register(cfg, "final_local_file_missing",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C,
		gr_final_local_file_missing, NULL, NULL,
		"Regenerate the file or reference a local file that exists.");
}

void guardrail_set_llm(struct guardrail_config *cfg, struct model *llm)
{
	if (cfg) cfg->llm = llm;
}
