#include "runtime/hooks_internal.h"

#include "agent/tokenizer.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/utf8.h"
#include "nb/cleanup.h"
#include "nb/str.h"
#include "cJSON.h"

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int hook_context_file_create(struct hook_manager *manager, const char *text,
				    char path[PATH_MAX])
{
	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf temporary NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = nb_path_expand(&directory, manager->config->general.output_dir, NULL);
	if (rc == NB_OK)
		rc = nb_file_mkdir(nb_buf_cstr(&directory), true, NULL);
	if (rc == NB_OK)
		rc = nb_file_temp_in(&temporary, nb_buf_cstr(&directory), NULL, NULL);
	if (rc != NB_OK)
		return rc;
	rc = nb_str_copy_to(nb_buf_str(&temporary), path, PATH_MAX);
	if (rc == NB_OK)
		rc = nb_file_write_all_atomic(
			path, text, strlen(text),
			&(nb_file_write_options){.mode = 0600, .sync_dir = true}, NULL);
	if (rc != NB_OK)
		(void)nb_file_remove(nb_buf_cstr(&temporary), NULL);
	return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
}

static int append_context(struct hook_manager *manager, nb_buf *context, struct hook_entry *entry,
			  const char *text)
{
	char path[PATH_MAX];
	char *preview NB_CLEANUP(nb_charp_cleanup) = NULL;
	const char *shown = text;
	int rc = 0;
	int saved = 0;

	if (!text || !*text)
		return 0;
	if (nb_utf8_validate(nb_str_from_cstr(text), NULL))
		return NB_ERR_PROTOCOL;
	if (entry->context_limit > 0 && tokenizer_estimate_tokens(text) > entry->context_limit) {
		saved = hook_context_file_create(manager, text, path) == 0;
		/* The shared estimator uses tokens; clamp only at UTF-8 boundaries. */
		size_t bytes = strlen(text);
		preview = nb_utf8_ellipsize_bytes(text, bytes);
		if (!preview)
			return NB_ERR_NOMEM;
		while (tokenizer_estimate_tokens(preview) > entry->context_limit && bytes > 0) {
			bytes /= 2;
			preview[nb_utf8_clamp_bytes(preview, bytes)] = '\0';
		}
		shown = preview;
	}
	rc = (nb_buf_printf(context, "[Hook %s / %s]\n%s\n", entry->id, entry->event, shown)
		      ? 0
		      : NB_ERR_NOMEM);
	if (rc == 0 && preview && saved)
		rc = (nb_buf_printf(context, "Full hook context: %s\n", path) ? 0 : NB_ERR_NOMEM);
	else if (rc == 0 && preview)
		rc = (nb_buf_append_cstr(context, "[Full hook context could not be saved.]\n")
			      ? 0
			      : NB_ERR_NOMEM);
	return rc;
}

static nb_str hook_trim_borrow(const char *text)
{
	nb_str trimmed = nb_str_from_parts(text, 0);
	size_t length = strlen(text), offset = 0;
	int started = 0;

	while (offset < length) {
		unsigned cp;
		size_t bytes;

		if (!nb_utf8_read(text + offset, length - offset, &cp, &bytes))
			break;
		int space = cp == ' ' || (cp >= '\t' && cp <= '\r') || cp == 0x85 ||
			    nb_utf8_is_unicode_space_cp(cp);

		if (!space) {
			if (!started) {
				trimmed.ptr = text + offset;
				started = 1;
			}
			trimmed.len = (size_t)(text + offset + bytes - trimmed.ptr);
		}
		offset += bytes;
	}
	return trimmed;
}

static int hook_optional_string(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return !value || cJSON_IsNull(value) || cJSON_IsString(value);
}

static int hook_has_value(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);

	return value && !cJSON_IsNull(value);
}

