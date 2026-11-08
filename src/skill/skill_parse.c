#include "skill_parse.h"
#include "util/log.h"
#include "util/cleanup.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SKILL_FRONTMATTER_MAX_BYTES (1024u * 1024u)

static void skill_file_cleanup(FILE **owner)
{
	if (*owner)
		fclose(*owner);
	*owner = NULL;
}

static const char *find_closing_delim(const char *data, size_t len)
{
	if (len < 4)
		return NULL;
	if (data[0] != '-' || data[1] != '-' || data[2] != '-')
		return NULL;
	const char *p = data + 3;
	const char *end = data + len;
	while (p < end && (*p == '\r' || *p == '\n'))
		p++;
	while (p < end) {
		if ((size_t)(end - p) >= 3 && p[0] == '-' && p[1] == '-' && p[2] == '-') {
			if (p + 3 >= end || p[3] == '\r' || p[3] == '\n' || p[3] == '\0')
				return p;
		}
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl)
			break;
		p = nl + 1;
	}
	return NULL;
}

static int is_blank_line(const char *line)
{
	while (*line && *line != '\n') {
		if (!isspace((unsigned char)*line))
			return 0;
		line++;
	}
	return 1;
}

static int get_indent(const char *line)
{
	int indent = 0;
	while (*line == ' ' || *line == '\t') {
		indent++;
		line++;
	}
	return indent;
}

static int copy_scalar(const char *src, char *dst, size_t dst_size)
{
	if (utf8valid(src))
		return MORPH_ERR_FORMAT;
	if (strlen(src) >= dst_size || utf8len(src) > (dst_size - 1) / 4)
		return -E2BIG;
	memcpy(dst, src, strlen(src) + 1);
	return 0;
}

static int strip_quotes(const char *src, char *dst, size_t dst_size)
{
	size_t slen = strlen(src);
	if (slen >= 2 && ((src[0] == '"' && src[slen - 1] == '"') ||
			  (src[0] == '\'' && src[slen - 1] == '\''))) {
		src++;
		slen -= 2;
	}
	char *value MORPH_CLEANUP(morph_charp_cleanup) = strndup(src, slen);
	return value ? copy_scalar(value, dst, dst_size) : -ENOMEM;
}

static int parse_yaml_scalar(const char *key, const char *value,
			      struct skill_frontmatter *fm)
{
	if (strcmp(key, "name") == 0) {
		return copy_scalar(value, fm->name, sizeof(fm->name));
	} else if (strcmp(key, "description") == 0) {
		return copy_scalar(value, fm->description, sizeof(fm->description));
	} else if (strcmp(key, "license") == 0) {
		return copy_scalar(value, fm->license, sizeof(fm->license));
	} else if (strcmp(key, "compatibility") == 0) {
		return copy_scalar(value, fm->compatibility, sizeof(fm->compatibility));
	} else if (strcmp(key, "allowed-tools") == 0) {
		return copy_scalar(value, fm->allowed_tools, sizeof(fm->allowed_tools));
	}
	return 0;
}

static int add_metadata(struct skill_frontmatter *fm,
			 const char *key, const char *value)
{
	if (fm->metadata_count >= SKILL_METADATA_MAX)
		return -E2BIG;
	int rc = copy_scalar(key, fm->metadata[fm->metadata_count].key,
			     sizeof(fm->metadata[fm->metadata_count].key));
	if (rc == 0)
		rc = strip_quotes(value, fm->metadata[fm->metadata_count].value,
				  sizeof(fm->metadata[fm->metadata_count].value));
	if (rc != 0)
		return rc;
	fm->metadata_count++;
	return 0;
}

static int parse_metadata_line(const char *content,
				struct skill_frontmatter *fm)
{
	char mkey[SKILL_METADATA_KEY_MAX] = {0};
	const char *colon = strchr(content, ':');
	if (!colon || colon == content)
		return MORPH_ERR_PARSE;
	size_t klen = (size_t)(colon - content);
	if (klen >= sizeof(mkey))
		return -E2BIG;
	memcpy(mkey, content, klen);
	mkey[klen] = '\0';
	while (klen > 0 && isspace((unsigned char)mkey[klen - 1]))
		mkey[--klen] = '\0';
	const char *vstart = colon + 1;
	while (*vstart == ' ' || *vstart == '\t')
		vstart++;
	if (*vstart != '\0')
		return add_metadata(fm, mkey, vstart);
	return 0;
}

