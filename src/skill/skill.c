#include "skill.h"
#include "nb/cleanup.h"
#include "skill_parse.h"
#include "nb/log.h"
#include "nb/file.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/utf8.h"
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

void skill_registry_init(struct nb_log *logger, struct skill_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
	reg->logger = logger;
	reg->by_name = nb_map_new_with_cap(16);
}

void skill_registry_cleanup(struct skill_registry *reg)
{
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++) {
		struct skill_entry **entry = nb_vec_at(&reg->entries, (size_t)i);
		skill_entry_cleanup(entry);
	}
	reg->count = 0;
	nb_map_free(reg->by_name);
	reg->by_name = NULL;
	nb_vec_cleanup(&reg->entries);
}

struct skill_entry *skill_entry_borrow_at(const struct skill_registry *reg, size_t index)
{
	struct skill_entry **entry = reg ? nb_vec_at(&reg->entries, index) : NULL;
	return entry ? *entry : NULL;
}

struct skill_entry *skill_borrow(struct skill_registry *reg, const char *name)
{
	if (!reg || !name)
		return NULL;
	return nb_map_get_cstr(reg->by_name, name);
}

int skill_registry_add_copy(struct skill_registry *reg, const struct skill_entry *source)
{
	struct skill_entry *entry NB_CLEANUP(skill_entry_cleanup) = NULL;
	struct skill_entry **slot;
	int rc;

	if (!reg || !source || !source->fm.name[0])
		return NB_ERR_INVAL;
	if (skill_borrow(reg, source->fm.name))
		return NB_ERR_EXISTS;
	if (reg->count == INT_MAX)
		return NB_ERR_RANGE;
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NB_ERR_NOMEM;
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
			return NB_ERR_NOMEM;
	}
	if (!reg->entries.elem_size) {
		rc = (nb_vec_init(&reg->entries, sizeof(entry)),
		      nb_vec_reserve(&reg->entries, 16) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	if (!reg->by_name) {
		reg->by_name = nb_map_new_with_cap(16);
		if (!reg->by_name)
			return NB_ERR_NOMEM;
	}
	slot = nb_vec_push(&reg->entries);
	if (!slot)
		return NB_ERR_NOMEM;
	if (!nb_map_put_cstr(reg->by_name, entry->fm.name, entry, NULL)) {
		nb_vec_pop(&reg->entries);
		return NB_ERR_NOMEM;
	}
	*slot = entry;
	entry = NULL;
	reg->count++;
	return 0;
}

int skill_discover_configured(struct skill_registry *reg, const char *workdir,
			     const char *user_dir)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};

	int rc = 0;
	int initial;

	if (!reg)
		return NB_ERR_INVAL;
	initial = reg->count;

	if (workdir && *workdir) {
		char *project = (nb_path_concat(&file_value_1, workdir, ".agents/skills") == NB_OK
					 ? strdup(nb_buf_cstr(&file_value_1))
					 : NULL);

		if (!project)
			return NB_ERR_NOMEM;
		char *resolved = (nb_path_expand(&file_value_2, project, NULL) == NB_OK &&
						  nb_path_canonical_missing(
							  &file_value_2, nb_buf_cstr(&file_value_2),
							  NULL) == NB_OK
					  ? strdup(nb_buf_cstr(&file_value_2))
					  : NULL);

		rc = resolved ? skill_discover(reg, resolved) : NB_ERR_NOMEM;
		free(resolved);
		free(project);
		if (rc < 0)
			return rc;
	}
	rc = skill_discover(reg, user_dir && *user_dir ? user_dir : "~/.morph/skills");
	if (rc < 0)
		return rc;
	char *default_dir = (nb_path_expand(&file_value_3, "~/.morph/skills", NULL) == NB_OK
				     ? strdup(nb_buf_cstr(&file_value_3))
				     : NULL);
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
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_5 = NB_OK;
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_6 = NB_OK;

	if (!reg || !dir_path)
		return NB_ERR_INVAL;

	char *expanded = (nb_path_expand(&file_value_4, dir_path, NULL) == NB_OK
				  ? strdup(nb_buf_cstr(&file_value_4))
				  : NULL);
	if (!expanded)
		return NB_ERR_NOMEM;

	struct stat st;
	if (stat(expanded, &st) != 0 || !S_ISDIR(st.st_mode)) {
		free(expanded);
		return 0;
	}

	DIR *dir = opendir(expanded);
	if (!dir) {
		int err = errno;
		free(expanded);
		return nb_status_from_errno(err, NULL);
	}

	int discovered = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char subdir[SKILL_PATH_MAX];
		if (((file_status_5 = nb_path_concat(&file_value_5, expanded, ent->d_name)) != NB_OK
			     ? file_status_5
		     : (file_status_5 = nb_str_copy_to(nb_buf_str(&file_value_5), subdir,
						       sizeof(subdir))) == NB_ERR_RANGE
			     ? NB_ERR_NAME_TOO_LONG
			     : file_status_5) != 0)
			continue;

		struct stat sub_st;
		if (stat(subdir, &sub_st) != 0 || !S_ISDIR(sub_st.st_mode))
			continue;

		char skill_md_path[SKILL_PATH_MAX];
		if (((file_status_6 = nb_path_concat(&file_value_6, subdir, "SKILL.md")) != NB_OK
			     ? file_status_6
		     : (file_status_6 = nb_str_copy_to(nb_buf_str(&file_value_6), skill_md_path,
						       sizeof(skill_md_path))) == NB_ERR_RANGE
			     ? NB_ERR_NAME_TOO_LONG
			     : file_status_6) != 0)
			continue;

		if (stat(skill_md_path, &sub_st) != 0) {
			nb_log_write(reg ? reg->logger : NULL, NB_LOG_DEBUG,
				     "skill_discover: no SKILL.md in %s", subdir);
			continue;
		}

		struct skill_frontmatter fm;
		memset(&fm, 0, sizeof(fm));
		int rc = skill_parse_frontmatter(reg ? reg->logger : NULL, skill_md_path, &fm);
		if (rc < 0) {
			nb_log_write(reg ? reg->logger : NULL, NB_LOG_WARN,
				     "skill_discover: failed to parse %s: %d", skill_md_path, rc);
			continue;
		}

		if (fm.description[0] == '\0') {
			nb_log_write(reg ? reg->logger : NULL, NB_LOG_WARN,
				     "skill_discover: skipping %s, missing description",
				     fm.name[0] ? fm.name : ent->d_name);
			continue;
		}

		if (fm.name[0] == '\0') {
			if (strlen(ent->d_name) >= sizeof(fm.name) ||
			    nb_utf8_validate(nb_str_from_cstr(ent->d_name), NULL) ||
			    nb_utf8_count(nb_str_from_cstr(ent->d_name)) > SKILL_NAME_MAX_CHARS) {
				nb_log_write(reg->logger, NB_LOG_WARN,
					     "skill_discover: skill directory name is too long");
				continue;
			}
			memcpy(fm.name, ent->d_name, strlen(ent->d_name) + 1);
			nb_log_write(reg ? reg->logger : NULL, NB_LOG_DEBUG,
				     "skill_discover: using dir name as skill name: %s",
				     ent->d_name);
		}

		if (skill_borrow(reg, fm.name)) {
			nb_log_write(
				reg ? reg->logger : NULL, NB_LOG_INFO,
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
		nb_log_write(reg ? reg->logger : NULL, NB_LOG_INFO, "skill_discover: found '%s'",
			     fm.name);
	}

	closedir(dir);
	free(expanded);
	return discovered;
}