static int hook_output_fields_valid(const cJSON *root, const char *event)
{
	const cJSON *field;
	int interrupt = strcmp(event, "Interrupt") == 0;
	int pre_tool = strcmp(event, "PreToolUse") == 0;
	int decisions = pre_tool || strcmp(event, "PostToolUse") == 0 ||
		strcmp(event, "Stop") == 0 || strcmp(event, "UserPromptSubmit") == 0;
	int specific_allowed = pre_tool || strcmp(event, "PostToolUse") == 0 ||
		strcmp(event, "SessionStart") == 0 || strcmp(event, "UserPromptSubmit") == 0;
	const cJSON *specific = cJSON_GetObjectItemCaseSensitive(root, "hookSpecificOutput");
	const cJSON *cont = cJSON_GetObjectItemCaseSensitive(root, "continue");
	const cJSON *suppress = cJSON_GetObjectItemCaseSensitive(root, "suppressOutput");
	const char *decision = hook_json_string_borrow(root, "decision");

	cJSON_ArrayForEach(field, root) {
		const char *key = field->string;

		if (strcmp(key, "systemMessage") == 0)
			continue;
		if (!interrupt && (strcmp(key, "continue") == 0 ||
		    strcmp(key, "stopReason") == 0 || strcmp(key, "suppressOutput") == 0))
			continue;
		if (decisions && (strcmp(key, "decision") == 0 || strcmp(key, "reason") == 0))
			continue;
		if (specific_allowed && strcmp(key, "hookSpecificOutput") == 0)
			continue;
		return 0;
	}
	if ((cont && !cJSON_IsBool(cont)) || (suppress && !cJSON_IsBool(suppress)) ||
	    !hook_optional_string(root, "systemMessage") ||
	    !hook_optional_string(root, "stopReason") ||
	    !hook_optional_string(root, "reason") || !hook_optional_string(root, "decision"))
		return 0;
	if (hook_has_value(root, "decision") && strcmp(decision, "block") != 0 &&
	    !(pre_tool && strcmp(decision, "approve") == 0))
		return 0;
	if (!specific || cJSON_IsNull(specific))
		return 1;
	if (!cJSON_IsObject(specific) ||
	    strcmp(hook_json_string_borrow(specific, "hookEventName"), event) != 0 ||
	    !hook_optional_string(specific, "additionalContext"))
		return 0;
	cJSON_ArrayForEach(field, specific) {
		const char *key = field->string;

		if (strcmp(key, "hookEventName") == 0 || strcmp(key, "additionalContext") == 0)
			continue;
		if (pre_tool && (strcmp(key, "permissionDecision") == 0 ||
		    strcmp(key, "permissionDecisionReason") == 0 ||
		    strcmp(key, "updatedInput") == 0))
			continue;
		if (strcmp(event, "PostToolUse") == 0 && strcmp(key, "updatedMCPToolOutput") == 0)
			continue;
		return 0;
	}
	decision = hook_json_string_borrow(specific, "permissionDecision");
	return hook_optional_string(specific, "permissionDecisionReason") &&
		hook_optional_string(specific, "permissionDecision") &&
		(!hook_has_value(specific, "permissionDecision") ||
		 strcmp(decision, "allow") == 0 || strcmp(decision, "deny") == 0 ||
		 strcmp(decision, "ask") == 0);
}

static int hook_append_reason(nb_buf *reason, const char *text)
{
	nb_str trimmed = hook_trim_borrow(text);
	int rc;

	if (nb_utf8_validate(nb_str_from_cstr(text), NULL) || !trimmed.len)
		return NB_ERR_PROTOCOL;
	rc = reason->len ? (nb_buf_append_cstr(reason, "\n\n") ? 0 : NB_ERR_NOMEM) : 0;
	if (rc == 0)
		rc = (nb_buf_append(reason, trimmed.ptr, trimmed.len) ? 0 : NB_ERR_NOMEM);
	return rc;
}

static int hook_decode_pre_tool(const cJSON *root, nb_buf *reason, cJSON **updated, int *blocked)
{
	const cJSON *specific = cJSON_GetObjectItemCaseSensitive(root, "hookSpecificOutput");
	const cJSON *cont = cJSON_GetObjectItemCaseSensitive(root, "continue");
	const cJSON *replacement = cJSON_GetObjectItemCaseSensitive(specific, "updatedInput");
	const char *decision = hook_json_string_borrow(specific, "permissionDecision");
	int use_specific = hook_has_value(specific, "permissionDecision") ||
		hook_has_value(specific, "permissionDecisionReason") ||
		hook_has_value(specific, "updatedInput");
	int rc = 0;

	if (cJSON_IsFalse(cont) || hook_has_value(root, "stopReason") ||
	    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "suppressOutput")))
		return NB_ERR_PROTOCOL;
	if (!use_specific) {
		decision = hook_json_string_borrow(root, "decision");
		if (strcmp(decision, "block") == 0) {
			rc = hook_append_reason(reason, hook_json_string_borrow(root, "reason"));
			if (rc == 0)
				*blocked = 1;
			return rc;
		}
		return hook_has_value(root, "decision") || hook_has_value(root, "reason")
			       ? NB_ERR_PROTOCOL
			       : 0;
	}
	if (strcmp(decision, "deny") == 0) {
		if (hook_has_value(specific, "updatedInput"))
			return NB_ERR_PROTOCOL;
		rc = hook_append_reason(reason,
			hook_json_string_borrow(specific, "permissionDecisionReason"));
		if (rc == 0)
			*blocked = 1;
		return rc;
	}
	if (strcmp(decision, "allow") != 0 || !cJSON_IsObject(replacement))
		return NB_ERR_PROTOCOL;
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(replacement, 1);

	if (!copy)
		return NB_ERR_NOMEM;
	cJSON_Delete(*updated);
	*updated = cJSON_StealOwner(&copy);
	return 0;
}

