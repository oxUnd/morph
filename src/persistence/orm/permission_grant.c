#include "persistence/orm/permission_grant.h"
#include "persistence/orm/internal.h"
#include "persistence/orm/records.h"
#include <time.h>
#include <string.h>

struct grant_row {
	int64_t id;
	nb_str subject;
	nb_str resource_kind;
	nb_str resource;
	nb_str project_root;
	int64_t created_at;
};

static const struct norm_field grant_fields[] = {
	NORM_FIELD(struct grant_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_FIELD(struct grant_row, subject, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct grant_row, resource_kind, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct grant_row, resource, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct grant_row, project_root, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct grant_row, created_at, NORM_TYPE_I64, 0),
};
static const struct norm_model grant_model =
	NORM_MODEL(struct grant_row, "permission_grants", grant_fields);

int permission_grant_save(struct db *db, const struct permission_grant *grant)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc;

	if (!db_is_open(db) || !grant || !grant->subject[0] || !grant->resource_kind[0] ||
	    !grant->resource[0] || !grant->project_root[0])
		return NB_ERR_INVAL;

	struct grant_row row = {
		.subject = nb_str_from_cstr(grant->subject),
		.resource_kind = nb_str_from_cstr(grant->resource_kind),
		.resource = nb_str_from_cstr(grant->resource),
		.project_root = nb_str_from_cstr(grant->project_root),
		.created_at = (int64_t)time(NULL),
	};

	rc = orm_scope_begin(db, &scope);
	if (rc != NB_OK)
		return rc;

	rc = norm_insert(scope.connection, &grant_model, &row, NULL, NULL, NULL, &error);
	return rc == NORM_ERR_UNIQUE ? NB_OK : orm_status(db, rc, &error);
}

static int grant_copy(const struct grant_row *row, struct permission_grant *grant)
{
	memset(grant, 0, sizeof(*grant));
	grant->id = row->id;
	grant->created_at = row->created_at;
	int rc = nb_str_copy_to(row->subject, grant->subject, sizeof(grant->subject));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->resource_kind, grant->resource_kind,
				    sizeof(grant->resource_kind));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->resource, grant->resource, sizeof(grant->resource));

	if (rc == NB_OK)
		rc = nb_str_copy_to(row->project_root, grant->project_root,
				    sizeof(grant->project_root));

	return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
}

int permission_grant_each(struct db *db, const char *project_root, permission_grant_iter_fn fn,
			  void *user_data)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	struct norm_order order = {.column = norm_col(&grant_model, 0), .direction = NORM_ASC};
	struct norm_select_query query = {
		.where = norm_eq(norm_col(&grant_model, 4), orm_string(project_root)),
		.order = &order,
		.order_count = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};
	int rc;

	if (!db_is_open(db) || !project_root || !*project_root || !fn)
		return NB_ERR_INVAL;

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = orm_status(
			db,
			norm_select(scope.connection, &grant_model, &query, NULL, &rows, &error),
			&error);

	for (size_t index = 0; rc == NB_OK && index < norm_rows_count(rows); index++) {
		struct permission_grant grant;

		rc = grant_copy(norm_rows_at(rows, index), &grant);
		if (rc == NB_OK)
			rc = fn(&grant, user_data);
	}

	return rc;
}

static int delete_grants(struct db *db, const char *project_root, const char *subject, int64_t id,
			 int *deleted)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_write_result result = {0};
	struct norm_expr filters[3];
	size_t count = 0;
	int rc;

	if (!db_is_open(db) || !deleted)
		return NB_ERR_INVAL;

	*deleted = 0;
	if (project_root)
		filters[count++] = norm_eq(norm_col(&grant_model, 4), orm_string(project_root));

	if (subject)
		filters[count++] = norm_eq(norm_col(&grant_model, 1), orm_string(subject));

	if (id > 0)
		filters[count++] = norm_eq(norm_col(&grant_model, 0), norm_i64(id));

	struct norm_delete_query query = {.allow_all_rows = count == 0};

	if (count)
		query.where = norm_and(filters, count);

	rc = orm_scope_begin(db, &scope);
	if (rc == NB_OK)
		rc = orm_status(
			db,
			norm_delete(scope.connection, &grant_model, &query, NULL, &result, &error),
			&error);

	if (rc == NB_OK) {
		if (result.affected_rows > INT_MAX)
			return NB_ERR_RANGE;

		*deleted = (int)result.affected_rows;
	}

	return rc;
}

int permission_grant_delete_id(struct db *db, const char *project_root, int64_t id, int *deleted)
{
	if (!project_root || !*project_root || id <= 0)
		return NB_ERR_INVAL;

	return delete_grants(db, project_root, NULL, id, deleted);
}

int permission_grant_delete_subject(struct db *db, const char *project_root, const char *subject,
				    int *deleted)
{
	if (!project_root || !*project_root || !subject || !*subject)
		return NB_ERR_INVAL;

	return delete_grants(db, project_root, subject, 0, deleted);
}

int permission_grant_clear_project(struct db *db, const char *project_root, int *deleted)
{
	if (!project_root || !*project_root)
		return NB_ERR_INVAL;

	return delete_grants(db, project_root, NULL, 0, deleted);
}

int permission_grant_clear_all(struct db *db, int *deleted)
{
	return delete_grants(db, NULL, NULL, 0, deleted);
}
