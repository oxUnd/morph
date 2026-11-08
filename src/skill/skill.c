#include "skill.h"
#include "util/cleanup.h"
#include "skill_parse.h"
#include "util/log.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/utf8.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static void skill_entry_cleanup(struct skill_entry **owner)
{
	if (!owner || !*owner)
		return;
	free((*owner)->body);
	free(*owner);
	*owner = NULL;
}

void skill_registry_init(struct morph_log *logger, struct skill_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
	reg->logger = logger;
	(void)morph_strmap_init(&reg->by_name, MORPH_ARRAY_INIT_CAP);
}

void skill_registry_cleanup(struct skill_registry *reg)
{
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++) {
		struct skill_entry **entry = morph_array_borrow(&reg->entries, (size_t)i);
		skill_entry_cleanup(entry);
	}
	reg->count = 0;
	morph_strmap_cleanup(&reg->by_name);
	morph_array_cleanup(&reg->entries);
}

struct skill_entry *skill_entry_borrow_at(const struct skill_registry *reg, size_t index)
{
	struct skill_entry **entry = reg ? morph_array_borrow(&reg->entries, index) : NULL;
	return entry ? *entry : NULL;
}

struct skill_entry *skill_borrow(struct skill_registry *reg, const char *name)
{
	if (!reg || !name)
		return NULL;
	return morph_strmap_borrow(&reg->by_name, name);
}

int skill_registry_add_copy(struct skill_registry *reg, const struct skill_entry *source)
{
	struct skill_entry *entry MORPH_CLEANUP(skill_entry_cleanup) = NULL;
	struct skill_entry **slot;
	int rc;

	if (!reg || !source || !source->fm.name[0])
		return -EINVAL;
	if (skill_borrow(reg, source->fm.name))
		return -EEXIST;
	if (reg->count == INT_MAX)
		return -EOVERFLOW;
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return -ENOMEM;
	entry->logger = source->logger;
	entry->fm = source->fm;
	memcpy(entry->skill_dir, source->skill_dir, sizeof(entry->skill_dir));
	memcpy(entry->skill_md_path, source->skill_md_path, sizeof(entry->skill_md_path));
	entry->enabled = source->enabled;
	entry->activated = source->activated;
	entry->body_loaded = source->body_loaded;
	if (source->body) {
		entry->body = strdup(source->body);
		if (!entry->body)
			return -ENOMEM;
	}
	if (!reg->entries.size) {
		rc = morph_array_init(&reg->entries, MORPH_ARRAY_INIT_CAP, sizeof(entry));
		if (rc != 0)
			return rc;
	}
	if (!reg->by_name.entries) {
		rc = morph_strmap_init(&reg->by_name, MORPH_ARRAY_INIT_CAP);
		if (rc != 0)
			return rc;
	}
	slot = morph_array_push_borrow(&reg->entries);
	if (!slot)
		return -ENOMEM;
	rc = morph_strmap_set_borrow(&reg->by_name, entry->fm.name, entry);
	if (rc != 0) {
		morph_array_pop(&reg->entries);
		return rc;
	}
	*slot = entry;
	entry = NULL;
	reg->count++;
	return 0;
}

int skill_discover_configured(struct skill_registry *reg, const char *workdir,
			     const char *user_dir)
{
	int rc = 0;
	int initial;

	if (!reg)
		return -EINVAL;
	initial = reg->count;

	if (workdir && *workdir) {
		char *project = file_path_join_alloc(workdir, ".agents/skills");

		if (!project)
			return -ENOMEM;
		char *resolved = file_resolve_path_copy(project);

		rc = resolved ? skill_discover(reg, resolved) : -ENOMEM;
		free(resolved);
		free(project);
		if (rc < 0)
			return rc;
	}
	rc = skill_discover(reg, user_dir && *user_dir ? user_dir : "~/.morph/skills");
	if (rc < 0)
		return rc;
	char *default_dir = file_expand_path_copy("~/.morph/skills");
	int use_shared = !user_dir || !*user_dir ||
		strcmp(user_dir, "~/.morph/skills") == 0 ||
		(default_dir && strcmp(user_dir, default_dir) == 0);

	free(default_dir);
	if (use_shared) {
		rc = skill_discover(reg, "~/.agents/skills");
		if (rc < 0)
			return rc;
	}
	return reg->count - initial;
}

