#include "session_sync_audit.h"
#include "records.h"
#include <nb/file.h>
#include <nb/map.h>
#include <inttypes.h>
#include <stdio.h>

struct audit_session_row {
	int64_t id;
	bool id_missing;
};
static const struct norm_field audit_session_fields[] = {
	NORM_NULLABLE_FIELD(struct audit_session_row, id, id_missing, NORM_TYPE_I64, 0),
};
static const struct norm_model audit_session_model =
	NORM_MODEL(struct audit_session_row, "sessions", audit_session_fields);

struct audit_message_row {
	int64_t id;
	bool id_missing;
	int64_t session_id;
	bool session_id_missing;
};
static const struct norm_field audit_message_fields[] = {
	NORM_NULLABLE_FIELD(struct audit_message_row, id, id_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct audit_message_row, session_id, session_id_missing, NORM_TYPE_I64,
			    0),
};
static const struct norm_model audit_message_model =
	NORM_MODEL(struct audit_message_row, "messages", audit_message_fields);

struct audit_ui_row {
	int64_t session_id;
	bool session_id_missing;
	int64_t core_message_id;
	bool core_message_id_missing;
	nb_str type;
	bool type_missing;
};
static const struct norm_field audit_ui_fields[] = {
	NORM_NULLABLE_FIELD(struct audit_ui_row, session_id, session_id_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct audit_ui_row, core_message_id, core_message_id_missing,
			    NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct audit_ui_row, type, type_missing, NORM_TYPE_TEXT, 0),
};
static const struct norm_model audit_ui_model =
	NORM_MODEL(struct audit_ui_row, "ui_messages", audit_ui_fields);

struct audit_scratch {
	nb_buf directory;
};

struct audit_session {
	int64_t id;
	bool id_missing;
	bool in_core;
	bool core_messages;
	bool ui_rows;
	bool readable;
};

static void audit_scratch_cleanup(struct audit_scratch *scratch)
{
	if (scratch->directory.len)
		(void)nb_file_remove_all(nb_buf_cstr(&scratch->directory), 0, NULL);

	nb_buf_destroy(&scratch->directory);
}

static int audit_validate_path(const char *path, nb_buf *canonical)
{
	nb_buf sidecar NB_CLEANUP(nb_buf_cleanup) = {0};
	struct nb_file_info info = {0};
	const char *const suffixes[] = {"-wal", "-journal"};
	int rc = nb_path_canonical(canonical, path, NULL);

	if (rc == 0)
		rc = nb_file_stat(nb_buf_cstr(canonical), true, &info, NULL);

	if (rc != 0)
		return rc;

	if (!info.is_regular)
		return NB_ERR_INVAL;

	for (size_t i = 0; i < NB_ARRAY_SIZE(suffixes); i++) {
		nb_buf_clear(&sidecar);
		if (!nb_buf_printf(&sidecar, "%s%s", nb_buf_cstr(canonical), suffixes[i]))
			return NB_ERR_NOMEM;

		if (nb_file_exists(nb_buf_cstr(&sidecar)))
			return NB_ERR_BUSY;
	}

	return 0;
}

static int audit_copy_inputs(struct audit_scratch *scratch, const char *core, const char *ui,
			     nb_buf *core_copy, nb_buf *ui_copy)
{
	nb_buf core_path NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf ui_path NB_CLEANUP(nb_buf_cleanup) = {0};
	const struct nb_file_copy_options options = {
		.nofollow_source = true,
		.new_file_mode = 0600,
	};
	int rc = audit_validate_path(core, &core_path);

	if (rc == 0)
		rc = audit_validate_path(ui, &ui_path);

	if (rc == 0)
		rc = nb_file_temp_dir(&scratch->directory, NULL, "morph-session-audit-", NULL);

	if (rc == 0)
		rc = nb_path_join(core_copy, nb_buf_cstr(&scratch->directory), "core.db");

	if (rc == 0)
		rc = nb_path_join(ui_copy, nb_buf_cstr(&scratch->directory), "ui.db");

	if (rc == 0)
		rc = nb_file_copy(nb_buf_cstr(&core_path), nb_buf_cstr(core_copy), &options, NULL);

	if (rc == 0)
		rc = nb_file_copy(nb_buf_cstr(&ui_path), nb_buf_cstr(ui_copy), &options, NULL);

	if (rc == 0)
		rc = audit_validate_path(core, &core_path);

	if (rc == 0)
		rc = audit_validate_path(ui, &ui_path);

	return rc;
}

