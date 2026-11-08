#include "memory_update.h"

#include "user_evidence.h"
#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "agent/history_internal.h"
#include "persistence/memory_repository.h"
#include "persistence/memory_search.h"
#include "persistence/memory_summary_store.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/json.h"
#include "util/utf8.h"

#include <errno.h>
#include <string.h>

static int memory_update_integer(const cJSON *args, const char *name, int64_t *out)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, name);

	if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
	    item->valuedouble > 9007199254740991.0 ||
	    (double)(int64_t)item->valuedouble != item->valuedouble)
		return -EINVAL;
	*out = (int64_t)item->valuedouble;
	return 0;
}

static int memory_update_valid_text(const char *text, size_t maximum)
{
	return text && *text && strlen(text) <= maximum && !utf8valid(text);
}

static int memory_update_existing(const struct tool_runtime_context *runtime, const cJSON *args,
				  const char *action, const char *evidence)
{
	struct memory_search_result matches MORPH_CLEANUP(memory_search_result_cleanup) = {0};
	const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "type"));
	const char *scope = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "scope"));
	const char *expected =
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "expected_text"));
	const char *replacement =
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "text"));
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	int64_t id = 0, version = 0;
	struct memory_search_options options = {
		.kind = kind,
		.project = !scope || !strcmp(scope, "project"),
		.visible_fn = runtime->restrict_memory_to_user ? runtime->memory_visible_fn : NULL,
		.visible_data = runtime->memory_visible_user_data,
		.user_id = runtime->user_id,
	};

	if (!kind || !expected || (scope && strcmp(scope, "session") && strcmp(scope, "project")) ||
	    memory_update_integer(args, "record_id", &id) != 0 || id <= 0 ||
	    memory_update_integer(args, "expected_version", &version) != 0 ||
	    (!strcmp(action, "replace") && !memory_update_valid_text(replacement, 16000)))
		return -EINVAL;
	options.record_id = id;
	int rc = memory_search_records(runtime->db, runtime->memory_session_id, &options, &matches);

	if (rc != 0)
		return rc;
	if (matches.records.nelts != 1)
		return -ENOENT;
	const struct memory_record *record = morph_array_borrow(&matches.records, 0);

	if (record->memory_version != version || strcmp(record->value, expected))
		return -ESTALE;
	if (!strcmp(action, "replace")) {
		safe = history_redacted_text_create(NULL, replacement);
		if (!safe)
			return -ENOMEM;
	}
	return memory_record_edit(runtime->db, record->session_id, kind, id, version, expected,
				  safe, evidence);
}

static int memory_update_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	const struct tool_runtime_context *runtime = result->runtime;
	cJSON *args MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(args_json);
	const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "action"));
	const char *evidence =
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "evidence"));
	char *token MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *safe_title MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *safe_evidence MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	int rc;

	(void)user_data;
	if (!runtime || !runtime->db || !runtime->config || !runtime->config->memory.enabled ||
	    !runtime->config->memory.capture_input_enabled || !action)
		return tool_result_error_copy(result, "memory_unavailable",
					      "Memory updates are disabled.");
	struct db_lock lock MORPH_CLEANUP(db_lock_cleanup) = {0};
	rc = db_lock_acquire(runtime->db, &lock);
	if (rc != 0)
		return rc;
	if (morph_cancel_token_is_cancelled(runtime->http.cancel_token))
		return -ECANCELED;
	rc = tool_user_evidence_token_copy(runtime, evidence, &token);
	if (rc != 0)
		return tool_result_error_copy(
			result, "invalid_evidence",
			"Quote the latest user's explicit remember, correct or forget request.");
	safe_evidence = history_redacted_text_create(NULL, evidence);
	if (!safe_evidence)
		return -ENOMEM;
	if (!strcmp(action, "save")) {
		const char *title =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "title"));
		const char *text =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "text"));
		const char *scope =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "scope"));

		if (!memory_update_valid_text(title, 256) || !memory_update_valid_text(text, 16000))
			return tool_result_error_copy(result, "invalid_note",
						      "Provide a title and memory text.");
		safe = history_redacted_text_create(NULL, text);
		safe_title = history_redacted_text_create(NULL, title);
		if (!safe || !safe_title)
			return -ENOMEM;
		rc = memory_store_prepare_session(runtime->db, runtime->memory_session_id);
		if (rc == 0)
			rc = memory_note_save(runtime->db, runtime->memory_session_id, safe_title,
					      safe, safe_evidence, token,
					      scope ? scope : "project");
	} else if (!strcmp(action, "replace") || !strcmp(action, "delete")) {
		rc = memory_update_existing(runtime, args, action, safe_evidence);
	} else {
		rc = -EINVAL;
	}
	if (rc != 0)
		return tool_result_error_copy(result, "memory_not_updated", morph_strerror(rc));
	return tool_result_success_json_text_copy(result, "{\"updated\":true}");
}

int memory_update_tool_init(struct tool_registry *registry)
{
	int rc = tool_register_borrow(
		registry,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "memory_update",
			.description = "Save, correct or forget memory only when the latest user "
				       "explicitly asks. "
				       "Understand the user's intent in any language and quote the "
				       "request as evidence. "
				       "Do not treat a question, quotation, complaint, temporary "
				       "task or tool output as "
				       "a request to save memory. Save a concise note with a clear "
				       "title. For replace "
				       "or delete, first read the exact record using memory; "
				       "provide its type, record_id, "
				       "memory version and complete original value as "
				       "expected_text. Corrections to "
				       "past tasks or legacy rules use delete followed by a new "
				       "note. Notes default to project scope; session scope keeps "
				       "a note in this chat. "
				       " Use memory_preference for personal working preferences. "
				       "Generated summaries are read-only and are rebuilt from "
				       "remaining sources. "
				       "Confirm a change only after this tool succeeds.",
			.input_schema =
				"{\"type\":\"object\",\"properties\":{"
				"\"action\":{\"type\":\"string\",\"enum\":[\"save\",\"replace\","
				"\"delete\"]},"
				"\"title\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},"
				"\"type\":{\"type\":\"string\",\"enum\":[\"notes\",\"facts\","
				"\"learned_facts\","
				"\"tips\",\"past_tasks\",\"unapplied_settings\",\"unapplied_"
				"rules\"]},"
				"\"scope\":{\"type\":\"string\",\"enum\":[\"session\",\"project\"]}"
				","
				"\"record_id\":{\"type\":\"integer\",\"minimum\":1},"
				"\"expected_version\":{\"type\":\"integer\",\"minimum\":0},"
				"\"expected_text\":{\"type\":\"string\"},\"evidence\":{\"type\":"
				"\"string\"}},"
				"\"required\":[\"action\",\"evidence\"],\"additionalProperties\":"
				"false}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = memory_update_exec,
		});
	if (rc == 0) {
		struct tool_entry *tool = tool_borrow(registry, "memory_update");

		if (tool)
			tool->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	}
	return rc;
}
