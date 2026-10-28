#ifndef MORPH_CLI_SKILL_STORE_H
#define MORPH_CLI_SKILL_STORE_H

struct morph_log;
#include "skill/skill.h"
#include "util/array.h"
#include "cJSON.h"

#define SKILL_INSTALL_METADATA ".morph-install.json"
#define SKILL_DIGEST_MAX 65

struct skill_candidate {
	struct skill_frontmatter fm;
	char path[PATH_MAX];
};

int skill_store_scan(struct morph_log *logger, const char *root, morph_array_t *candidates);
int skill_store_copy(const char *source, const char *destination);
int skill_store_remove(const char *path);
int skill_store_digest(const char *path, char digest[SKILL_DIGEST_MAX]);
cJSON *skill_store_metadata(const char *path);
int skill_store_unmodified(const char *path, const cJSON *metadata);
int skill_store_install(const char *root, const struct skill_candidate *candidate,
			cJSON *metadata, int replace);

#endif
