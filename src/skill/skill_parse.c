#include "skill_parse.h"
#include "nb/log.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/utf8.h"
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
	if (nb_utf8_validate(nb_str_from_cstr(src), NULL))
		return NB_ERR_FORMAT;
	if (strlen(src) >= dst_size || nb_utf8_count(nb_str_from_cstr(src)) > (dst_size - 1) / 4)
		return NB_ERR_TOO_BIG;
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
	char *value NB_CLEANUP(nb_charp_cleanup) = strndup(src, slen);
	return value ? copy_scalar(value, dst, dst_size) : NB_ERR_NOMEM;
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
		return NB_ERR_TOO_BIG;
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
		return NB_ERR_PARSE;
	size_t klen = (size_t)(colon - content);
	if (klen >= sizeof(mkey))
		return NB_ERR_TOO_BIG;
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
	char *copy NB_CLEANUP(nb_charp_cleanup) = malloc(yaml_len + 1);
	if (!copy)
		return NB_ERR_NOMEM;
	memcpy(copy, yaml, yaml_len);
	copy[yaml_len] = '\0';

	char *next_line = NULL;
	char *current_key NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf bv NB_CLEANUP(nb_buf_cleanup) = {0};
	if ((nb_buf_init(&bv), nb_buf_reserve(&bv, 256) ? 0 : NB_ERR_NOMEM) != 0) {
		return NB_ERR_NOMEM;
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
					while (bv.len > 0 && (bv.ptr[bv.len - 1] == '\n' ||
							      bv.ptr[bv.len - 1] == '\r'))
						bv.len--;
					bv.ptr[bv.len] = '\0';
					if (strcmp(current_key, "metadata") != 0) {
						rc = parse_yaml_scalar(current_key, bv.ptr, fm);
						if (rc != 0)
							return rc;
					}
				}
				nb_charp_cleanup(&current_key);
				nb_buf_clear(&bv);
				in_block_scalar = 0;
				in_metadata = 0;
			} else {
				const char *content = line;
				while (*content == ' ' || *content == '\t')
					content++;
				if (*content == '\0') {
					if ((nb_buf_append_char(&bv, '\n') ? 0 : NB_ERR_NOMEM) != 0)
						return NB_ERR_NOMEM;
					continue;
				}
				if (in_metadata) {
					rc = parse_metadata_line(content, fm);
					if (rc != 0)
						return rc;
					continue;
				}
				if (bv.len > 0 &&
				    (nb_buf_append_char(&bv, '\n') ? 0 : NB_ERR_NOMEM) != 0)
					return NB_ERR_NOMEM;
				if ((nb_buf_append_cstr(&bv, content) ? 0 : NB_ERR_NOMEM) != 0)
					return NB_ERR_NOMEM;
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
		char *key NB_CLEANUP(nb_charp_cleanup) = malloc(klen + 1);
		if (!key)
			return NB_ERR_NOMEM;
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
				nb_charp_cleanup(&current_key);
			current_key = nb_charp_steal(&key);
			in_block_scalar = 1;
			block_indent = indent;
			nb_buf_clear(&bv);
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
		while (bv.len > 0 && (bv.ptr[bv.len - 1] == '\n' || bv.ptr[bv.len - 1] == '\r'))
			bv.len--;
		bv.ptr[bv.len] = '\0';
		if (strcmp(current_key, "metadata") != 0)
			return parse_yaml_scalar(current_key, bv.ptr, fm);
	}
	return 0;
}

int skill_parse(struct nb_log *logger, const char *data, size_t len, struct skill_frontmatter *fm,
		char **body_out)
{
	if (!data || !fm)
		return NB_ERR_INVAL;
	if (body_out && *body_out)
		return NB_ERR_EXISTS;
	memset(fm, 0, sizeof(*fm));
	const char *end = data + len;

	const char *closing = find_closing_delim(data, len);
	if (!closing) {
		nb_log_write(logger, NB_LOG_WARN,
			     "skill_parse: no YAML frontmatter delimiters found");
		return NB_ERR_PARSE;
	}

	const char *yaml_start = data + 3;
	while (yaml_start < closing && (*yaml_start == '\r' || *yaml_start == '\n'))
		yaml_start++;

	size_t yaml_len = (size_t)(closing - yaml_start);
	if (yaml_len > SKILL_FRONTMATTER_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	int rc = parse_yaml_block(yaml_start, yaml_len, fm);
	if (rc != 0) {
		nb_log_write(logger, NB_LOG_WARN, "skill_parse: invalid metadata: %s",
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
		return rc;
	}

	const char *body_start = closing + 3;
	while (body_start < end && (*body_start == '\r' || *body_start == '\n'))
		body_start++;

	if (body_out && body_start < end) {
		*body_out = strndup(body_start, (size_t)(end - body_start));
		if (!*body_out)
			return NB_ERR_NOMEM;
	}

	return 0;
}

int skill_parse_file(struct nb_log *logger, const char *path, struct skill_frontmatter *fm,
		     char **body_out)
{
	nb_buf content NB_CLEANUP(nb_buf_destroy) = {0};

	if (!path || !fm)
		return NB_ERR_INVAL;
	int rc = nb_file_read_all(path, &content, 0, NULL);
	if (rc != NB_OK) {
		nb_log_write(logger, NB_LOG_ERROR, "skill_parse_file: failed to read %s", path);
		return rc;
	}
	return skill_parse(logger, nb_buf_cstr(&content), nb_buf_len(&content), fm, body_out);
}

int skill_parse_frontmatter(struct nb_log *logger, const char *path, struct skill_frontmatter *fm)
{
	FILE *file NB_CLEANUP(skill_file_cleanup) = NULL;
	char *line NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf yaml NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t capacity = 0;
	ssize_t length;
	int rc;

	if (!path || !fm)
		return NB_ERR_INVAL;
	memset(fm, 0, sizeof(*fm));
	file = fopen(path, "rb");
	if (!file)
		return nb_status_from_errno(errno, NULL);
	length = getline(&line, &capacity, file);
	if (length < 0)
		return ferror(file) ? NB_ERR_IO : NB_ERR_PARSE;
	while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n'))
		line[--length] = '\0';
	if (length != 3 || memcmp(line, "---", 3) != 0)
		return NB_ERR_PARSE;
	rc = (nb_buf_init(&yaml), nb_buf_reserve(&yaml, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	while ((length = getline(&line, &capacity, file)) >= 0) {
		while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n'))
			line[--length] = '\0';
		if (length == 3 && memcmp(line, "---", 3) == 0) {
			rc = parse_yaml_block(nb_buf_cstr(&yaml), yaml.len, fm);
			if (rc != 0)
				nb_log_write(logger, NB_LOG_WARN,
					     "skill_parse: invalid metadata in %s: %s", path,
					     nb_err_str(morph_error_descriptions(),
							MORPH_ERROR_COUNT, rc));
			return rc;
		}
		if ((size_t)length >= SKILL_FRONTMATTER_MAX_BYTES - yaml.len)
			return NB_ERR_TOO_BIG;
		rc = (nb_buf_append(&yaml, line, (size_t)length) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_char(&yaml, '\n') ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	return ferror(file) ? NB_ERR_IO : NB_ERR_PARSE;
}
