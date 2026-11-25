#include <norm/query.h>
#include "persistence/orm/fcgi_store.h"
#include "persistence/orm/fcgi_sessions.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "persistence/orm/fcgi_models.h"
#include "persistence/orm/records.h"
#include "persistence/orm/session.h"
#include "persistence/orm/session_models.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

struct fcgi_session_view {
	nb_str display_id;
	bool missing_display_id;
	nb_str name;
	nb_str model;
	bool missing_model;
	int64_t created_at;
	int64_t updated_at;
};
static const struct norm_field session_fields[] = {
	NORM_NULLABLE_FIELD(struct fcgi_session_view, display_id, missing_display_id,
			    NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct fcgi_session_view, name, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct fcgi_session_view, model, missing_model, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct fcgi_session_view, created_at, NORM_TYPE_I64, 0),
	NORM_FIELD(struct fcgi_session_view, updated_at, NORM_TYPE_I64, 0),
};
static const struct norm_model fcgi_session_view_model = {
	.size = sizeof(struct fcgi_session_view),
	.alignment = _Alignof(struct fcgi_session_view),
	.fields = session_fields,
	.field_count = sizeof(session_fields) / sizeof(session_fields[0]),
};

int store_create_session(struct session_store *store, const char *user_id, const char *name,
			 const char *model, char out_session_id[64])
{
	struct session session = {0};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};

	if (!store || !user_id || !out_session_id)
		return NB_ERR_INVAL;

	int rc = session_create(&store->db, name, model, &session);

	if (rc == 0)
		rc = session_ensure_display_id(&store->db, &session);

	if (rc == 0)
		rc = nb_str_copy_to(nb_str_from_cstr(session.display_id), out_session_id, 64);

	if (rc == 0)
		rc = orm_scope_begin(&store->db, &scope);

	struct norm_error error = {0};
	const struct orm_fcgi_session_owner_row owner = {
		.session_id = nb_str_from_cstr(session.display_id),
		.user_id = nb_str_from_cstr(user_id),
	};
	const struct norm_term user = norm_term_excluded(norm_col(&orm_fcgi_session_owner, 1));
	const struct norm_assignment set[] = {
		{.column = norm_col(&orm_fcgi_session_owner, 1), .expression = &user},
	};
	const size_t keys[] = {0};
	const struct norm_conflict conflict = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = keys,
		.field_count = 1,
		.set = set,
		.set_count = 1,
	};
	const struct norm_insert_options options = {.conflict = &conflict};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_insert(scope.connection, &orm_fcgi_session_owner, &owner,
					    &options, NULL, NULL, &error),
				&error);

	return rc;
}

