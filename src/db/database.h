#ifndef DATABASE_H
#define DATABASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>

struct nb_log;

struct db {
	struct nb_log *logger;
	sqlite3 *handle;
	char path[PATH_MAX];
};

/* A short connection guard keeps multi-statement operations indivisible when
 * several tool workers share one FULLMUTEX connection. Never hold it for I/O. */
struct db_lock {
	sqlite3_mutex *mutex;
};

int db_lock_acquire(struct db *db, struct db_lock *lock);
void db_lock_cleanup(struct db_lock *lock);

int db_open(struct nb_log *logger, struct db *db, const char *path);
void db_close(struct db *db);
int db_exec(struct db *db, const char *sql);
int db_init_schema(struct db *db);
/* Initializes only the shared credit ledger, without unrelated application tables. */
int db_init_credit_schema(struct db *db);

#ifdef __cplusplus
}
#endif

#endif
