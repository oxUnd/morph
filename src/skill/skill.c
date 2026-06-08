#include "skill.h"
#include "skill_parse.h"
#include "util/log.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

void skill_registry_init(struct skill_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
	(void)morph_strmap_init(&reg->by_name, SKILL_MAX_ENTRIES);
}

void skill_registry_cleanup(struct skill_registry *reg)
{
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++) {
		free(reg->entries[i].body);
		reg->entries[i].body = NULL;
		reg->entries[i].body_loaded = 0;
	}
	reg->count = 0;
	morph_strmap_cleanup(&reg->by_name);
}

static int find_skill(struct skill_registry *reg, const char *name)
{
	struct skill_entry *e;

	if (!reg || !name)
		return -1;
	e = (struct skill_entry *)morph_strmap_get(&reg->by_name, name);
	if (e)
		return (int)(e - reg->entries);
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->entries[i].fm.name, name) == 0)
			return i;
	}
	return -1;
}

struct skill_entry *skill_lookup(struct skill_registry *reg, const char *name)
{
	if (!reg || !name)
		return NULL;
	int idx = find_skill(reg, name);
	if (idx < 0)
		return NULL;
	return &reg->entries[idx];
}

int skill_discover(struct skill_registry *reg, const char *dir_path)
{
	if (!reg || !dir_path)
		return -EINVAL;

	char *expanded = file_expand_path(dir_path);
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
		MORPH_RETURN(-err);
	}

	int discovered = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char subdir[SKILL_PATH_MAX];
		if (strlen(expanded) + 1 + strlen(ent->d_name) >=
		    sizeof(subdir))
			continue;
		snprintf(subdir, sizeof(subdir), "%s/%s", expanded, ent->d_name);

		struct stat sub_st;
		if (stat(subdir, &sub_st) != 0 || !S_ISDIR(sub_st.st_mode))
			continue;

		char skill_md_path[SKILL_PATH_MAX];
		size_t subdir_len = strlen(subdir);
		const char skill_md_suffix[] = "/SKILL.md";
		size_t suffix_len = sizeof(skill_md_suffix) - 1;
		if (subdir_len + suffix_len >= sizeof(skill_md_path))
			continue;
		memcpy(skill_md_path, subdir, subdir_len);
		memcpy(skill_md_path + subdir_len, skill_md_suffix,
		       suffix_len + 1);

		if (stat(skill_md_path, &sub_st) != 0) {
			log_dbg("skill_discover: no SKILL.md in %s", subdir);
			continue;
		}

		struct skill_frontmatter fm;
		memset(&fm, 0, sizeof(fm));
		int rc = skill_parse_frontmatter(skill_md_path, &fm);
		if (rc < 0) {
			log_warn("skill_discover: failed to parse %s: %d",
				 skill_md_path, rc);
			continue;
		}

		if (fm.description[0] == '\0') {
			log_warn("skill_discover: skipping %s, missing description",
				 fm.name[0] ? fm.name : ent->d_name);
			continue;
		}

		if (fm.name[0] == '\0') {
			strncpy(fm.name, ent->d_name, sizeof(fm.name) - 1);
			log_dbg("skill_discover: using dir name as skill name: %s",
				ent->d_name);
		}

		int existing = find_skill(reg, fm.name);
		if (existing >= 0) {
			log_info("skill_discover: '%s' already registered, skipping duplicate from %s",
				 fm.name, subdir);
			continue;
		}

		if (reg->count >= SKILL_MAX_ENTRIES) {
			log_warn("skill_discover: registry full, skipping %s", fm.name);
			continue;
		}

		struct skill_entry *e = &reg->entries[reg->count];
		memset(e, 0, sizeof(*e));
		e->fm = fm;
		strncpy(e->skill_dir, subdir, sizeof(e->skill_dir) - 1);
		strncpy(e->skill_md_path, skill_md_path,
			sizeof(e->skill_md_path) - 1);
		e->enabled = 1;
		e->activated = 0;
		e->body = NULL;
		e->body_loaded = 0;

		(void)morph_strmap_set(&reg->by_name, e->fm.name, e);
		reg->count++;
		discovered++;
		log_info("skill_discover: found '%s'", fm.name);
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
		int rc = skill_parse_file(skill->skill_md_path, &fm_tmp, &body);
		if (rc < 0) {
			log_err("skill_activate: failed to read %s: %d",
				skill->skill_md_path, rc);
			return rc;
		}
		free(skill->body);
		skill->body = body;
		skill->body_loaded = 1;
	}

	skill->activated = 1;
	log_info("skill_activate: activated '%s'", skill->fm.name);
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
	log_info("skill_deactivate: deactivated '%s'", skill->fm.name);
}

void skill_deactivate_all(struct skill_registry *reg)
{
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++)
		skill_deactivate(&reg->entries[i]);
}

char *skill_build_activated_instructions(struct skill_registry *reg)
{
	if (!reg)
		return NULL;

	morph_buf_t buf;
	int rc = morph_buf_init(&buf, 8192);
	if (rc != 0)
		return NULL;

	for (int i = 0; i < reg->count; i++) {
		struct skill_entry *e = &reg->entries[i];
		if (!e->activated || !e->body || !e->body[0])
			continue;
		rc = morph_buf_printf(&buf,
				      "<skill name=\"%s\" dir=\"%s\">\n%s\n</skill>\n\n",
				      e->fm.name, e->skill_dir, e->body);
		if (rc != 0) {
			morph_buf_cleanup(&buf);
			return NULL;
		}
	}

	if (buf.len == 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}

	return morph_buf_detach(&buf);
}

int skill_build_catalog(struct skill_registry *reg, char *buf, size_t buf_size)
{
	if (!reg || !buf || buf_size == 0)
		return -EINVAL;

	size_t len = 0;
	int written = 0;

	for (int i = 0; i < reg->count; i++) {
		struct skill_entry *e = &reg->entries[i];
		if (!e->enabled)
			continue;

		int n = snprintf(buf + len, buf_size - len, "- %s: %s\n",
				 e->fm.name, e->fm.description);
		if (n < 0 || (size_t)n >= buf_size - len)
			break;
		len += (size_t)n;
		written++;
	}

	if (written == 0)
		buf[0] = '\0';

	return written;
}