static int owned_session_id(struct session_store *store, const char *session_id,
			    const char *user_id, int64_t *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int rc = store ? orm_scope_begin(&store->db, &scope) : NB_ERR_INVAL;
	struct norm_error error = {0};
	const struct norm_table_ref sessions = norm_table(&session_model, NB_STR_LIT("s"));
	const struct norm_table_ref owners = norm_table(&orm_fcgi_session_owner, NB_STR_LIT("o"));
	const struct norm_join joins[] = {
		{.table = &owners,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(norm_table_col(&sessions, 1), norm_table_col(&owners, 0))},
	};
	const struct norm_expr predicates[] = {
		norm_eq(norm_table_col(&sessions, 1), orm_string(session_id)),
		norm_eq(norm_table_col(&owners, 1), orm_string(user_id)),
	};
	const struct norm_join_column columns[] = {
		{.column = norm_table_col(&sessions, 0), .into = 0},
	};
	const struct norm_join_query query = {
		.from = &sessions,
		.joins = joins,
		.join_count = 1,
		.projection = &orm_integer_model,
		.columns = columns,
		.column_count = 1,
		.select = {.where = norm_and(predicates, 2)},
	};

	*out = 0;
	if (rc == 0)
		rc = orm_status(&store->db,
				norm_join_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	const struct orm_integer *row = rc == 0 ? norm_rows_at(rows, 0) : NULL;

	if (row)
		*out = row->value;

	return rc;
}

int store_session_owned_by(struct session_store *store, const char *session_id, const char *user_id)
{
	int64_t id = 0;

	return owned_session_id(store, session_id, user_id, &id) == 0 && id != 0;
}

int store_list_sessions_page_json(struct session_store *store, const char *user_id, int limit,
				  int offset, char **out_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!store || !user_id || !out_json)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(&store->db, &scope);
	struct norm_error error = {0};
	const struct norm_table_ref sessions = norm_table(&session_model, NB_STR_LIT("s"));
	const struct norm_table_ref owners = norm_table(&orm_fcgi_session_owner, NB_STR_LIT("o"));
	const struct norm_join joins[] = {
		{.table = &sessions,
		 .kind = NORM_JOIN_INNER,
		 .on = norm_columns_eq(norm_table_col(&sessions, 1), norm_table_col(&owners, 0))},
	};
	const struct norm_join_column columns[] = {
		{.column = norm_table_col(&sessions, 1), .into = 0},
		{.column = norm_table_col(&sessions, 2), .into = 1},
		{.column = norm_table_col(&sessions, 3), .into = 2},
		{.column = norm_table_col(&sessions, SESSION_CREATED_AT), .into = 3},
		{.column = norm_table_col(&sessions, SESSION_UPDATED_AT), .into = 4},
	};
	const struct norm_order order = {
		.column = norm_table_col(&sessions, SESSION_UPDATED_AT),
		.direction = NORM_DESC,
	};
	const struct norm_join_query query = {
		.from = &owners,
		.joins = joins,
		.join_count = 1,
		.projection = &fcgi_session_view_model,
		.columns = columns,
		.column_count = NB_ARRAY_SIZE(columns),
		.select =
			{
				.where = norm_eq(norm_table_col(&owners, 1), orm_string(user_id)),
				.order = &order,
				.order_count = 1,
				.has_limit = limit > 0,
				.limit = limit > 0 ? (uint64_t)limit + 1 : 0,
				.offset = offset > 0 ? (uint64_t)offset : 0,
				.max_rows = SIZE_MAX,
				.max_bytes = SIZE_MAX,
			},
	};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_join_select(scope.connection, &query, NULL, &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	root = cJSON_CreateObject();
	cJSON *items = root ? cJSON_AddArrayToObject(root, "items") : NULL;

	if (!items)
		return NB_ERR_NOMEM;

	size_t count = norm_rows_count(rows);
	bool more = limit > 0 && count > (size_t)limit;

	if (more)
		count = (size_t)limit;

	for (size_t i = 0; i < count; i++) {
		const struct fcgi_session_view *row = norm_rows_at(rows, i);
		cJSON *item = cJSON_CreateObject();

		if (!item)
			return NB_ERR_NOMEM;

		cJSON_AddStringToObject(item, "id", row->display_id.ptr);
		cJSON_AddStringToObject(item, "name", row->name.ptr);
		cJSON_AddStringToObject(item, "model", row->missing_model ? "" : row->model.ptr);
		cJSON_AddNumberToObject(item, "created_at", (double)row->created_at);
		cJSON_AddNumberToObject(item, "updated_at", (double)row->updated_at);
		cJSON_AddItemToArray(items, item);
	}

	cJSON_AddNumberToObject(root, "limit", limit > 0 ? limit : 0);
	cJSON_AddNumberToObject(root, "offset", offset > 0 ? offset : 0);
	cJSON_AddBoolToObject(root, "has_more", more);
	char *json = cJSON_PrintUnformatted(root);

	if (!json)
		return NB_ERR_NOMEM;

	*out_json = json;
	return 0;
}

int store_list_sessions_json(struct session_store *store, const char *user_id, char **out_json)
{
	return store_list_sessions_page_json(store, user_id, 0, 0, out_json);
}

static int update_session_write(struct session_store *store, const char *session_id,
				const char *user_id, const char *name, const char *model)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_write_result result = {0};
	struct norm_error error = {0};
	struct norm_assignment set[3] = {0};
	size_t count = 0;
	int64_t id = 0;
	int rc = owned_session_id(store, session_id, user_id, &id);

	if (rc != 0 || !id)
		return rc != 0 ? rc : NB_ERR_NOT_FOUND;

	if (name)
		set[count++] = (struct norm_assignment){.column = norm_col(&session_model, 2),
							.value = orm_string(name)};

	if (model)
		set[count++] = (struct norm_assignment){.column = norm_col(&session_model, 3),
							.value = orm_string(model)};

	set[count++] =
		(struct norm_assignment){.column = norm_col(&session_model, SESSION_UPDATED_AT),
					 .value = norm_i64(fcgi_store_now())};
	const struct norm_update_query query = {
		.set = set,
		.set_count = count,
		.where = norm_eq(norm_col(&session_model, 0), norm_i64(id)),
	};

	rc = orm_scope_begin(&store->db, &scope);
	if (rc == 0)
		rc = orm_status(&store->db,
				norm_update(scope.connection, &session_model, &query, NULL, &result,
					    &error),
				&error);

	return rc != 0 ? rc : result.affected_rows > 0 ? 0 : NB_ERR_NOT_FOUND;
}

