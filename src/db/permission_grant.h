#ifndef MORPH_PERMISSION_GRANT_H
#define MORPH_PERMISSION_GRANT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "database.h"
#include <stdint.h>
#include <stddef.h>

struct permission_grant {
	int64_t id;
	char subject[256];
	char resource_kind[32];
	char resource[PATH_MAX];
	char project_root[PATH_MAX];
	int64_t created_at;
};

typedef int (*permission_grant_iter_fn)(
	const struct permission_grant *grant, void *user_data);

int permission_grant_save(struct db *db,
			  const struct permission_grant *grant);
int permission_grant_each(struct db *db, const char *project_root,
			  permission_grant_iter_fn fn, void *user_data);
int permission_grant_delete_id(struct db *db, const char *project_root,
			       int64_t id, int *deleted);
int permission_grant_delete_subject(struct db *db, const char *project_root,
				    const char *subject, int *deleted);
int permission_grant_clear_project(struct db *db, const char *project_root,
				   int *deleted);
int permission_grant_clear_all(struct db *db, int *deleted);

#ifdef __cplusplus
}
#endif

#endif
