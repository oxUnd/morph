#ifndef DB_ERROR_MIGRATION_H
#define DB_ERROR_MIGRATION_H

#include "nb/buf.h"

struct db;

/* Converts pre-nbase persisted error codes once, under schema version 3. */
int db_migrate_error_codes(struct db *db);

/* Appends an expression for a trusted SQL column at a legacy import boundary. */
int db_error_migration_append_status_sql(nb_buf *sql, const char *value);

#endif