/* A standalone backup may retain a WAL-mode header. Bootstrap only the private
 * copy, then reopen it read-only; no input path reaches the database driver. */
static int audit_prepare_copy(const char *path)
{
	struct db database NB_CLEANUP(db_close) = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = db_open_plain(NULL, &database, path);

	if (rc == 0)
		rc = orm_scope_begin(&database, &scope);

	if (rc == 0)
		rc = orm_status(&database,
				norm_sqlite_set_journal_mode(
					scope.connection, NORM_SQLITE_JOURNAL_DELETE, NULL, &error),
				&error);

	return rc;
}

static int audit_check(struct orm_scope *scope)
{
	struct norm_error error = {0};
	size_t issues = 0;
	int rc = orm_status(scope->database,
			    norm_sqlite_check(scope->connection, NORM_SQLITE_CHECK_QUICK, NULL,
					      NULL, NULL, &issues, &error),
			    &error);

	return rc ? rc : issues ? NB_ERR_FORMAT : 0;
}

static nb_str audit_id_key(const int64_t *id, bool missing)
{
	return missing ? NB_STR_LIT("null") : nb_str_from_parts((const char *)id, sizeof(*id));
}

static void audit_session_cleanup(struct audit_session **owner)
{
	free(*owner);
	*owner = NULL;
}

static void audit_session_destroy(void *value, void *context)
{
	(void)context;
	free(value);
}

static int audit_session_get(nb_map *sessions, nb_vec *ordered, int64_t id, bool missing,
			     struct audit_session **out)
{
	nb_str key = audit_id_key(&id, missing);
	struct audit_session *value = nb_map_get(sessions, key);

	if (value) {
		*out = value;
		return 0;
	}

	struct audit_session *owner NB_CLEANUP(audit_session_cleanup) = calloc(1, sizeof(*owner));

	if (!owner)
		return NB_ERR_NOMEM;

	owner->id = id;
	owner->id_missing = missing;
	if (!nb_map_put(sessions, key, owner, NULL))
		return NB_ERR_NOMEM;

	value = owner;
	owner = NULL;
	if (!nb_vec_push_copy(ordered, &value))
		return NB_ERR_NOMEM;

	*out = value;
	return 0;
}

static int audit_session_compare(const void *left, const void *right)
{
	const struct audit_session *a = *(const struct audit_session *const *)left;
	const struct audit_session *b = *(const struct audit_session *const *)right;

	if (a->id_missing != b->id_missing)
		return a->id_missing ? -1 : 1;

	return (a->id > b->id) - (a->id < b->id);
}

static int audit_add_id(cJSON *array, const struct audit_session *session)
{
	char text[32];
	int length = snprintf(text, sizeof(text), "%" PRId64, session->id);
	cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (length < 0 || (size_t)length >= sizeof(text))
		return NB_ERR_RANGE;

	item = session->id_missing ? cJSON_CreateNull() : cJSON_CreateRaw(text);
	if (!item || !cJSON_AddItemToArray(array, item))
		return NB_ERR_NOMEM;

	item = NULL;
	return 0;
}

static int audit_add_count(cJSON *report, const char *name, size_t count)
{
	char text[32];
	int length = snprintf(text, sizeof(text), "%zu", count);
	cJSON *item NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (length < 0 || (size_t)length >= sizeof(text))
		return NB_ERR_RANGE;

	item = cJSON_CreateRaw(text);
	if (!item || !cJSON_AddItemToObject(report, name, item))
		return NB_ERR_NOMEM;

	item = NULL;
	return 0;
}

