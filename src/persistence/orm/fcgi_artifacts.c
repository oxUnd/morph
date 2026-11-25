#include <norm/query.h>
#include "persistence/orm/fcgi_store.h"
#include "persistence/orm/fcgi_artifacts.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "sapi/fastcgi/security.h"
#include "persistence/orm/fcgi_models.h"
#include "persistence/orm/fcgi_quota_internal.h"
#include "persistence/orm/records.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *artifact_json_array(const struct norm_rows *rows)
{
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *items = root ? cJSON_AddArrayToObject(root, "items") : NULL;

	if (!items)
		return NULL;

	for (size_t i = 0; i < norm_rows_count(rows); i++) {
		const struct orm_fcgi_artifacts_row *row = norm_rows_at(rows, i);
		cJSON *item = cJSON_CreateObject();
		char url[128];

		if (!item)
			return NULL;

		int written = snprintf(url, sizeof(url), "/api/artifacts/%s", row->id.ptr);

		if (written < 0 || (size_t)written >= sizeof(url)) {
			cJSON_Delete(item);
			return NULL;
		}

		cJSON_AddStringToObject(item, "id", row->id.ptr);
		cJSON_AddStringToObject(item, "session_id", row->session_id.ptr);
		cJSON_AddStringToObject(item, "kind", row->kind.ptr);
		cJSON_AddStringToObject(item, "mime", row->mime.ptr);
		cJSON_AddStringToObject(item, "filename", row->filename.ptr);
		cJSON_AddNumberToObject(item, "size_bytes", (double)row->size_bytes);
		cJSON_AddStringToObject(item, "status", row->status.ptr);
		cJSON_AddNumberToObject(item, "created_at", (double)row->created_at);
		cJSON_AddStringToObject(item, "url", url);
		cJSON_AddItemToArray(items, item);
	}

	return cJSON_PrintUnformatted(root);
}

static int artifact_quota_allows(struct orm_scope *scope, const char *user_id, const char *kind,
				 int64_t size_bytes)
{
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!user_id || !kind || size_bytes < 0)
		return NB_ERR_INVAL;

	int rc = orm_fcgi_quota_copy(scope, user_id, &rows);

	if (rc != 0)
		return rc;

	const struct orm_fcgi_quota_view *row = norm_rows_at(rows, 0);

	if (!row)
		return 0;

	if (strcmp(kind, "image") == 0 && row->daily_images >= 0 &&
	    row->used_images >= row->daily_images)
		return NB_ERR_NO_SPACE;

	if (strcmp(kind, "video") == 0 && row->daily_videos >= 0 &&
	    row->used_videos >= row->daily_videos)
		return NB_ERR_NO_SPACE;

	if (row->storage_bytes >= 0 && (row->used_bytes > row->storage_bytes ||
					size_bytes > row->storage_bytes - row->used_bytes))
		return NB_ERR_NO_SPACE;

	return 0;
}

static int artifact_register_write(struct session_store *store, const char *user_id,
				   const char *session_id, const char *kind, const char *mime,
				   const char *filename, const char *relative_path,
				   int64_t size_bytes, char *id, size_t capacity)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc = orm_scope_begin(&store->db, &scope);

	if (rc == 0)
		rc = artifact_quota_allows(&scope, user_id, kind, size_bytes);

	if (rc == 0)
		rc = fcgi_random_id("art_", id, capacity);

	if (rc != 0)
		return rc;

	const struct orm_fcgi_artifacts_row row = {
		.id = nb_str_from_cstr(id),
		.user_id = nb_str_from_cstr(user_id),
		.session_id = nb_str_from_cstr(session_id),
		.kind = nb_str_from_cstr(kind),
		.mime = nb_str_from_cstr(mime),
		.filename = nb_str_from_cstr(filename),
		.relative_path = nb_str_from_cstr(relative_path),
		.size_bytes = size_bytes,
		.status = NB_STR_LIT("ready"),
		.created_at = fcgi_store_now(),
		.missing_expires_at = true,
	};

	rc = orm_status(
		&store->db,
		norm_insert(scope.connection, &orm_fcgi_artifacts, &row, NULL, NULL, NULL, &error),
		&error);
	if (rc == 0)
		rc = orm_fcgi_storage_add(&scope, user_id, size_bytes);

	if (rc == 0 && strcmp(kind, "image") == 0)
		rc = orm_fcgi_usage_add(&scope, user_id, 3, 1);

	if (rc == 0 && strcmp(kind, "video") == 0)
		rc = orm_fcgi_usage_add(&scope, user_id, 4, 1);

	return rc;
}

