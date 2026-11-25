#ifndef MORPH_ORM_INTERNAL_H
#define MORPH_ORM_INTERNAL_H

#include "persistence/orm/database.h"
#include <norm/orm.h>
#include <norm/sqlite.h>
#include <norm/schema.h>
#include <norm/query.h>
#include <nb/cleanup.h>

/* The database retains one lease and serializes its use across threads.
 * Transactions retain their owner lock until completion. */
struct orm_scope {
	const struct db *database;
	struct norm_conn *connection;
	struct norm_call call;
};

bool orm_checkpoint_ready(const struct db *db);
void orm_checkpoint_set_ready(const struct db *db, bool ready);
const char *orm_checkpoint_token(const struct db *db);
int orm_checkpoint_set_token(const struct db *db, const char *token);

int orm_scope_begin(const struct db *db, struct orm_scope *scope);
void orm_scope_cleanup(struct orm_scope *scope);
const struct norm_call *orm_scope_call(const struct orm_scope *scope);
int orm_status(const struct db *db, nb_status status, const struct norm_error *error);
void orm_rows_cleanup(struct norm_rows **rows);
int orm_table_has_column(struct orm_scope *scope, const char *schema, const char *table,
			 const char *column, bool *has);
/* Typed operations share status mapping and preserve complete result reads. */
int orm_select_rows(struct orm_scope *scope, const struct norm_model *model,
		    const struct norm_select_query *query, struct norm_rows **rows);
int orm_read(struct orm_scope *scope, const struct norm_query *query, struct norm_rows **rows);
int orm_join_rows(struct orm_scope *scope, const struct norm_join_query *query,
		  struct norm_rows **rows);
int orm_insert_row(struct orm_scope *scope, const struct norm_model *model, const void *row,
		   const struct norm_insert_options *options, struct norm_write_result *result);
int orm_update_rows(struct orm_scope *scope, const struct norm_model *model,
		    const struct norm_assignment *assignments, size_t count, struct norm_expr where,
		    struct norm_write_result *result);
int orm_delete_rows(struct orm_scope *scope, const struct norm_model *model, struct norm_expr where,
		    struct norm_write_result *result);
int orm_insert_from(struct orm_scope *scope, const struct norm_model *model, const size_t *fields,
		    size_t count, const struct norm_query *query,
		    const struct norm_conflict *conflict, struct norm_write_result *result);

int orm_transaction_begin(struct orm_scope *scope, enum norm_sqlite_tx_mode mode);
/* A successful begin retains the owner lock until finish on the same thread. */
int orm_write_begin(struct db *db, enum norm_sqlite_tx_mode mode);
int orm_write_finish(struct db *db, int status);
int orm_transaction_commit(struct orm_scope *scope);
int orm_transaction_rollback(struct orm_scope *scope);
int orm_savepoint_begin(struct orm_scope *scope, const char *name);
int orm_savepoint_abort(struct orm_scope *scope, const char *name);
int orm_savepoint_finish(struct orm_scope *scope, const char *name, int status);

#endif
