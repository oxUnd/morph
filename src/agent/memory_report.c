#include "memory.h"

#include "persistence/memory_search.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "cJSON.h"
#include "cJSON.h"

#include <errno.h>

static int memory_report_add_record(cJSON *entries, const struct memory_record *record)
{
	cJSON *entry NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

	if (!entry)
		return NB_ERR_NOMEM;
	if (!cJSON_AddNumberToObject(entry, "id", (double)record->id) ||
	    !cJSON_AddStringToObject(entry, "kind", record->kind) ||
	    !cJSON_AddStringToObject(entry, "key", record->key) ||
	    !cJSON_AddStringToObject(entry, "value", record->value) ||
	    !cJSON_AddStringToObject(entry, "source", record->source) ||
	    !cJSON_AddStringToObject(entry, "details", record->details) ||
	    !cJSON_AddStringToObject(entry, "session", record->session) ||
	    !cJSON_AddNumberToObject(entry, "updated_at", (double)record->updated_at) ||
	    !cJSON_AddNumberToObject(entry, "score", record->score) ||
	    !cJSON_AddNumberToObject(entry, "memory_version", (double)record->memory_version))
		return NB_ERR_NOMEM;
	return (cJSON_AddItemToArray(entries, entry) ? (entry = NULL, NB_OK) : NB_ERR_NOMEM);
}

int memory_report_create(struct db *db, int64_t session_id, const struct memory_options *options,
			 const char *query, cJSON **out)
{
	struct memory_search_result matches NB_CLEANUP(memory_search_result_cleanup) = {0};
	struct memory_search_options search = {.query = query, .project = 1};
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	char *preferences NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *history NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *jobs NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct memory_record *record;
	int rc;

	if (!db || !options || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	search.visible_fn = options->visible_fn;
	search.user_id = options->visible_user_id;
	search.visible_data = options->visible_user_data;
	rc = memory_search_records(db, session_id, &search, &matches);
	if (rc != 0)
		return rc;
	preferences = preference_render_copy(db, session_id, 0);
	history = preference_render_copy(db, session_id, 1);
	jobs = memory_background_text_create(db, session_id);
	if (!preferences || !history || !jobs)
		return MORPH_ERR_DB;
	report = cJSON_CreateObject();
	if (!report)
		return NB_ERR_NOMEM;
	cJSON *entries = cJSON_AddArrayToObject(report, "entries");
	if (!entries || !cJSON_AddBoolToObject(report, "enabled", options->enabled) ||
	    !cJSON_AddBoolToObject(report, "learning_enabled", options->learn_from_chats_enabled) ||
	    !cJSON_AddBoolToObject(report, "save_tasks_enabled", options->save_tasks_enabled) ||
	    !cJSON_AddNumberToObject(report, "reply_char_limit", options->max_reply_chars) ||
	    !cJSON_AddStringToObject(report, "preferences", preferences) ||
	    !cJSON_AddStringToObject(report, "history", history) ||
	    !cJSON_AddStringToObject(report, "jobs", jobs))
		return NB_ERR_NOMEM;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&matches.records) &&
			     (record = (struct memory_record *)nb_vec_at(&matches.records, _i_));
	     _i_++) {
		rc = memory_report_add_record(entries, record);
		if (rc != 0)
			return rc;
	}
	*out = cJSON_StealOwner(&report);
	return 0;
}
