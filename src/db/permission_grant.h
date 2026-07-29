#ifndef MORPH_PERMISSION_GRANT_H
#define MORPH_PERMISSION_GRANT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "database.h"
#include <stddef.h>

struct permission_grant {
	char subject[256];
	char resource_kind[32];
	char resource[PATH_MAX];
	char project_root[PATH_MAX];
};

typedef int (*permission_grant_iter_fn)(
	const struct permission_grant *grant, void *user_data);

int permission_grant_save(struct db *db,
			  const struct permission_grant *grant);
int permission_grant_each(struct db *db, const char *project_root,
			  permission_grant_iter_fn fn, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
