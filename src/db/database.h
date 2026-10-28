#ifndef DATABASE_H
#define DATABASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>

struct morph_log;

struct db {
	struct morph_log *logger;
	sqlite3 *handle;
	char path[PATH_MAX];
};

int db_open(struct morph_log *logger, struct db *db, const char *path);
void db_close(struct db *db);
int db_exec(struct db *db, const char *sql);
int db_init_schema(struct db *db);

#ifdef __cplusplus
}
#endif

#endif