static int audit_build_report(nb_vec *ordered, const char *platform, size_t session_count,
			      size_t unsupported, size_t invalid_links, cJSON **out)
{
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	const char *const names[] = {
		"sessions_with_core_messages_but_no_ui_rows",
		"sessions_with_ui_rows_but_no_readable_rows",
		"ui_sessions_missing_from_core",
		"core_message_sessions_missing_from_core",
	};
	cJSON *arrays[NB_ARRAY_SIZE(names)] = {0};
	bool findings = unsupported || invalid_links;

	if (!report || !cJSON_AddStringToObject(report, "target_platform", platform))
		return NB_ERR_NOMEM;

	int rc = audit_add_count(report, "session_count", session_count);

	for (size_t i = 0; rc == 0 && i < NB_ARRAY_SIZE(names); i++) {
		cJSON *array NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();

		if (!array || !cJSON_AddItemToObject(report, names[i], array))
			return NB_ERR_NOMEM;

		arrays[i] = array;
		array = NULL;
	}

	if (ordered->len > 1)
		qsort(ordered->data, ordered->len, sizeof(struct audit_session *),
		      audit_session_compare);

	for (size_t i = 0; rc == 0 && i < ordered->len; i++) {
		const struct audit_session *session =
			*(struct audit_session **)nb_vec_at(ordered, i);
		const bool include[] = {
			session->in_core && session->core_messages && !session->ui_rows,
			session->in_core && session->ui_rows && !session->readable,
			!session->in_core && session->ui_rows,
			!session->in_core && session->core_messages,
		};

		for (size_t index = 0; rc == 0 && index < NB_ARRAY_SIZE(include); index++) {
			if (include[index]) {
				findings = true;
				rc = audit_add_id(arrays[index], session);
			}
		}
	}

	if (rc == 0)
		rc = audit_add_count(report, "unsupported_ui_message_count", unsupported);

	if (rc == 0)
		rc = audit_add_count(report, "invalid_core_message_link_count", invalid_links);

	if (rc == 0 && (!cJSON_AddBoolToObject(report, "identity_verified", false) ||
			!cJSON_AddStringToObject(
				report, "limitations",
				"Local integer IDs cannot prove matching database origins. "
				"Assets, model history and runtime caches are not checked.") ||
			!cJSON_AddBoolToObject(report, "has_findings", findings)))
		rc = NB_ERR_NOMEM;

	if (rc == 0) {
		*out = report;
		report = NULL;
	}

	return rc;
}

