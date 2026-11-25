#include <norm/query.h>
#include "persistence/orm/fcgi_users.h"
#include "persistence/orm/fcgi_models.h"
#include "persistence/orm/internal.h"
#include "persistence/orm/records.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "sapi/fastcgi/security.h"
#include "errors.h"
#include <nb/str.h>
#include <stdlib.h>
#include <string.h>

static int username_valid(const char *username)
{
	if (!username)
		return 0;

	size_t length = strlen(username);

	return length >= 3 && length <= 63 && nb_str_is_alnum(nb_str_from_cstr(username), "_-.");
}

int store_setup_required(struct session_store *store)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&orm_fcgi_users, 5), norm_i64(0)),
		.has_limit = true,
		.limit = 1,
	};
	int rc = store ? orm_scope_begin(&store->db, &scope) : NB_ERR_INVAL;

	if (rc == 0)
		rc = orm_status(
			&store->db,
			norm_select(scope.connection, &orm_fcgi_users, &query, NULL, &rows, &error),
			&error);

	return rc != 0 || norm_rows_count(rows) == 0;
}

int store_create_user(struct session_store *store, const char *username, const char *password,
		      const char *role, char out_user_id[64])
{
	char user_id[64];
	char hash[192];
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	int rc;

	if (!store || !username_valid(username) || !password || strlen(password) < 8)
		return NB_ERR_INVAL;

	rc = fcgi_random_id("usr_", user_id, sizeof(user_id));
	if (rc == 0)
		rc = fcgi_password_hash(password, hash, sizeof(hash));

	if (rc == 0)
		rc = orm_scope_begin(&store->db, &scope);

	if (rc != 0)
		return rc;

	int64_t now = fcgi_store_now();
	struct orm_fcgi_users_row row = {
		.user_id = nb_str_from_cstr(user_id),
		.username = nb_str_from_cstr(username),
		.password_hash = nb_str_from_cstr(hash),
		.role = nb_str_from_cstr(role ? role : "user"),
		.created_at = now,
		.updated_at = now,
	};
	nb_status status =
		norm_insert(scope.connection, &orm_fcgi_users, &row, NULL, NULL, NULL, &error);

	if (status == NORM_ERR_UNIQUE || status == NORM_ERR_CONSTRAINT)
		return NB_ERR_EXISTS;

	rc = orm_status(&store->db, status, &error);
	const struct orm_fcgi_user_quota_row quota = {
		.user_id = nb_str_from_cstr(user_id),
		.profile = nb_str_from_cstr(role && strcmp(role, "admin") == 0 ? "admin" : "free"),
	};
	const struct norm_term profile = norm_term_excluded(norm_col(&orm_fcgi_user_quota, 1));
	const struct norm_assignment set[] = {
		{.column = norm_col(&orm_fcgi_user_quota, 1), .expression = &profile},
	};
	const size_t keys[] = {0};
	const struct norm_conflict update = {
		.action = NORM_CONFLICT_UPDATE,
		.fields = keys,
		.field_count = 1,
		.set = set,
		.set_count = 1,
	};
	const struct norm_insert_options quota_options = {.conflict = &update};
	const struct orm_fcgi_usage_storage_row storage = {.user_id = nb_str_from_cstr(user_id)};
	const struct norm_conflict ignore = {.action = NORM_CONFLICT_NOTHING};
	const struct norm_insert_options storage_options = {.conflict = &ignore};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_insert(scope.connection, &orm_fcgi_user_quota, &quota,
					    &quota_options, NULL, NULL, &error),
				&error);

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_insert(scope.connection, &orm_fcgi_usage_storage, &storage,
					    &storage_options, NULL, NULL, &error),
				&error);

	if (rc == 0 && out_user_id)
		rc = nb_str_copy_to(nb_str_from_cstr(user_id), out_user_id, 64);

	return rc;
}

int store_verify_user(struct session_store *store, const char *username, const char *password,
		      struct fcgi_user *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};

	if (!store || !username || !password)
		return 0;

	int rc = orm_scope_begin(&store->db, &scope);
	const struct norm_expr predicates[] = {
		norm_eq(norm_col(&orm_fcgi_users, 2), orm_string(username)),
		norm_eq(norm_col(&orm_fcgi_users, 5), norm_i64(0)),
	};
	const struct norm_select_query query = {.where = norm_and(predicates, 2)};

	if (rc == 0)
		rc = orm_status(
			&store->db,
			norm_select(scope.connection, &orm_fcgi_users, &query, NULL, &rows, &error),
			&error);

	const struct orm_fcgi_users_row *row = rc == 0 ? norm_rows_at(rows, 0) : NULL;

	if (!row || !fcgi_password_verify(password, row->password_hash.ptr))
		return 0;

	if (out) {
		rc = nb_str_copy_to(row->user_id, out->user_id, sizeof(out->user_id));
		if (rc == 0)
			rc = nb_str_copy_to(row->username, out->username, sizeof(out->username));

		if (rc == 0)
			rc = nb_str_copy_to(row->role, out->role, sizeof(out->role));
	}

	return rc == 0;
}
