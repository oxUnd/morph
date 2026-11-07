#ifndef MORPH_CLI_SERVICES_SKILL_STORE_H
#define MORPH_CLI_SERVICES_SKILL_STORE_H

struct morph_log;
#include "skill/skill.h"
#include "util/array.h"
#include "cJSON.h"

#define CLI_SKILL_STORE_INSTALL_METADATA ".morph-install.json"
#define CLI_SKILL_STORE_DIGEST_MAX 65

struct cli_skill_candidate {
	struct skill_frontmatter fm;
	char path[PATH_MAX];
};

int cli_skill_store_scan(struct morph_log *logger, const char *root, morph_array_t *candidates);
int cli_skill_store_copy(const char *source, const char *destination);
int cli_skill_store_remove(const char *path);
int cli_skill_store_digest(const char *path, char digest[CLI_SKILL_STORE_DIGEST_MAX]);
cJSON *cli_skill_store_metadata_copy(const char *path);
int cli_skill_store_unmodified(const char *path, const cJSON *metadata);
int cli_skill_store_install(const char *root, const struct cli_skill_candidate *candidate,
			    cJSON *metadata, int replace);

#endif