static int parse_yaml_block(const char *yaml, size_t yaml_len,
			     struct skill_frontmatter *fm)
{
	char *copy MORPH_CLEANUP(morph_charp_cleanup) = malloc(yaml_len + 1);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, yaml, yaml_len);
	copy[yaml_len] = '\0';

	char *next_line = NULL;
	char *current_key MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	morph_buf_t bv MORPH_CLEANUP(morph_buf_cleanup) = {0};
	if (morph_buf_init(&bv, 256) != 0) {
		return -ENOMEM;
	}
	int in_metadata = 0;
	int in_block_scalar = 0;
	int block_indent = 0;
	int rc = 0;

	for (char *line = copy; line && *line; line = next_line) {
		char *nl = strchr(line, '\n');
		next_line = nl ? nl + 1 : NULL;
		if (nl)
			*nl = '\0';

		if (in_block_scalar && current_key) {
			int indent = get_indent(line);
			if (line[0] != '\0' && !is_blank_line(line) && indent <= block_indent) {
				if (bv.len > 0) {
					while (bv.len > 0 && (bv.data[bv.len - 1] == '\n' || bv.data[bv.len - 1] == '\r'))
						bv.len--;
					bv.data[bv.len] = '\0';
					if (strcmp(current_key, "metadata") != 0) {
						rc = parse_yaml_scalar(current_key, bv.data, fm);
						if (rc != 0)
							return rc;
					}
				}
				morph_charp_cleanup(&current_key);
				morph_buf_reset(&bv);
				in_block_scalar = 0;
				in_metadata = 0;
			} else {
				const char *content = line;
				while (*content == ' ' || *content == '\t')
					content++;
				if (*content == '\0') {
					if (morph_buf_putc(&bv, '\n') != 0)
						return -ENOMEM;
					continue;
				}
				if (in_metadata) {
					rc = parse_metadata_line(content, fm);
					if (rc != 0)
						return rc;
					continue;
				}
				if (bv.len > 0 && morph_buf_putc(&bv, '\n') != 0)
					return -ENOMEM;
				if (morph_buf_puts(&bv, content) != 0)
					return -ENOMEM;
				continue;
			}
		}

		if (is_blank_line(line))
			continue;

		int indent = get_indent(line);
		const char *content = line + indent;

		if (in_metadata && indent > 0) {
			rc = parse_metadata_line(content, fm);
			if (rc != 0)
				return rc;
			continue;
		}

		const char *colon = strchr(content, ':');
		if (!colon)
			continue;

		size_t klen = (size_t)(colon - content);
		char *key MORPH_CLEANUP(morph_charp_cleanup) = malloc(klen + 1);
		if (!key)
			return -ENOMEM;
		memcpy(key, content, klen);
		key[klen] = '\0';
		while (klen > 0 && isspace((unsigned char)key[klen - 1]))
			key[--klen] = '\0';

		const char *vstart = colon + 1;
		while (*vstart == ' ' || *vstart == '\t')
			vstart++;

		if (strcmp(key, "metadata") == 0 && *vstart == '\0') {
			in_metadata = 1;
			continue;
		}

		if (*vstart == '\0' || *vstart == '|' || *vstart == '>') {
			if (current_key)
				morph_charp_cleanup(&current_key);
			current_key = morph_charp_steal(&key);
			in_block_scalar = 1;
			block_indent = indent;
			morph_buf_reset(&bv);
			in_metadata = (strcmp(current_key, "metadata") == 0);
			continue;
		}

		char *value = (char *)vstart;
		size_t length = strlen(value);
		if (length > 1 && (value[0] == '"' || value[0] == '\'') &&
		    value[length - 1] == value[0]) {
			value[length - 1] = '\0';
			value++;
		}
		rc = parse_yaml_scalar(key, value, fm);
		if (rc != 0)
			return rc;
	}

	if (in_block_scalar && current_key && bv.len > 0) {
		while (bv.len > 0 && (bv.data[bv.len - 1] == '\n' || bv.data[bv.len - 1] == '\r'))
			bv.len--;
		bv.data[bv.len] = '\0';
		if (strcmp(current_key, "metadata") != 0)
			return parse_yaml_scalar(current_key, bv.data, fm);
	}
	return 0;
}

