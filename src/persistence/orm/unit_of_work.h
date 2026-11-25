#ifndef MORPH_ORM_UNIT_OF_WORK_H
#define MORPH_ORM_UNIT_OF_WORK_H

#include "persistence/orm/database.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A successful begin retains write ownership until commit/rollback on the same
 * thread. Commit releases that ownership even if committing fails. */
int store_transaction_begin(struct db *db);
int store_transaction_commit(struct db *db);
int store_transaction_rollback(struct db *db);
/* The caller retains a db_lock across both calls and all enclosed model actions.
 * Nested savepoints preserve a caller-owned outer transaction. */
int store_savepoint_begin(struct db *db, const char *name);
int store_savepoint_finish(struct db *db, const char *name, int status);

#ifdef __cplusplus
}
#endif

#endif
