#ifndef MORPH_ORM_TEST_FIXTURE_H
#define MORPH_ORM_TEST_FIXTURE_H

#include "persistence/orm/database.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test-only model fixtures. No driver handle crosses this boundary. */
int orm_fixture_execute(struct db *db, const char *script);
int orm_fixture_integer(struct db *db, const char *query, int64_t *out);
int orm_fixture_text_copy(struct db *db, const char *query, char **out);
int orm_fixture_row_count(struct db *db, const char *query, size_t *out);
bool orm_fixture_has_column(struct db *db, const char *schema, const char *table,
			    const char *column);
bool orm_fixture_transaction_idle(struct db *db);
size_t orm_fixture_statement_count(struct db *db);
int orm_fixture_commit_hook(struct db *db, int (*callback)(void *), void *context);
int orm_fixture_progress_hook(struct db *db, int instructions, int (*callback)(void *),
			      void *context);
int orm_fixture_trace_hook(struct db *db, void (*callback)(void *, const char *), void *context);
int orm_fixture_scalar_error(struct db *db, const char *name, const char *message);
int orm_fixture_scalar_hook(struct db *db, const char *name, int (*callback)(void *),
			    void *context);

#ifdef __cplusplus
}
#endif

#endif