int store_artifact_register(struct session_store *store, const char *user_id,
			    const char *session_id, const char *kind, const char *mime,
			    const char *filename, const char *relative_path, int64_t size_bytes,
			    char out_artifact_id[64])
{
	char id[64];

	if (!store || !user_id || !session_id || !kind || !mime || !filename || !relative_path)
		return NB_ERR_INVAL;

	int rc = fcgi_store_transaction_begin(store);

	if (rc != 0)
		return rc;

	rc = fcgi_store_transaction_end(
		store, artifact_register_write(store, user_id, session_id, kind, mime, filename,
					       relative_path, size_bytes, id, sizeof(id)));
	if (rc == 0 && out_artifact_id)
		rc = nb_str_copy_to(nb_str_from_cstr(id), out_artifact_id, 64);

	return rc;
}

int store_artifact_get(struct session_store *store, const char *artifact_id, const char *user_id,
		       struct artifact_record *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};

	if (!store || !artifact_id || !user_id || !out)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(&store->db, &scope);
	const struct norm_expr predicates[] = {
		norm_eq(norm_col(&orm_fcgi_artifacts, 0), orm_string(artifact_id)),
		norm_eq(norm_col(&orm_fcgi_artifacts, 1), orm_string(user_id)),
	};
	const struct norm_select_query query = {.where = norm_and(predicates, 2)};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_select(scope.connection, &orm_fcgi_artifacts, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	const struct orm_fcgi_artifacts_row *row = norm_rows_at(rows, 0);

	if (!row)
		return NB_ERR_NOT_FOUND;

	struct artifact_record value = {0};
	struct text_field {
		nb_str text;
		char *destination;
		size_t capacity;
	};
	const struct text_field fields[] = {
		{row->id, value.id, sizeof(value.id)},
		{row->user_id, value.user_id, sizeof(value.user_id)},
		{row->session_id, value.session_id, sizeof(value.session_id)},
		{row->kind, value.kind, sizeof(value.kind)},
		{row->mime, value.mime, sizeof(value.mime)},
		{row->filename, value.filename, sizeof(value.filename)},
		{row->relative_path, value.relative_path, sizeof(value.relative_path)},
		{row->status, value.status, sizeof(value.status)},
	};

	for (size_t i = 0; rc == 0 && i < sizeof(fields) / sizeof(fields[0]); i++)
		rc = nb_str_copy_to(fields[i].text, fields[i].destination, fields[i].capacity);

	if (rc == 0) {
		value.size_bytes = row->size_bytes;
		value.created_at = row->created_at;
		*out = value;
	}

	return rc;
}

int store_artifact_list_json(struct session_store *store, const char *user_id,
			     const char *session_id, char **out_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};

	if (!store || !user_id || !session_id || !out_json)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(&store->db, &scope);
	const struct norm_expr predicates[] = {
		norm_eq(norm_col(&orm_fcgi_artifacts, 1), orm_string(user_id)),
		norm_eq(norm_col(&orm_fcgi_artifacts, 2), orm_string(session_id)),
	};
	const struct norm_order order = {
		.column = norm_col(&orm_fcgi_artifacts, 9),
		.direction = NORM_DESC,
	};
	const struct norm_select_query query = {
		.where = norm_and(predicates, 2),
		.order = &order,
		.order_count = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_select(scope.connection, &orm_fcgi_artifacts, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	char *json = artifact_json_array(rows);

	if (!json)
		return NB_ERR_NOMEM;

	*out_json = json;
	return 0;
}
