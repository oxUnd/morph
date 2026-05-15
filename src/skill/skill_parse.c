#include "skill_parse.h"
#include "util/log.h"
#include "util/file.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *find_closing_delim(const char *data, size_t len)
{
	if (len < 4)
		return NULL;
	if (data[0] != '-' || data[1] != '-' || data[2] != '-')
		return NULL;
	const char *p = data + 3;
	while (*p == '\r' || *p == '\n')
		p++;
	const char *end = data + len;
	while (p < end) {
		if (p[0] == '-' && p[1] == '-' && p[2] == '-') {
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

static void strip_trailing_newlines(char *s)
{
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		s[--len] = '\0';
}

static void strip_quotes(const char *src, char *dst, size_t dst_size)
{
	size_t slen = strlen(src);
	if (slen >= 2 && ((src[0] == '"' && src[slen - 1] == '"') ||
			  (src[0] == '\'' && src[slen - 1] == '\''))) {
		src++;
		slen -= 2;
	}
	if (slen >= dst_size)
		slen = dst_size - 1;
	memcpy(dst, src, slen);
	dst[slen] = '\0';
}

static void parse_yaml_scalar(const char *key, const char *value,
			      struct skill_frontmatter *fm)
{
	if (strcmp(key, "name") == 0) {
		strncpy(fm->name, value, sizeof(fm->name) - 1);
	} else if (strcmp(key, "description") == 0) {
		strncpy(fm->description, value, sizeof(fm->description) - 1);
	} else if (strcmp(key, "license") == 0) {
		strncpy(fm->license, value, sizeof(fm->license) - 1);
	} else if (strcmp(key, "compatibility") == 0) {
		strncpy(fm->compatibility, value, sizeof(fm->compatibility) - 1);
	} else if (strcmp(key, "allowed-tools") == 0) {
		strncpy(fm->allowed_tools, value, sizeof(fm->allowed_tools) - 1);
	}
}

static void add_metadata(struct skill_frontmatter *fm,
			 const char *key, const char *value)
{
	if (fm->metadata_count >= SKILL_METADATA_MAX)
		return;
	strncpy(fm->metadata[fm->metadata_count].key,
		key, SKILL_METADATA_KEY_MAX - 1);
	strip_quotes(value, fm->metadata[fm->metadata_count].value,
		     SKILL_METADATA_VAL_MAX);
	fm->metadata_count++;
}

static void parse_metadata_line(const char *content,
				struct skill_frontmatter *fm)
{
	char mkey[SKILL_METADATA_KEY_MAX] = {0};
	const char *colon = strchr(content, ':');
	if (!colon || colon == content)
		return;
	size_t klen = (size_t)(colon - content);
	if (klen >= sizeof(mkey))
		klen = sizeof(mkey) - 1;
	memcpy(mkey, content, klen);
	mkey[klen] = '\0';
	while (klen > 0 && isspace((unsigned char)mkey[klen - 1]))
		mkey[--klen] = '\0';
	const char *vstart = colon + 1;
	while (*vstart == ' ' || *vstart == '\t')
		vstart++;
	if (*vstart != '\0')
		add_metadata(fm, mkey, vstart);
}

static void parse_yaml_block(const char *yaml, size_t yaml_len,
			     struct skill_frontmatter *fm)
{
	char *copy = malloc(yaml_len + 1);
	if (!copy)
		return;
	memcpy(copy, yaml, yaml_len);
	copy[yaml_len] = '\0';

	char *line = copy;
	char *current_key = NULL;
	char *block_value = NULL;
	size_t block_cap = 0;
	int in_metadata = 0;
	int in_block_scalar = 0;
	int block_indent = 0;

	while (*line) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		if (in_block_scalar && current_key) {
			int indent = get_indent(line);
			if (line[0] != '\0' && !is_blank_line(line) && indent <= block_indent) {
				if (block_value) {
					strip_trailing_newlines(block_value);
					if (strcmp(current_key, "metadata") != 0)
						parse_yaml_scalar(current_key, block_value, fm);
				}
				free(current_key);
				current_key = NULL;
				free(block_value);
				block_value = NULL;
				block_cap = 0;
				in_block_scalar = 0;
				in_metadata = 0;
			} else {
				const char *content = line;
				while (*content == ' ' || *content == '\t')
					content++;
				if (*content == '\0') {
					size_t vlen = block_value ? strlen(block_value) : 0;
					if (vlen + 2 >= block_cap) {
						block_cap = (vlen + 2) * 2;
						char *nb = realloc(block_value, block_cap);
						if (!nb) goto next_line;
						block_value = nb;
					}
					block_value[vlen] = '\n';
					block_value[vlen + 1] = '\0';
					goto next_line;
				}
				if (in_metadata) {
					parse_metadata_line(content, fm);
					goto next_line;
				}
				size_t clen = strlen(content);
				size_t vlen = block_value ? strlen(block_value) : 0;
				if (vlen + clen + 2 >= block_cap) {
					block_cap = (vlen + clen + 2) * 2;
					char *nb = realloc(block_value, block_cap);
					if (!nb) goto next_line;
					block_value = nb;
				}
				if (vlen > 0) {
					block_value[vlen] = '\n';
					vlen++;
				}
				memcpy(block_value + vlen, content, clen + 1);
				goto next_line;
			}
		}

		if (is_blank_line(line))
			goto next_line;

		int indent = get_indent(line);
		const char *content = line + indent;

		if (in_metadata && indent > 0) {
			parse_metadata_line(content, fm);
			goto next_line;
		}

		const char *colon = strchr(content, ':');
		if (!colon)
			goto next_line;

		size_t klen = (size_t)(colon - content);
		char *key = malloc(klen + 1);
		if (!key)
			goto next_line;
		memcpy(key, content, klen);
		key[klen] = '\0';
		while (klen > 0 && isspace((unsigned char)key[klen - 1]))
			key[--klen] = '\0';

		const char *vstart = colon + 1;
		while (*vstart == ' ' || *vstart == '\t')
			vstart++;

		if (strcmp(key, "metadata") == 0 && *vstart == '\0') {
			in_metadata = 1;
			free(key);
			goto next_line;
		}

		if (*vstart == '\0') {
			if (current_key)
				free(current_key);
			current_key = key;
			in_block_scalar = 1;
			block_indent = indent;
			if (block_value) {
				free(block_value);
				block_value = NULL;
				block_cap = 0;
			}
			in_metadata = (strcmp(key, "metadata") == 0);
			goto next_line;
		}

		if (*vstart == '|' || *vstart == '>') {
			if (current_key)
				free(current_key);
			current_key = key;
			in_block_scalar = 1;
			block_indent = indent;
			if (block_value) {
				free(block_value);
				block_value = NULL;
				block_cap = 0;
			}
			in_metadata = (strcmp(key, "metadata") == 0);
			goto next_line;
		}

		if (vstart[0] == '"') {
			vstart++;
			size_t vlen = strlen(vstart);
			if (vlen > 0 && vstart[vlen - 1] == '"')
				vlen--;
			char *val = malloc(vlen + 1);
			if (val) {
				memcpy(val, vstart, vlen);
				val[vlen] = '\0';
				parse_yaml_scalar(key, val, fm);
				free(val);
			}
		} else if (vstart[0] == '\'') {
			vstart++;
			size_t vlen = strlen(vstart);
			if (vlen > 0 && vstart[vlen - 1] == '\'')
				vlen--;
			char *val = malloc(vlen + 1);
			if (val) {
				memcpy(val, vstart, vlen);
				val[vlen] = '\0';
				parse_yaml_scalar(key, val, fm);
				free(val);
			}
		} else {
			parse_yaml_scalar(key, vstart, fm);
		}

		free(key);

next_line:
		if (nl)
			line = nl + 1;
		else
			break;
	}

	if (in_block_scalar && current_key && block_value) {
		strip_trailing_newlines(block_value);
		if (strcmp(current_key, "metadata") != 0)
			parse_yaml_scalar(current_key, block_value, fm);
		free(current_key);
		free(block_value);
	} else {
		free(current_key);
		free(block_value);
	}

	free(copy);
}

int skill_parse(const char *data, size_t len, struct skill_frontmatter *fm,
		char **body_out)
{
	if (!data || !fm)
		return -EINVAL;
	memset(fm, 0, sizeof(*fm));
	if (body_out)
		*body_out = NULL;

	const char *closing = find_closing_delim(data, len);
	if (!closing) {
		log_warn("skill_parse: no YAML frontmatter delimiters found");
		return -EIO;
	}

	const char *yaml_start = data + 3;
	while (*yaml_start == '\r' || *yaml_start == '\n')
		yaml_start++;

	size_t yaml_len = (size_t)(closing - yaml_start);
	parse_yaml_block(yaml_start, yaml_len, fm);

	const char *body_start = closing + 3;
	while (*body_start == '\r' || *body_start == '\n')
		body_start++;

	if (body_out && *body_start) {
		*body_out = strdup(body_start);
		if (!*body_out)
			return -ENOMEM;
	}

	return 0;
}

int skill_parse_file(const char *path, struct skill_frontmatter *fm,
		     char **body_out)
{
	if (!path || !fm)
		return -EINVAL;

	size_t len = 0;
	char *data = file_read_all(path, &len);
	if (!data) {
		log_err("skill_parse_file: failed to read %s", path);
		return -ENOENT;
	}

	int rc = skill_parse(data, len, fm, body_out);
	free(data);
	return rc;
}
