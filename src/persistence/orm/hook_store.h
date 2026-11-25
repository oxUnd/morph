#ifndef MORPH_ORM_HOOK_STORE_H
#define MORPH_ORM_HOOK_STORE_H

#include "persistence/orm/database.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct store_hook_trust {
	char *digest;
	char *definition;
	int disabled;
};

struct store_hook_choice {
	const char *id;
	const char *digest;
	const char *definition;
};

void hook_store_trust_cleanup(struct store_hook_trust *record);
int hook_store_init(struct db *db);
int hook_store_trust_copy(struct db *db, const char *id, struct store_hook_trust *record);
int hook_store_workspace_trusted(struct db *db, const char *path);
int hook_store_workspace_set(struct db *db, const char *path, int trusted);
int hook_store_migrate_legacy(struct db *db, const char *id, const char *digest,
			      const char *legacy_id, const char *legacy_digest);
int hook_store_action(struct db *db, const char *id, const char *action, const char *digest,
		      const char *definition);
int hook_store_batch_write(struct db *db, const char *workspace, int trust,
			   const struct store_hook_choice *choices, size_t count);

#ifdef __cplusplus
}
#endif

#endif
