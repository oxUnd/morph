#ifndef MORPH_ORM_DATABASE_H
#define MORPH_ORM_DATABASE_H

#include <stdbool.h>
#include <limits.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nb_log;
struct orm_database;

struct db {
	struct nb_log *logger;
	struct orm_database *storage;
	char path[PATH_MAX];
};

struct db_lock {
	struct orm_database *storage;
};

bool db_is_open(const struct db *db);
int db_lock_acquire(struct db *db, struct db_lock *lock);
void db_lock_cleanup(struct db_lock *lock);
int db_open(struct nb_log *logger, struct db *db, const char *path);
int db_open_readonly(struct nb_log *logger, struct db *db, const char *path);
/* Writable maintenance/backup connection without changing journal mode. */
int db_open_plain(struct nb_log *logger, struct db *db, const char *path);
void db_close(struct db *db);
int db_init_schema(struct db *db);
int db_init_credit_schema(struct db *db);

#ifdef __cplusplus
}
#endif

#endif
