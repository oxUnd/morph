#ifndef SKILL_H
#define SKILL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>
#include "nb/map.h"
#include "nb/vec.h"
#include "nb/cleanup.h"

#define SKILL_NAME_MAX_CHARS 64
#define SKILL_DESC_MAX_CHARS 1024
#define SKILL_NAME_MAX (SKILL_NAME_MAX_CHARS * 4 + 1)
#define SKILL_DESC_MAX (SKILL_DESC_MAX_CHARS * 4 + 1)
#define SKILL_PATH_MAX PATH_MAX
#define SKILL_METADATA_MAX 16
#define SKILL_METADATA_KEY_MAX (63 * 4 + 1)
#define SKILL_METADATA_VAL_MAX (255 * 4 + 1)

struct nb_log;

struct skill_metadata_entry {
	char key[SKILL_METADATA_KEY_MAX];
	char value[SKILL_METADATA_VAL_MAX];
};

struct skill_frontmatter {
	char name[SKILL_NAME_MAX];
	char description[SKILL_DESC_MAX];
	char license[255 * 4 + 1];
	char compatibility[511 * 4 + 1];
	char allowed_tools[1023 * 4 + 1];
	struct skill_metadata_entry metadata[SKILL_METADATA_MAX];
	int metadata_count;
};

struct skill_entry {
	struct nb_log *logger;
	struct skill_frontmatter fm;
	char skill_dir[SKILL_PATH_MAX];
	char skill_md_path[SKILL_PATH_MAX];
	char *body;
	int body_loaded;
	int enabled;
	int activated;
};

struct skill_registry {
	struct nb_log *logger;
	nb_vec entries;
	int count;
	nb_map *by_name;
};

void skill_registry_init(struct nb_log *logger, struct skill_registry *reg);
void skill_registry_cleanup(struct skill_registry *reg);
int skill_discover(struct skill_registry *reg, const char *dir_path);
/* Project skills take precedence; NULL workdir omits project discovery. */
int skill_discover_configured(struct skill_registry *reg, const char *workdir,
			     const char *user_dir);
/* Registry-owned slot, valid until registry cleanup; activation mutates it. */
struct skill_entry *skill_borrow(struct skill_registry *reg, const char *name);
/* Entries retain their addresses across subsequent registry additions. */
struct skill_entry *skill_entry_borrow_at(const struct skill_registry *reg, size_t index);
/* Copies an entry, including its owned body. Failure leaves the registry unchanged. */
int skill_registry_add_copy(struct skill_registry *reg, const struct skill_entry *source);
/* Sort a pointer view without moving registry entries or invalidating lookups. */
int skill_sorted_entries_borrow(const struct skill_registry *reg,
				const struct skill_entry **entries, size_t capacity);
int skill_sorted_entries_create(const struct skill_registry *reg,
				const struct skill_entry ***out);
void skill_entry_list_cleanup(const struct skill_entry ***entries);
int skill_activate(struct skill_entry *skill);
void skill_deactivate(struct skill_entry *skill);
void skill_deactivate_all(struct skill_registry *reg);
/* Independent malloc-owned instructions, or NULL for no instructions.
 * out must be NULL; failure preserves it. Release with free(). */
int skill_activated_instructions_create(struct skill_registry *reg, char **out);
int skill_build_catalog(struct skill_registry *reg, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif
