#ifndef DB_ERROR_MIGRATION_H
#define DB_ERROR_MIGRATION_H

struct db;

/* Converts pre-nbase persisted error codes once, under schema version 3. */
int db_migrate_error_codes(struct db *db);

#endif