static int audit_collect(struct orm_scope *core, struct orm_scope *ui, const char *platform,
			 cJSON **out)
{
	struct norm_rows *sessions NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_rows *messages NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_rows *ui_rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	nb_map *session_map NB_CLEANUP(nb_map_cleanup) = nb_map_new();
	nb_map *message_map NB_CLEANUP(nb_map_cleanup) = nb_map_new();
	nb_vec ordered NB_CLEANUP(nb_vec_cleanup) = {0};
	const char *const kinds[] = {
		"USER",	   "THOUGHT", "ACTION",	     "OBSERVATION",  "FINAL",	      "ERROR",
		"IMAGE",   "VIDEO",   "PLAN",	     "ASK_USER",     "HITL_APPROVAL", "user",
		"thought", "action",  "observation", "final",	     "error",	      "image",
		"video",   "plan",    "askUser",     "hitlApproval",
	};
	struct norm_error error = {0};
	const struct norm_select_query query = {.max_rows = SIZE_MAX, .max_bytes = SIZE_MAX};
	size_t session_count = 0;
	size_t unsupported = 0;
	size_t invalid_links = 0;

	if (!session_map || !message_map)
		return NB_ERR_NOMEM;

	nb_vec_init(&ordered, sizeof(struct audit_session *));
	nb_map_set_value_dtor(session_map, audit_session_destroy, NULL);
	int rc = orm_status(core->database,
			    norm_select(core->connection, &audit_session_model, &query, NULL,
					&sessions, &error),
			    &error);

	if (rc == 0)
		rc = orm_status(core->database,
				norm_select(core->connection, &audit_message_model, &query, NULL,
					    &messages, &error),
				&error);

	if (rc == 0)
		rc = orm_status(ui->database,
				norm_select(ui->connection, &audit_ui_model, &query, NULL, &ui_rows,
					    &error),
				&error);

	for (size_t i = 0; rc == 0 && i < norm_rows_count(sessions); i++) {
		const struct audit_session_row *row = norm_rows_at(sessions, i);
		struct audit_session *session = NULL;

		rc = audit_session_get(session_map, &ordered, row->id, row->id_missing, &session);
		if (rc == 0 && !session->in_core) {
			session->in_core = true;
			session_count++;
		}
	}

	for (size_t i = 0; rc == 0 && i < norm_rows_count(messages); i++) {
		const struct audit_message_row *row = norm_rows_at(messages, i);
		struct audit_session *session = NULL;

		rc = audit_session_get(session_map, &ordered, row->session_id,
				       row->session_id_missing, &session);
		if (rc == 0) {
			session->core_messages = true;
			if (!row->id_missing &&
			    !nb_map_put(message_map, audit_id_key(&row->id, false), (void *)row,
					NULL))
				rc = NB_ERR_NOMEM;
		}
	}

	for (size_t i = 0; rc == 0 && i < norm_rows_count(ui_rows); i++) {
		const struct audit_ui_row *row = norm_rows_at(ui_rows, i);
		struct audit_session *session = NULL;

		rc = audit_session_get(session_map, &ordered, row->session_id,
				       row->session_id_missing, &session);
		if (rc != 0)
			break;

		session->ui_rows = true;
		bool readable = false;

		for (size_t kind = 0; !row->type_missing && kind < NB_ARRAY_SIZE(kinds); kind++) {
			if (nb_str_eq_cstr(row->type, kinds[kind])) {
				readable = true;
				break;
			}
		}

		session->readable |= readable;
		unsupported += !readable;
		if (!row->core_message_id_missing) {
			const struct audit_message_row *message =
				nb_map_get(message_map, audit_id_key(&row->core_message_id, false));
			bool missing = !message || message->session_id_missing;

			if (missing != row->session_id_missing ||
			    (!missing && message->session_id != row->session_id))
				invalid_links++;
		}
	}

	return rc ? rc
		  : audit_build_report(&ordered, platform, session_count, unsupported,
				       invalid_links, out);
}

int session_sync_audit_copy(const char *core_path, const char *ui_path, const char *platform,
			    cJSON **out)
{
	struct audit_scratch scratch NB_CLEANUP(audit_scratch_cleanup) = {0};
	nb_buf core_copy NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf ui_copy NB_CLEANUP(nb_buf_cleanup) = {0};
	struct db core NB_CLEANUP(db_close) = {0};
	struct db ui NB_CLEANUP(db_close) = {0};
	struct orm_scope core_scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct orm_scope ui_scope NB_CLEANUP(orm_scope_cleanup) = {0};

	if (!core_path || !ui_path || !platform || !out ||
	    (strcmp(platform, "android") && strcmp(platform, "ios")))
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	int rc = audit_copy_inputs(&scratch, core_path, ui_path, &core_copy, &ui_copy);

	if (rc == 0)
		rc = audit_prepare_copy(nb_buf_cstr(&core_copy));

	if (rc == 0)
		rc = audit_prepare_copy(nb_buf_cstr(&ui_copy));

	if (rc == 0)
		rc = db_open_readonly(NULL, &core, nb_buf_cstr(&core_copy));

	if (rc == 0)
		rc = db_open_readonly(NULL, &ui, nb_buf_cstr(&ui_copy));

	if (rc == 0)
		rc = orm_scope_begin(&core, &core_scope);

	if (rc == 0)
		rc = orm_scope_begin(&ui, &ui_scope);

	if (rc == 0)
		rc = audit_check(&core_scope);

	if (rc == 0)
		rc = audit_check(&ui_scope);

	return rc ? rc : audit_collect(&core_scope, &ui_scope, platform, out);
}
