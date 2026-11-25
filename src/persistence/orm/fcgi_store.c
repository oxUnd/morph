#include "persistence/orm/fcgi_store.h"
#include "errors.h"
#include "persistence/orm/fcgi_models.h"
#include "persistence/orm/internal.h"
#include "sapi/fastcgi/session_store_internal.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

int fcgi_store_transaction_begin(struct session_store *store)
{
	int rc = orm_write_begin(&store->db, NORM_SQLITE_IMMEDIATE);

	return rc == 0 ? 0 : NB_ERR_IO;
}

int fcgi_store_transaction_end(struct session_store *store, int result)
{
	int rc = orm_write_finish(&store->db, result);

	return rc == MORPH_ERR_DB || rc == NB_ERR_STATE ? NB_ERR_IO : rc;
}

static int store_env_int(const char *name, int fallback)
{
	const char *value = getenv(name);
	char *end = NULL;

	if (!value || !*value)
		return fallback;

	errno = 0;
	long parsed = strtol(value, &end, 10);

	if (errno != 0 || end == value || *end != '\0' || parsed < 0)
		return fallback;

	return parsed > INT_MAX ? INT_MAX : (int)parsed;
}

int store_maintenance(struct session_store *store)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int event_days = store_env_int("MORPH_FCGI_EVENT_RETENTION_DAYS", 7);
	int action_days = store_env_int("MORPH_FCGI_ACTION_RETENTION_DAYS", 1);
	int stale_seconds = store_env_int("MORPH_FCGI_TURN_STALE_SECONDS", 120);
	int64_t now = fcgi_store_now();
	int rc = store ? orm_scope_begin(&store->db, &scope) : NB_ERR_INVAL;
	struct norm_error error = {0};
	const struct norm_expr actions[] = {
		norm_eq(norm_col(&orm_fcgi_actions, 5), norm_i64(1)),
		norm_lt(norm_col(&orm_fcgi_actions, 4),
			norm_i64(now - (int64_t)action_days * 86400)),
	};
	const struct norm_expr artifacts[] = {
		norm_is_not_null(norm_col(&orm_fcgi_artifacts, 10)),
		norm_lt(norm_col(&orm_fcgi_artifacts, 10), norm_i64(now)),
	};
	const struct norm_delete_query events_query = {
		.where = norm_lt(norm_col(&orm_fcgi_events, 4),
				 norm_i64(now - (int64_t)event_days * 86400)),
	};
	const struct norm_delete_query actions_query = {.where = norm_and(actions, 2)};
	const struct norm_delete_query turns_query = {
		.where = norm_lt(norm_col(&orm_fcgi_running_turns, 4),
				 norm_i64(now - stale_seconds)),
	};
	const struct norm_delete_query artifacts_query = {.where = norm_and(artifacts, 2)};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_delete(scope.connection, &orm_fcgi_events, &events_query, NULL,
					    NULL, &error),
				&error);

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_delete(scope.connection, &orm_fcgi_actions, &actions_query,
					    NULL, NULL, &error),
				&error);

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_delete(scope.connection, &orm_fcgi_running_turns, &turns_query,
					    NULL, NULL, &error),
				&error);

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_delete(scope.connection, &orm_fcgi_artifacts, &artifacts_query,
					    NULL, NULL, &error),
				&error);

	return rc;
}