int skill_discover(struct skill_registry *reg, const char *dir_path)
{
	if (!reg || !dir_path)
		return -EINVAL;

	char *expanded = file_expand_path_copy(dir_path);
	if (!expanded)
		return -ENOMEM;

	struct stat st;
	if (stat(expanded, &st) != 0 || !S_ISDIR(st.st_mode)) {
		free(expanded);
		return 0;
	}

	DIR *dir = opendir(expanded);
	if (!dir) {
		int err = errno;
		free(expanded);
		return -err;
	}

	int discovered = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char subdir[SKILL_PATH_MAX];
		if (file_path_join(subdir, sizeof(subdir),
				   expanded, ent->d_name) != 0)
			continue;

		struct stat sub_st;
		if (stat(subdir, &sub_st) != 0 || !S_ISDIR(sub_st.st_mode))
			continue;

		char skill_md_path[SKILL_PATH_MAX];
		if (file_path_join(skill_md_path, sizeof(skill_md_path),
				   subdir, "SKILL.md") != 0)
			continue;

		if (stat(skill_md_path, &sub_st) != 0) {
			log_dbg(reg ? reg->logger : NULL, "skill_discover: no SKILL.md in %s",
				subdir);
			continue;
		}

		struct skill_frontmatter fm;
		memset(&fm, 0, sizeof(fm));
		int rc = skill_parse_frontmatter(reg ? reg->logger : NULL, skill_md_path, &fm);
		if (rc < 0) {
			log_warn(reg ? reg->logger : NULL, "skill_discover: failed to parse %s: %d",
				 skill_md_path, rc);
			continue;
		}

		if (fm.description[0] == '\0') {
			log_warn(reg ? reg->logger : NULL,
				 "skill_discover: skipping %s, missing description",
				 fm.name[0] ? fm.name : ent->d_name);
			continue;
		}

		if (fm.name[0] == '\0') {
			if (strlen(ent->d_name) >= sizeof(fm.name) || utf8valid(ent->d_name) ||
			    utf8len(ent->d_name) > SKILL_NAME_MAX_CHARS) {
				log_warn(reg->logger,
				"skill_discover: skill directory name is too long");
				continue;
			}
			memcpy(fm.name, ent->d_name, strlen(ent->d_name) + 1);
			log_dbg(reg ? reg->logger : NULL,
				"skill_discover: using dir name as skill name: %s", ent->d_name);
		}

		if (skill_borrow(reg, fm.name)) {
			log_info(reg ? reg->logger : NULL,
				 "skill_discover: '%s' already registered, skipping duplicate from "
				 "%s",
				 fm.name, subdir);
			continue;
		}

		struct skill_entry e = {.logger = reg->logger, .fm = fm, .enabled = 1};
		memcpy(e.skill_dir, subdir, strlen(subdir) + 1);
		memcpy(e.skill_md_path, skill_md_path, strlen(skill_md_path) + 1);
		rc = skill_registry_add_copy(reg, &e);
		if (rc != 0) {
			closedir(dir);
			free(expanded);
			return rc;
		}
		discovered++;
		log_info(reg ? reg->logger : NULL, "skill_discover: found '%s'", fm.name);
	}

	closedir(dir);
	free(expanded);
	return discovered;
}

int skill_activate(struct skill_entry *skill)
{
	if (!skill)
		return -EINVAL;
	if (!skill->enabled)
		return -EACCES;
	if (skill->activated && skill->body_loaded)
		return 0;

	if (!skill->body_loaded) {
		char *body = NULL;
		struct skill_frontmatter fm_tmp;
		memset(&fm_tmp, 0, sizeof(fm_tmp));
		int rc = skill_parse_file(skill ? skill->logger : NULL, skill->skill_md_path,
					  &fm_tmp, &body);
		if (rc < 0) {
			log_err(skill ? skill->logger : NULL,
				"skill_activate: failed to read %s: %d", skill->skill_md_path, rc);
			return rc;
		}
		free(skill->body);
		skill->body = body;
		skill->body_loaded = 1;
	}

	skill->activated = 1;
	log_info(skill ? skill->logger : NULL, "skill_activate: activated '%s'", skill->fm.name);
	return 0;
}

