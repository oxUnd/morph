#include "runtime/hooks_internal.h"

#include "agent/tokenizer.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"
#include "util/cleanup.h"
#include "util/str.h"
#include "util/json.h"

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
	char *directory MORPH_CLEANUP(morph_charp_cleanup) =
		file_expand_path_copy(manager->config->general.output_dir);
	if (!directory)
		return -ENOMEM;
	int rc = file_ensure_dir(directory);
	if (rc == 0)
		rc = file_path_join(path, PATH_MAX, directory, "hook-context-XXXXXX");
	if (rc != 0)
		return rc;
	int fd = mkstemp(path);
	if (fd < 0)
		return morph_error_from_errno(errno);
	if (close(fd) != 0) {
		rc = morph_error_from_errno(errno);
		(void)unlink(path);
		return rc;
	}
	rc = file_write_atomic(path, text, strlen(text));
	if (rc != 0)
		(void)unlink(path);
	return rc;
}

static int append_context(struct hook_manager *manager, morph_buf_t *context,
			  struct hook_entry *entry, const char *text)
{
	char path[PATH_MAX];
	char *preview MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	const char *shown = text;
	int rc = 0;
	int saved = 0;

	if (!text || !*text)
		return 0;
	if (utf8valid(text))
		return MORPH_ERR_PROTOCOL;
	if (entry->context_limit > 0 && tokenizer_estimate_tokens(text) > entry->context_limit) {
		saved = hook_context_file_create(manager, text, path) == 0;
		/* The shared estimator uses tokens; clamp only at UTF-8 boundaries. */
		size_t bytes = strlen(text);
		preview = utf8_dup_clamped(text, bytes);
		if (!preview)
			return -ENOMEM;
		while (tokenizer_estimate_tokens(preview) > entry->context_limit && bytes > 0) {
			bytes /= 2;
			preview[utf8_clamp_bytes(preview, bytes)] = '\0';
		}
		shown = preview;
	}
	rc = morph_buf_printf(context, "[Hook %s / %s]\n%s\n", entry->id, entry->event, shown);
	if (rc == 0 && preview && saved)
		rc = morph_buf_printf(context, "Full hook context: %s\n", path);
	else if (rc == 0 && preview)
		rc = morph_buf_puts(context, "[Full hook context could not be saved.]\n");
	return rc;
}

static morph_str_t hook_trim_borrow(const char *text)
{
	morph_str_t trimmed = {0, text};
	size_t length = strlen(text), offset = 0;
	int started = 0;

	while (offset < length) {
		unsigned cp;
		size_t bytes;

		if (!utf8_decode_codepoint(text + offset, length - offset, &cp, &bytes))
			break;
		int space = cp == ' ' || (cp >= '\t' && cp <= '\r') || cp == 0x85 ||
			utf8_is_unicode_space_cp(cp);

		if (!space) {
			if (!started) {
				trimmed.data = text + offset;
				started = 1;
			}
			trimmed.len = (size_t)(text + offset + bytes - trimmed.data);
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

static int hook_append_reason(morph_buf_t *reason, const char *text)
{
	morph_str_t trimmed = hook_trim_borrow(text);
	int rc;

	if (utf8valid(text) || !trimmed.len)
		return MORPH_ERR_PROTOCOL;
	rc = reason->len ? morph_buf_puts(reason, "\n\n") : 0;
	if (rc == 0)
		rc = morph_buf_append(reason, trimmed.data, trimmed.len);
	return rc;
}

static int hook_decode_pre_tool(const cJSON *root, morph_buf_t *reason, cJSON **updated,
				int *blocked)
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
		return MORPH_ERR_PROTOCOL;
	if (!use_specific) {
		decision = hook_json_string_borrow(root, "decision");
		if (strcmp(decision, "block") == 0) {
			rc = hook_append_reason(reason, hook_json_string_borrow(root, "reason"));
			if (rc == 0)
				*blocked = 1;
			return rc;
		}
		return hook_has_value(root, "decision") || hook_has_value(root, "reason")
			? MORPH_ERR_PROTOCOL : 0;
	}
	if (strcmp(decision, "deny") == 0) {
		if (hook_has_value(specific, "updatedInput"))
			return MORPH_ERR_PROTOCOL;
		rc = hook_append_reason(reason,
			hook_json_string_borrow(specific, "permissionDecisionReason"));
		if (rc == 0)
			*blocked = 1;
		return rc;
	}
	if (strcmp(decision, "allow") != 0 || !cJSON_IsObject(replacement))
		return MORPH_ERR_PROTOCOL;
	cJSON *copy MORPH_CLEANUP(morph_json_cleanup) = cJSON_Duplicate(replacement, 1);

	if (!copy)
		return -ENOMEM;
	cJSON_Delete(*updated);
	*updated = morph_json_steal(&copy);
	return 0;
}

int hook_decode_output(struct hook_manager *manager, struct hook_run *run, const char *event,
		       morph_buf_t *context, morph_buf_t *reason, cJSON **updated, int *blocked,
		       int *ask, int *terminate)
{
	const char *text = morph_buf_cstr_borrow(&run->out);
	char *trimmed_text MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = NULL;
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
		rc = hook_append_reason(reason, morph_buf_cstr_borrow(&run->err));
		if (rc == 0)
			*blocked = 1;
		return rc;
	}
	if (run->exit_code != 0)
		return MORPH_ERR_PROCESSING;
	if (strcmp(event, "SessionEnd") == 0)
		return 0;
	if (utf8valid(text))
		return MORPH_ERR_PROTOCOL;
	morph_str_t trimmed = hook_trim_borrow(text);

	if (!trimmed.len)
		return 0;
	trimmed_text = morph_str_cstr_copy(trimmed);
	if (!trimmed_text)
		return -ENOMEM;
	root = cJSON_ParseWithOpts(trimmed_text, NULL, 1);
	if (!root) {
		const char *start = trimmed_text;

		if (*start == '{' || *start == '[')
			return MORPH_ERR_PROTOCOL;
		if (strcmp(event, "SessionStart") == 0 || strcmp(event, "UserPromptSubmit") == 0)
			return append_context(manager, context, run->entry, trimmed_text);
		if (strcmp(event, "PreToolUse") == 0 || strcmp(event, "PostToolUse") == 0 ||
		    strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0 ||
		    strcmp(event, "SessionEnd") == 0)
			return 0;
		return MORPH_ERR_PROTOCOL;
	}
	if (!cJSON_IsObject(root) || !hook_json_unique_fields(root, 0) ||
	    !hook_output_fields_valid(root, event))
		return MORPH_ERR_PROTOCOL;
	const char *system_message = hook_json_string_borrow(root, "systemMessage");

	if (*system_message) {
		run->system_message = strdup(system_message);
		if (!run->system_message)
			return -ENOMEM;
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
		rc = MORPH_ERR_PROTOCOL;
	} else if (strcmp(decision, "block") == 0) {
		rc = hook_append_reason(reason, feedback);
		if (rc == 0)
			*blocked = 1;
	} else if (post_tool && hook_has_value(root, "reason")) {
		rc = MORPH_ERR_PROTOCOL;
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