int store_update_session(struct session_store *store, const char *session_id, const char *user_id,
			 const char *name, const char *model)
{
	if (!store || (!name && !model))
		return NB_ERR_INVAL;

	int rc = fcgi_store_transaction_begin(store);

	if (rc != 0)
		return rc;

	return fcgi_store_transaction_end(
		store, update_session_write(store, session_id, user_id, name, model));
}

int store_messages_json(struct session_store *s, const char *session_id, const char *user_id,
			int limit, int64_t before, char **out_json)
{
	struct message *head;
	struct message *m;
	cJSON *root;
	cJSON *items;
	int total = 0;
	int skipped;
	int added = 0;
	int64_t internal_id = 0;
	char *json;

	if (limit <= 0 || limit > 500)
		limit = 100;
	int lookup_rc = owned_session_id(s, session_id, user_id, &internal_id);
	if (lookup_rc != 0)
		return lookup_rc;
	if (internal_id == 0)
		return NB_ERR_NOT_FOUND;

	head = message_list(&s->db, internal_id, &total);
	if (!head && total > 0)
		return NB_ERR_IO;
	for (m = head; m; m = m->next)
		if (before <= 0 || m->id < before)
			added++;
	skipped = added > limit ? added - limit : 0;

	root = cJSON_CreateObject();
	items = root ? cJSON_AddArrayToObject(root, "items") : NULL;
	if (!root || !items) {
		cJSON_Delete(root);
		message_free_list(head);
		return NB_ERR_NOMEM;
	}
	added = 0;
	for (m = head; m; m = m->next) {
		cJSON *item;

		if (before > 0 && m->id >= before)
			continue;
		if (added++ < skipped)
			continue;
		item = cJSON_CreateObject();
		if (!item) {
			cJSON_Delete(root);
			message_free_list(head);
			return NB_ERR_NOMEM;
		}
		cJSON_AddNumberToObject(item, "id", (double)m->id);
		cJSON_AddStringToObject(item, "role", m->role);
		cJSON_AddStringToObject(item, "content", m->content ? m->content : "");
		if (m->turn_id)
			cJSON_AddStringToObject(item, "turn_id", m->turn_id);
		cJSON_AddNumberToObject(item, "token_count", m->token_count);
		cJSON_AddBoolToObject(item, "compressed", m->compressed != 0);
		cJSON_AddNumberToObject(item, "created_at", (double)m->created_at);
		cJSON_AddItemToArray(items, item);
	}
	message_free_list(head);
	cJSON_AddNumberToObject(root, "limit", limit);
	cJSON_AddBoolToObject(root, "has_more", skipped > 0);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		return NB_ERR_NOMEM;
	*out_json = json;
	return 0;
}

static int delete_session_write(struct session_store *store, const char *session_id,
				const char *user_id)
{
	static const struct {
		const struct norm_model *model;
		size_t session_column;
	} deletions[] = {
		{&orm_fcgi_session_owner, 0}, {&orm_fcgi_events, 1},
		{&orm_fcgi_actions, 1},	      {&orm_fcgi_canvas_nodes, 1},
		{&orm_fcgi_artifacts, 2},     {&orm_fcgi_running_turns, 2},
	};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	int64_t id = 0;
	int rc = owned_session_id(store, session_id, user_id, &id);

	if (rc != 0)
		return rc;

	if (!id)
		return NB_ERR_NOT_FOUND;

	rc = orm_scope_begin(&store->db, &scope);
	struct norm_error error = {0};
	const size_t fields[] = {0};
	const struct norm_select_query running_query = {
		.fields = fields,
		.field_count = 1,
		.where = norm_eq(norm_col(&orm_fcgi_running_turns, 2), orm_string(session_id)),
		.has_limit = true,
		.limit = 1,
	};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_select(scope.connection, &orm_fcgi_running_turns,
					    &running_query, NULL, &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (norm_rows_count(rows) > 0)
		return NB_ERR_AGAIN;

	for (size_t i = 0; rc == 0 && i < NB_ARRAY_SIZE(deletions); i++) {
		const struct norm_delete_query query = {
			.where = norm_eq(norm_col(deletions[i].model, deletions[i].session_column),
					 orm_string(session_id)),
		};
		rc = orm_status(&store->db,
				norm_delete(scope.connection, deletions[i].model, &query, NULL,
					    NULL, &error),
				&error);
	}

	if (rc == 0)
		rc = session_delete(&store->db, id);

	return rc;
}

int store_delete_session(struct session_store *store, const char *session_id, const char *user_id)
{
	if (!store || !session_id || !user_id)
		return NB_ERR_INVAL;

	int rc = fcgi_store_transaction_begin(store);

	if (rc != 0)
		return rc;

	return fcgi_store_transaction_end(store, delete_session_write(store, session_id, user_id));
}
