#include "runtime/hooks_internal.h"

#include "agent/tokenizer.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int append_context(struct hook_manager *manager, morph_buf_t *context,
			  struct hook_entry *entry, const char *text)
{
	char path[PATH_MAX];
	char *directory = NULL;
	char *preview = NULL;
	const char *shown = text;
	int rc = 0;

	if (!text || !*text)
		return 0;
	if (utf8valid(text))
		return MORPH_ERR_PROTOCOL;
	if (entry->context_limit > 0 && tokenizer_estimate_tokens(text) > entry->context_limit) {
		directory = file_expand_path(manager->config->general.output_dir);
		if (!directory)
			return -ENOMEM;
		rc = file_ensure_dir(directory);
		if (rc == 0)
			rc = file_path_join(path, sizeof(path), directory, "hook-context-XXXXXX");
		free(directory);
		if (rc != 0)
			return rc;
		int fd = mkstemp(path);
		if (fd < 0)
			return morph_error_from_errno(errno);
		FILE *file = fdopen(fd, "w");
		if (!file) {
			int saved = -errno;
			close(fd);
			unlink(path);
			return saved;
		}
		if (fwrite(text, 1, strlen(text), file) != strlen(text))
			rc = -EIO;
		if (fclose(file) != 0)
			rc = -EIO;
		if (rc != 0) {
			unlink(path);
			return rc;
		}
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
	if (context->len + strlen(shown) > 32768u) {
		free(preview);
		return -E2BIG;
	}
	rc = morph_buf_printf(context, "[Hook %s / %s]\n%s\n", entry->id, entry->event, shown);
	if (rc == 0 && preview)
		rc = morph_buf_printf(context, "Full hook context: %s\n", path);
	free(preview);
	return rc;
}

int hook_decode_output(struct hook_manager *manager, struct hook_run *run, const char *event,
		       morph_buf_t *context, morph_buf_t *reason, cJSON **updated, int *blocked,
		       int *ask, int *terminate)
{
	const char *text = morph_buf_cstr(&run->out);
	const char *feedback = "";
	cJSON *root;
	const cJSON *specific;
	const cJSON *replacement;
	int deny = 0;
	int rc = 0;

	if (run->rc < 0)
		return run->rc;
	if (run->exit_code == 2) {
		feedback = morph_buf_cstr(&run->err);
		deny = strcmp(event, "PreToolUse") == 0 || strcmp(event, "Stop") == 0 ||
		       strcmp(event, "UserPromptSubmit") == 0 || strcmp(event, "PostToolUse") == 0;
		if (deny) {
			*blocked = 1;
			return morph_buf_printf(reason, "%s%s\n", feedback,
						*feedback ? "" : "Hook blocked this operation.");
		}
		return MORPH_ERR_PROCESSING;
	}
	if (run->exit_code != 0)
		return MORPH_ERR_PROCESSING;
	if (!*text)
		return 0;
	if (utf8valid(text))
		return MORPH_ERR_PROTOCOL;
	root = cJSON_ParseWithOpts(text, NULL, 1);
	if (!root) {
		if (strcmp(event, "SessionStart") == 0 || strcmp(event, "UserPromptSubmit") == 0)
			return append_context(manager, context, run->entry, text);
		if (strcmp(event, "PreToolUse") == 0 || strcmp(event, "PostToolUse") == 0 ||
		    strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0 ||
		    strcmp(event, "SessionEnd") == 0)
			return 0;
		return MORPH_ERR_PROTOCOL;
	}
	if (!cJSON_IsObject(root) || !hook_json_unique_fields(root, 0)) {
		cJSON_Delete(root);
		return MORPH_ERR_PROTOCOL;
	}
	specific = cJSON_GetObjectItemCaseSensitive(root, "hookSpecificOutput");
	if (specific && (!cJSON_IsObject(specific) ||
			 strcmp(hook_json_string(specific, "hookEventName"), event) != 0)) {
		cJSON_Delete(root);
		return MORPH_ERR_PROTOCOL;
	}
	const cJSON *cont = cJSON_GetObjectItemCaseSensitive(root, "continue");
	const cJSON *stop = cJSON_GetObjectItemCaseSensitive(root, "stopReason");
	const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "systemMessage");
	const cJSON *suppress = cJSON_GetObjectItemCaseSensitive(root, "suppressOutput");
	const cJSON *extra = cJSON_GetObjectItemCaseSensitive(specific, "additionalContext");
	if ((cont && !cJSON_IsBool(cont)) || (stop && !cJSON_IsString(stop)) ||
	    (message && !cJSON_IsString(message)) || (suppress && !cJSON_IsBool(suppress)) ||
	    (extra && !cJSON_IsString(extra))) {
		cJSON_Delete(root);
		return MORPH_ERR_PROTOCOL;
	}
	if (strcmp(event, "SessionEnd") == 0 || strcmp(event, "Interrupt") == 0) {
		cJSON_Delete(root);
		return 0;
	}
	if (strcmp(event, "PreToolUse") == 0) {
		const char *decision = hook_json_string(specific, "permissionDecision");
		if (*decision && strcmp(decision, "deny") != 0 && strcmp(decision, "allow") != 0 &&
		    strcmp(decision, "ask") != 0)
			rc = MORPH_ERR_PROTOCOL;
		if (strcmp(decision, "ask") == 0)
			*ask = 1;
		deny = strcmp(decision, "deny") == 0 ||
		       strcmp(hook_json_string(root, "decision"), "block") == 0;
		feedback = deny ? hook_json_string(specific, "permissionDecisionReason") : "";
		if (!*feedback)
			feedback = hook_json_string(root, "reason");
		replacement = cJSON_GetObjectItemCaseSensitive(specific, "updatedInput");
		if (replacement) {
			if (strcmp(decision, "allow") != 0 || !cJSON_IsObject(replacement)) {
				rc = MORPH_ERR_PROTOCOL;
			} else if (*updated && !cJSON_Compare(*updated, replacement, 1)) {
				/* Never execute a nondeterministically selected rewrite. */
				deny = 1;
				feedback = "Conflicting hook input rewrites.";
			} else if (!*updated) {
				*updated = cJSON_Duplicate(replacement, 1);
				if (!*updated)
					rc = -ENOMEM;
			}
		}
		if (cont || stop || suppress)
			rc = MORPH_ERR_PROTOCOL;
	} else if (strcmp(event, "Stop") == 0 || strcmp(event, "UserPromptSubmit") == 0 ||
		   strcmp(event, "PostToolUse") == 0 || strcmp(event, "SessionStart") == 0 ||
		   strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0) {
		deny = strcmp(hook_json_string(root, "decision"), "block") == 0 ||
		       cJSON_IsFalse(cont);
		if (strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0 ||
		    strcmp(event, "SessionStart") == 0)
			deny = cJSON_IsFalse(cont);
		if (cJSON_IsFalse(cont) && strcmp(event, "PostToolUse") != 0)
			*terminate = 1;
		if (strcmp(event, "PostToolUse") == 0 &&
		    (suppress || cJSON_HasObjectItem(specific, "updatedMCPToolOutput")))
			rc = MORPH_ERR_PROTOCOL;
		feedback = hook_json_string(root, "reason");
		if (!*feedback)
			feedback = hook_json_string(root, "stopReason");
	}
	if (rc == 0 && strcmp(event, "PreCompact") != 0 && strcmp(event, "PostCompact") != 0)
		rc = append_context(manager, context, run->entry,
				    hook_json_string(specific, "additionalContext"));
	if (rc == 0 && deny) {
		*blocked = 1;
		rc = morph_buf_printf(reason, "%s\n",
				      *feedback ? feedback : "Hook blocked this operation.");
	}
	cJSON_Delete(root);
	return rc;
}
