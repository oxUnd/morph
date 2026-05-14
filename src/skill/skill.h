#ifndef SKILL_H
#define SKILL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define SKILL_NAME_MAX 65
#define SKILL_DESC_MAX 1025
#define SKILL_MAX_ENTRIES 64
#define SKILL_PATH_MAX 512
#define SKILL_METADATA_MAX 16
#define SKILL_METADATA_KEY_MAX 64
#define SKILL_METADATA_VAL_MAX 256

struct skill_metadata_entry {
	char key[SKILL_METADATA_KEY_MAX];
	char value[SKILL_METADATA_VAL_MAX];
};

struct skill_frontmatter {
	char name[SKILL_NAME_MAX];
	char description[SKILL_DESC_MAX];
	char license[256];
	char compatibility[512];
	char allowed_tools[1024];
	struct skill_metadata_entry metadata[SKILL_METADATA_MAX];
	int metadata_count;
};

struct skill_entry {
	struct skill_frontmatter fm;
	char skill_dir[SKILL_PATH_MAX];
	char skill_md_path[SKILL_PATH_MAX];
	char *body;
	int body_loaded;
	int enabled;
	int activated;
};

struct skill_registry {
	struct skill_entry entries[SKILL_MAX_ENTRIES];
	int count;
};

void skill_registry_init(struct skill_registry *reg);
void skill_registry_cleanup(struct skill_registry *reg);
int skill_discover(struct skill_registry *reg, const char *dir_path);
struct skill_entry *skill_lookup(struct skill_registry *reg, const char *name);
int skill_activate(struct skill_entry *skill);
void skill_deactivate(struct skill_entry *skill);
void skill_deactivate_all(struct skill_registry *reg);
char *skill_build_activated_instructions(struct skill_registry *reg);
int skill_build_catalog(struct skill_registry *reg, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif
