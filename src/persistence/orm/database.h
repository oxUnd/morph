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
struct db_connection_config;
struct nb_cancel;

enum db_backend {
	DB_BACKEND_SQLITE = 0,
	DB_BACKEND_POSTGRESQL,
};

enum db_tls_mode {
	DB_TLS_DEFAULT = 0,
	DB_TLS_DISABLE,
	DB_TLS_REQUIRE,
	DB_TLS_VERIFY_CA,
	DB_TLS_VERIFY_FULL,
};

struct db_options {
	enum db_backend backend;
	const char *path;
	const char *url_env;
	enum db_tls_mode tls_mode;
	const char *ca_file;
	const char *cert_file;
	const char *key_file;
	int connect_timeout_ms;
	int query_timeout_ms;
};

struct db {
	struct nb_log *logger;
	struct orm_database *storage;
	char path[PATH_MAX];
};

struct db_lock {
	struct orm_database *storage;
};

bool db_is_open(const struct db *db);
enum db_backend db_backend(const struct db *db);
bool db_is_memory(const struct db *db);
/* Borrowed until cleared; the token must outlive all operations using it. */
int db_set_cancel(struct db *db, const struct nb_cancel *cancel);
int db_open_options(struct nb_log *logger, struct db *db, const struct db_options *options);
int db_connection_config_copy(const struct db *db, struct db_connection_config **out);
void db_connection_config_cleanup(struct db_connection_config **owner);
bool db_connection_config_matches(const struct db_connection_config *config,
				  const struct db *db);
int db_open_connection(struct nb_log *logger, struct db *db,
		       const struct db_connection_config *config);
int db_reopen(struct nb_log *logger, struct db *out, const struct db *source);
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