int hook_decode_output(struct hook_manager *manager, struct hook_run *run, const char *event,
		       nb_buf *context, nb_buf *reason, cJSON **updated, int *blocked, int *ask,
		       int *terminate)
{
	const char *text = nb_buf_cstr(&run->out);
	char *trimmed_text NB_CLEANUP(nb_charp_cleanup) = NULL;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	const cJSON *specific;
	int rc = 0;
	int controls = strcmp(event, "PreToolUse") == 0 || strcmp(event, "Stop") == 0 ||
		strcmp(event, "UserPromptSubmit") == 0 || strcmp(event, "PostToolUse") == 0;

	(void)ask;
	run->context_error = 0;
	run->context_valid = 0;
	free(run->system_message);
	run->system_message = NULL;

	if (run->rc < 0)
		return run->rc;
	if (run->exit_code == 2) {
		if (!controls)
			return MORPH_ERR_PROCESSING;
		rc = hook_append_reason(reason, nb_buf_cstr(&run->err));
		if (rc == 0)
			*blocked = 1;
		return rc;
	}
	if (run->exit_code != 0)
		return MORPH_ERR_PROCESSING;
	if (strcmp(event, "SessionEnd") == 0)
		return 0;
	if (nb_utf8_validate(nb_str_from_cstr(text), NULL))
		return NB_ERR_PROTOCOL;
	nb_str trimmed = hook_trim_borrow(text);

	if (!trimmed.len)
		return 0;
	trimmed_text = strndup((trimmed).ptr, (trimmed).len);
	if (!trimmed_text)
		return NB_ERR_NOMEM;
	root = cJSON_ParseWithOpts(trimmed_text, NULL, 1);
	if (!root) {
		const char *start = trimmed_text;

		if (*start == '{' || *start == '[')
			return NB_ERR_PROTOCOL;
		if (strcmp(event, "SessionStart") == 0 || strcmp(event, "UserPromptSubmit") == 0)
			return append_context(manager, context, run->entry, trimmed_text);
		if (strcmp(event, "PreToolUse") == 0 || strcmp(event, "PostToolUse") == 0 ||
		    strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0 ||
		    strcmp(event, "SessionEnd") == 0)
			return 0;
		return NB_ERR_PROTOCOL;
	}
	if (!cJSON_IsObject(root) || !hook_json_unique_fields(root, 0) ||
	    !hook_output_fields_valid(root, event))
		return NB_ERR_PROTOCOL;
	const char *system_message = hook_json_string_borrow(root, "systemMessage");

	if (*system_message) {
		run->system_message = strdup(system_message);
		if (!run->system_message)
			return NB_ERR_NOMEM;
	}
	specific = cJSON_GetObjectItemCaseSensitive(root, "hookSpecificOutput");
	const cJSON *cont = cJSON_GetObjectItemCaseSensitive(root, "continue");
	const cJSON *suppress = cJSON_GetObjectItemCaseSensitive(root, "suppressOutput");
	const char *decision = hook_json_string_borrow(root, "decision");
	const char *feedback = hook_json_string_borrow(root, "reason");
	int post_tool = strcmp(event, "PostToolUse") == 0;
	int startup_context = strcmp(event, "UserPromptSubmit") == 0 ||
		strcmp(event, "SessionStart") == 0;

	if (strcmp(event, "Interrupt") == 0)
		return 0;
	if (startup_context) {
		rc = append_context(manager, context, run->entry,
				    hook_json_string_borrow(specific, "additionalContext"));
		if (rc != 0) {
			run->context_error = rc;
			return rc;
		}
		run->context_valid = 1;
	}
	if (strcmp(event, "PreToolUse") == 0) {
		rc = hook_decode_pre_tool(root, reason, updated, blocked);
	} else if (cJSON_IsFalse(cont)) {
		if (post_tool) {
			if (!hook_trim_borrow(feedback).len)
				feedback = hook_json_string_borrow(root, "stopReason");
			if (!hook_trim_borrow(feedback).len)
				feedback = "PostToolUse hook stopped execution";
			rc = hook_append_reason(reason, feedback);
			if (rc == 0)
				*blocked = 1;
		} else {
			*terminate = 1;
			if (strcmp(event, "UserPromptSubmit") == 0)
				*blocked = 1;
			feedback = hook_json_string_borrow(root, "stopReason");
			if (hook_trim_borrow(feedback).len)
				rc = hook_append_reason(reason, feedback);
		}
	} else if (post_tool && (cJSON_IsTrue(suppress) ||
			       hook_has_value(specific, "updatedMCPToolOutput"))) {
		rc = NB_ERR_PROTOCOL;
	} else if (strcmp(decision, "block") == 0) {
		rc = hook_append_reason(reason, feedback);
		if (rc == 0)
			*blocked = 1;
	} else if (post_tool && hook_has_value(root, "reason")) {
		rc = NB_ERR_PROTOCOL;
	}
	if (rc == 0 && specific && !startup_context &&
	    !(post_tool && (cJSON_IsTrue(suppress) ||
			   hook_has_value(specific, "updatedMCPToolOutput")))) {
		rc = append_context(manager, context, run->entry,
				    hook_json_string_borrow(specific, "additionalContext"));
		if (rc != 0)
			run->context_error = rc;
	}
	return rc;
}