int skill_activate(struct skill_entry *skill)
{
	if (!skill)
		return NB_ERR_INVAL;
	if (!skill->enabled)
		return NB_ERR_PERMISSION;
	if (skill->activated && skill->body_loaded)
		return 0;

	if (!skill->body_loaded) {
		char *body = NULL;
		struct skill_frontmatter fm_tmp;
		memset(&fm_tmp, 0, sizeof(fm_tmp));
		int rc = skill_parse_file(skill ? skill->logger : NULL, skill->skill_md_path,
					  &fm_tmp, &body);
		if (rc < 0) {
			nb_log_write(skill ? skill->logger : NULL, NB_LOG_ERROR,
				     "skill_activate: failed to read %s: %d", skill->skill_md_path,
				     rc);
			return rc;
		}
		free(skill->body);
		skill->body = body;
		skill->body_loaded = 1;
	}

	skill->activated = 1;
	nb_log_write(skill ? skill->logger : NULL, NB_LOG_INFO, "skill_activate: activated '%s'",
		     skill->fm.name);
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
	nb_log_write(skill ? skill->logger : NULL, NB_LOG_INFO,
		     "skill_deactivate: deactivated '%s'", skill->fm.name);
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
		return NB_ERR_INVAL;
	if ((size_t)reg->count > capacity)
		return NB_ERR_NO_SPACE;
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
	const struct skill_entry **entries NB_CLEANUP(skill_entry_list_cleanup) = NULL;
	int rc;

	if (!reg || !out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (!reg->count)
		return 0;
	entries = calloc((size_t)reg->count, sizeof(*entries));
	if (!entries)
		return NB_ERR_NOMEM;
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
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (!reg)
		return 0;
	const struct skill_entry **entries NB_CLEANUP(skill_entry_list_cleanup) = NULL;

	rc = skill_sorted_entries_create(reg, &entries);
	if (rc < 0)
		return rc;
	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, 8192) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	for (int i = 0; i < reg->count; i++) {
		const struct skill_entry *e = entries[i];

		if (!e->enabled || !e->activated || !e->body || !e->body[0])
			continue;
		rc = (nb_buf_printf(&buf, "<skill name=\"%s\" dir=\"%s\">\n%s\n</skill>\n\n",
				    e->fm.name, e->skill_dir, e->body)
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc != 0) {
			nb_buf_cleanup(&buf);
			return rc;
		}
	}
	if (buf.len) {
		*out = (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
		if (!*out)
			rc = NB_ERR_NOMEM;
	}
	nb_buf_cleanup(&buf);
	return rc;
}

int skill_build_catalog(struct skill_registry *reg, char *buf, size_t buf_size)
{
	if (!reg || !buf || buf_size == 0)
		return NB_ERR_INVAL;
	const struct skill_entry **entries NB_CLEANUP(skill_entry_list_cleanup) = NULL;
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
			return NB_ERR_NO_SPACE;
		len += (size_t)n;
		written++;
	}

	if (written == 0)
		buf[0] = '\0';

	return written;
}