int skill_parse(struct morph_log *logger, const char *data, size_t len,
		struct skill_frontmatter *fm, char **body_out)
{
	if (!data || !fm)
		return -EINVAL;
	if (body_out && *body_out)
		return -EEXIST;
	memset(fm, 0, sizeof(*fm));
	const char *end = data + len;

	const char *closing = find_closing_delim(data, len);
	if (!closing) {
		log_warn(logger, "skill_parse: no YAML frontmatter delimiters found");
		return MORPH_ERR_PARSE;
	}

	const char *yaml_start = data + 3;
	while (yaml_start < closing && (*yaml_start == '\r' || *yaml_start == '\n'))
		yaml_start++;

	size_t yaml_len = (size_t)(closing - yaml_start);
	if (yaml_len > SKILL_FRONTMATTER_MAX_BYTES)
		return -E2BIG;
	int rc = parse_yaml_block(yaml_start, yaml_len, fm);
	if (rc != 0) {
		log_warn(logger, "skill_parse: invalid metadata: %s", morph_strerror(rc));
		return rc;
	}

	const char *body_start = closing + 3;
	while (body_start < end && (*body_start == '\r' || *body_start == '\n'))
		body_start++;

	if (body_out && body_start < end) {
		*body_out = strndup(body_start, (size_t)(end - body_start));
		if (!*body_out)
			return -ENOMEM;
	}

	return 0;
}

int skill_parse_file(struct morph_log *logger, const char *path, struct skill_frontmatter *fm,
		     char **body_out)
{
	if (!path || !fm)
		return -EINVAL;

	size_t len = 0;
	char *data = file_read_all_copy(path, &len);
	if (!data) {
		log_err(logger, "skill_parse_file: failed to read %s", path);
		return -ENOENT;
	}

	int rc = skill_parse(logger, data, len, fm, body_out);
	free(data);
	return rc;
}

int skill_parse_frontmatter(struct morph_log *logger, const char *path,
			    struct skill_frontmatter *fm)
{
	FILE *file MORPH_CLEANUP(skill_file_cleanup) = NULL;
	char *line MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	morph_buf_t yaml MORPH_CLEANUP(morph_buf_cleanup) = {0};
	size_t capacity = 0;
	ssize_t length;
	int rc;

	if (!path || !fm)
		return -EINVAL;
	memset(fm, 0, sizeof(*fm));
	file = fopen(path, "rb");
	if (!file)
		return -errno;
	length = getline(&line, &capacity, file);
	if (length < 0)
		return ferror(file) ? -EIO : MORPH_ERR_PARSE;
	while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n'))
		line[--length] = '\0';
	if (length != 3 || memcmp(line, "---", 3) != 0)
		return MORPH_ERR_PARSE;
	rc = morph_buf_init(&yaml, 4096);
	if (rc != 0)
		return rc;
	while ((length = getline(&line, &capacity, file)) >= 0) {
		while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n'))
			line[--length] = '\0';
		if (length == 3 && memcmp(line, "---", 3) == 0) {
			rc = parse_yaml_block(morph_buf_cstr_borrow(&yaml), yaml.len, fm);
			if (rc != 0)
				log_warn(logger, "skill_parse: invalid metadata in %s: %s", path,
					 morph_strerror(rc));
			return rc;
		}
		if ((size_t)length >= SKILL_FRONTMATTER_MAX_BYTES - yaml.len)
			return -E2BIG;
		rc = morph_buf_append(&yaml, line, (size_t)length);
		if (rc == 0)
			rc = morph_buf_putc(&yaml, '\n');
		if (rc != 0)
			return rc;
	}
	return ferror(file) ? -EIO : MORPH_ERR_PARSE;
}