void skill_deactivate(struct skill_entry *skill)
{
	if (!skill)
		return;
	free(skill->body);
	skill->body = NULL;
	skill->body_loaded = 0;
	skill->activated = 0;
	log_info(skill ? skill->logger : NULL, "skill_deactivate: deactivated '%s'",
		 skill->fm.name);
}

static int skill_entry_compare(const void *left, const void *right)
{
	const struct skill_entry *const *a = left;
	const struct skill_entry *const *b = right;

	return strcmp((*a)->fm.name, (*b)->fm.name);
}

int skill_sorted_entries_borrow(const struct skill_registry *reg,
				const struct skill_entry **entries, size_t capacity)
{
	if (!reg || !entries)
		return -EINVAL;
	if ((size_t)reg->count > capacity)
		return -ENOSPC;
	for (int i = 0; i < reg->count; i++)
		entries[i] = skill_entry_borrow_at(reg, (size_t)i);
	qsort(entries, (size_t)reg->count, sizeof(entries[0]), skill_entry_compare);
	return reg->count;
}

void skill_entry_list_cleanup(const struct skill_entry ***entries)
{
	if (!entries)
		return;
	free(*entries);
	*entries = NULL;
}

int skill_sorted_entries_create(const struct skill_registry *reg,
				const struct skill_entry ***out)
{
	const struct skill_entry **entries MORPH_CLEANUP(skill_entry_list_cleanup) = NULL;
	int rc;

	if (!reg || !out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	if (!reg->count)
		return 0;
	entries = calloc((size_t)reg->count, sizeof(*entries));
	if (!entries)
		return -ENOMEM;
	rc = skill_sorted_entries_borrow(reg, entries, (size_t)reg->count);
	if (rc < 0)
		return rc;
	*out = entries;
	entries = NULL;
	return 0;
}

void skill_deactivate_all(struct skill_registry *reg)
{
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++)
		skill_deactivate(skill_entry_borrow_at(reg, (size_t)i));
}

int skill_activated_instructions_create(struct skill_registry *reg, char **out)
{
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc;

	if (!out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	if (!reg)
		return 0;
	const struct skill_entry **entries MORPH_CLEANUP(skill_entry_list_cleanup) = NULL;

	rc = skill_sorted_entries_create(reg, &entries);
	if (rc < 0)
		return rc;
	rc = morph_buf_init(&buf, 8192);
	if (rc != 0)
		return rc;
	for (int i = 0; i < reg->count; i++) {
		const struct skill_entry *e = entries[i];

		if (!e->enabled || !e->activated || !e->body || !e->body[0])
			continue;
		rc = morph_buf_printf(&buf,
			"<skill name=\"%s\" dir=\"%s\">\n%s\n</skill>\n\n",
			e->fm.name, e->skill_dir, e->body);
		if (rc != 0) {
			morph_buf_cleanup(&buf);
			return rc;
		}
	}
	if (buf.len) {
		*out = morph_buf_detach(&buf);
		if (!*out)
			rc = -ENOMEM;
	}
	morph_buf_cleanup(&buf);
	return rc;
}

int skill_build_catalog(struct skill_registry *reg, char *buf, size_t buf_size)
{
	if (!reg || !buf || buf_size == 0)
		return -EINVAL;
	const struct skill_entry **entries MORPH_CLEANUP(skill_entry_list_cleanup) = NULL;
	int rc = skill_sorted_entries_create(reg, &entries);

	if (rc < 0)
		return rc;

	size_t len = 0;
	int written = 0;

	for (int i = 0; i < reg->count; i++) {
		const struct skill_entry *e = entries[i];
		if (!e->enabled)
			continue;

		int n = snprintf(buf + len, buf_size - len, "- %s: %s\n",
				 e->fm.name, e->fm.description);
		if (n < 0 || (size_t)n >= buf_size - len)
			return -ENOSPC;
		len += (size_t)n;
		written++;
	}

	if (written == 0)
		buf[0] = '\0';

	return written;
}
