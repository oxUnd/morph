#include "unit_of_work.h"
#include "internal.h"
#include "errors.h"

int store_transaction_begin(struct db *db)
{
	int rc = orm_write_begin(db, NORM_SQLITE_IMMEDIATE);

	return rc == NB_ERR_STATE ? MORPH_ERR_DB : rc;
}

int store_transaction_commit(struct db *db)
{
	return orm_write_finish(db, 0);
}

int store_transaction_rollback(struct db *db)
{
	int rc = orm_write_finish(db, NB_ERR_CANCELLED);

	return rc == NB_ERR_CANCELLED ? 0 : rc;
}

int store_savepoint_begin(struct db *db, const char *name)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc == 0 ? orm_savepoint_begin(&scope, name) : rc;
}

int store_savepoint_finish(struct db *db, const char *name, int status)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	int rc = orm_scope_begin(db, &scope);

	return rc == 0 ? orm_savepoint_finish(&scope, name, status) : status ? status : rc;
}
