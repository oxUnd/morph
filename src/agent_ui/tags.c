#include "agent_ui.h"

#include "util/buf.h"
#include "util/strmap.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_UI_MAX_TAGS 100
#define AGENT_UI_MAX_DEPTH 8
#define AGENT_UI_ATTR_VALUE_MAX 2048

struct agent_ui_parser {
	const char *input;
	size_t len;
	size_t pos;
	int recognized;
	cJSON *warnings;
};

static int is_name_char(char c)
{
	unsigned char uc = (unsigned char)c;

	return isalnum(uc) || c == '-' || c == '_' || c == ':';
}

static int is_attr_end(char c)
{
	return c == '\0' || isspace((unsigned char)c) || c == '>';
}

static int starts_with(const struct agent_ui_parser *p, const char *s)
{
	size_t n;

	if (!p || !s)
		return 0;
	n = strlen(s);
	return p->pos + n <= p->len &&
	       strncmp(p->input + p->pos, s, n) == 0;
}

static int supported_tag(const char *name)
{
	static const char *tags[] = {
		"speak", "vocab", "sentence", "button", "navigate",
		"highlight", "copy", NULL
	};

	if (!name)
		return 0;
	for (int i = 0; tags[i]; i++) {
		if (strcmp(name, tags[i]) == 0)
			return 1;
	}
	return 0;
}

static int tag_name_at(const struct agent_ui_parser *p, size_t pos,
		       size_t prefix_len, morph_buf_t *name)
{
	size_t name_start;
	size_t name_len = 0;
	int rc;

	if (!p || pos + prefix_len >= p->len || !name)
		return 0;
	name_start = pos + prefix_len;
	while (name_start + name_len < p->len &&
	       is_name_char(p->input[name_start + name_len]))
		name_len++;
	if (name_len == 0)
		return 0;
	rc = morph_buf_init(name, name_len + 1);
	if (rc < 0)
		return 0;
	if (morph_buf_append(name, p->input + name_start, name_len) < 0) {
		morph_buf_cleanup(name);
		return 0;
	}
	return 1;
}

static int opening_tag_prefix_len(const struct agent_ui_parser *p,
				  int *missing_colon)
{
	morph_buf_t name;
	int ok;

	if (missing_colon)
		*missing_colon = 0;
	if (starts_with(p, "<m:"))
		return 3;
	if (!p || p->pos + 2 >= p->len ||
	    p->input[p->pos] != '<' || p->input[p->pos + 1] != 'm' ||
	    p->input[p->pos + 2] == ':' ||
	    !is_name_char(p->input[p->pos + 2]))
		return 0;
	ok = tag_name_at(p, p->pos, 2, &name);
	if (!ok)
		return 0;
	ok = supported_tag(morph_buf_cstr(&name)) ||
	     strcmp(morph_buf_cstr(&name), "ask_user") == 0;
	morph_buf_cleanup(&name);
	if (!ok)
		return 0;
	if (missing_colon)
		*missing_colon = 1;
	return 2;
}

static int closing_tag_prefix_len(const struct agent_ui_parser *p,
				  const char *name, int *missing_colon)
{
	size_t start;
	size_t n;

	if (missing_colon)
		*missing_colon = 0;
	if (!p || !name)
		return 0;
	n = strlen(name);
	if (starts_with(p, "</m:")) {
		start = p->pos + 4;
		if (start + n + 1 <= p->len &&
		    strncmp(p->input + start, name, n) == 0 &&
		    p->input[start + n] == '>')
			return 4;
	}
	if (p->pos + 3 > p->len ||
	    p->input[p->pos] != '<' ||
	    p->input[p->pos + 1] != '/' ||
	    p->input[p->pos + 2] != 'm')
		return 0;
	start = p->pos + 3;
	if (start + n + 1 <= p->len &&
	    strncmp(p->input + start, name, n) == 0 &&
	    p->input[start + n] == '>') {
		if (missing_colon)
			*missing_colon = 1;
		return 3;
	}
	return 0;
}

static int missing_colon_closing_supported_at(const struct agent_ui_parser *p)
{
	morph_buf_t name;
	int ok;

	if (!p || p->pos + 3 >= p->len ||
	    p->input[p->pos] != '<' ||
	    p->input[p->pos + 1] != '/' ||
	    p->input[p->pos + 2] != 'm' ||
	    !is_name_char(p->input[p->pos + 3]))
		return 0;
	ok = tag_name_at(p, p->pos, 3, &name);
	if (!ok)
		return 0;
	ok = supported_tag(morph_buf_cstr(&name)) ||
	     strcmp(morph_buf_cstr(&name), "ask_user") == 0;
	morph_buf_cleanup(&name);
	return ok;
}

static int bodyless_tag(const char *name)
{
	return name &&
	       (strcmp(name, "button") == 0 ||
		strcmp(name, "navigate") == 0 ||
		strcmp(name, "copy") == 0);
}

static void add_warning(struct agent_ui_parser *p, const char *message)
{
	if (!p || !p->warnings || !message)
		return;
	cJSON_AddItemToArray(p->warnings, cJSON_CreateString(message));
}

static int decode_entity_at(const char *s, size_t len, size_t *used, char *out)
{
	struct entity {
		const char *name;
		char value;
	};
	static const struct entity entities[] = {
		{ "&quot;", '"' },
		{ "&apos;", '\'' },
		{ "&lt;", '<' },
		{ "&gt;", '>' },
		{ "&amp;", '&' },
		{ NULL, 0 }
	};

	for (int i = 0; entities[i].name; i++) {
		size_t n = strlen(entities[i].name);

		if (len >= n && strncmp(s, entities[i].name, n) == 0) {
			*used = n;
			*out = entities[i].value;
			return 1;
		}
	}
	return 0;
}

static int append_decoded(morph_buf_t *buf, const char *s, size_t len)
{
	size_t i = 0;

	while (i < len) {
		size_t used = 0;
		char decoded = 0;

		if (s[i] == '&' &&
		    decode_entity_at(s + i, len - i, &used, &decoded)) {
			int rc = morph_buf_putc(buf, decoded);

			if (rc < 0)
				return rc;
			i += used;
			continue;
		}
		if (morph_buf_putc(buf, s[i]) < 0)
			return -ENOMEM;
		i++;
	}
	return 0;
}

static int json_add_string_n(cJSON *obj, const char *name,
			     const char *s, size_t len)
{
	morph_buf_t buf;
	int rc;

	rc = morph_buf_init(&buf, len + 1);
	if (rc < 0)
		return rc;
	rc = append_decoded(&buf, s, len);
	if (rc == 0 && !cJSON_AddStringToObject(obj, name,
						morph_buf_cstr(&buf)))
		rc = -ENOMEM;
	morph_buf_cleanup(&buf);
	return rc;
}

static size_t find_tag_end(const struct agent_ui_parser *p, size_t from)
{
	size_t i = from;
	char quote = 0;

	while (i < p->len) {
		char c = p->input[i];

		if (quote) {
			if (c == quote)
				quote = 0;
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '>') {
			return i;
		}
		i++;
	}
	return p->len;
}

static void skip_spaces(struct agent_ui_parser *p)
{
	while (p->pos < p->len &&
	       isspace((unsigned char)p->input[p->pos])) {
		p->pos++;
	}
}

static int consume_utf8_quote(const struct agent_ui_parser *p, size_t *pos,
			      char *quote)
{
	unsigned char b0;
	unsigned char b1;
	unsigned char b2;

	if (*pos >= p->len)
		return 0;
	if (p->input[*pos] == '"' || p->input[*pos] == '\'') {
		*quote = p->input[*pos];
		(*pos)++;
		return 1;
	}
	if (*pos + 2 >= p->len)
		return 0;
	b0 = (unsigned char)p->input[*pos];
	b1 = (unsigned char)p->input[*pos + 1];
	b2 = (unsigned char)p->input[*pos + 2];
	if (b0 != 0xe2 || b1 != 0x80)
		return 0;
	if (b2 == 0x9c || b2 == 0x9d) {
		*quote = '"';
		*pos += 3;
		return 1;
	}
	if (b2 == 0x98 || b2 == 0x99) {
		*quote = '\'';
		*pos += 3;
		return 1;
	}
	return 0;
}

static int at_matching_utf8_quote(const struct agent_ui_parser *p,
				  size_t pos, char quote, size_t *width)
{
	if (pos >= p->len)
		return 0;
	if (p->input[pos] == quote) {
		*width = 1;
		return 1;
	}
	if (pos + 2 >= p->len)
		return 0;
	if ((unsigned char)p->input[pos] != 0xe2 ||
	    (unsigned char)p->input[pos + 1] != 0x80)
		return 0;
	if (quote == '"' &&
	    (unsigned char)p->input[pos + 2] == 0x9d) {
		*width = 3;
		return 1;
	}
	if (quote == '\'' &&
	    (unsigned char)p->input[pos + 2] == 0x99) {
		*width = 3;
		return 1;
	}
	return 0;
}

static int parse_attrs(struct agent_ui_parser *p, size_t end, cJSON *attrs)
{
	while (p->pos < end) {
		size_t key_start;
		size_t key_len;
		size_t val_start;
		size_t val_end;
		char quote = 0;

		skip_spaces(p);
		if (p->pos >= end || p->input[p->pos] == '/')
			break;
		key_start = p->pos;
		while (p->pos < end && is_name_char(p->input[p->pos]))
			p->pos++;
		key_len = p->pos - key_start;
		if (key_len == 0) {
			p->pos++;
			continue;
		}
		skip_spaces(p);
		if (p->pos >= end || p->input[p->pos] != '=') {
			add_warning(p, "attribute without value ignored");
			continue;
		}
		p->pos++;
		skip_spaces(p);
		if (consume_utf8_quote(p, &p->pos, &quote)) {
			val_start = p->pos;
			while (p->pos < end) {
				size_t width = 0;

				if (at_matching_utf8_quote(p, p->pos, quote,
							   &width)) {
					val_end = p->pos;
					p->pos += width;
					goto add_attr;
				}
				p->pos++;
			}
			val_end = p->pos;
			add_warning(p, "unterminated quoted attribute");
		} else {
			val_start = p->pos;
			while (p->pos < end && !is_attr_end(p->input[p->pos]))
				p->pos++;
			val_end = p->pos;
			add_warning(p, "unquoted attribute value accepted");
		}

add_attr:
		{
			morph_buf_t key;
			morph_buf_t value;
			int rc;
			size_t value_len = val_end - val_start;

			if (value_len > AGENT_UI_ATTR_VALUE_MAX)
				value_len = AGENT_UI_ATTR_VALUE_MAX;
			rc = morph_buf_init(&key, key_len + 1);
			if (rc < 0)
				return rc;
			rc = morph_buf_init(&value, value_len + 1);
			if (rc < 0) {
				morph_buf_cleanup(&key);
				return rc;
			}
			if (morph_buf_append(&key, p->input + key_start,
					     key_len) < 0 ||
			    append_decoded(&value, p->input + val_start,
					   value_len) < 0 ||
			    !cJSON_AddStringToObject(attrs,
						     morph_buf_cstr(&key),
						     morph_buf_cstr(&value))) {
				morph_buf_cleanup(&value);
				morph_buf_cleanup(&key);
				return -ENOMEM;
			}
			morph_buf_cleanup(&value);
			morph_buf_cleanup(&key);
		}
	}
	return 0;
}

static int append_text_node(cJSON *nodes, const char *s, size_t len,
			    morph_buf_t *plain)
{
	cJSON *node;
	int rc = 0;

	if (len == 0)
		return 0;
	node = cJSON_CreateObject();
	if (!node)
		return -ENOMEM;
	if (!cJSON_AddStringToObject(node, "type", "text") ||
	    json_add_string_n(node, "text", s, len) < 0) {
		cJSON_Delete(node);
		return -ENOMEM;
	}
	cJSON_AddItemToArray(nodes, node);
	if (plain)
		rc = append_decoded(plain, s, len);
	return rc;
}

static int consume_closing(struct agent_ui_parser *p, const char *name)
{
	int missing_colon = 0;
	int prefix_len;

	prefix_len = closing_tag_prefix_len(p, name, &missing_colon);
	if (prefix_len == 0)
		return 0;
	if (missing_colon)
		add_warning(p, "missing colon in closing m tag accepted");
	p->pos += (size_t)prefix_len + strlen(name) + 1;
	return 1;
}

static int parse_nodes(struct agent_ui_parser *p, const char *stop_name,
		       int depth, cJSON *nodes, morph_buf_t *plain);

static int parse_element(struct agent_ui_parser *p, int depth, cJSON *nodes,
			 morph_buf_t *plain)
{
	size_t start = p->pos;
	size_t name_start;
	size_t name_len;
	size_t tag_end;
	int prefix_len;
	int missing_colon = 0;
	int self_closing;
	morph_buf_t name_buf;
	cJSON *node = NULL;
	cJSON *attrs = NULL;
	cJSON *children = NULL;
	morph_buf_t body;
	int body_ready = 0;
	int rc = 0;

	prefix_len = opening_tag_prefix_len(p, &missing_colon);
	if (prefix_len == 0)
		return append_text_node(nodes, p->input + start, 1, plain);
	if (p->recognized >= AGENT_UI_MAX_TAGS || depth >= AGENT_UI_MAX_DEPTH)
		return append_text_node(nodes, p->input + start,
					(size_t)prefix_len, plain);
	p->pos += (size_t)prefix_len;
	name_start = p->pos;
	while (p->pos < p->len && is_name_char(p->input[p->pos]))
		p->pos++;
	name_len = p->pos - name_start;
	if (name_len == 0) {
		p->pos = start;
		return append_text_node(nodes, p->input + p->pos, 1, plain);
	}
	rc = morph_buf_init(&name_buf, name_len + 1);
	if (rc < 0)
		return rc;
	if (morph_buf_append(&name_buf, p->input + name_start, name_len) < 0) {
		morph_buf_cleanup(&name_buf);
		return -ENOMEM;
	}
	if (!supported_tag(morph_buf_cstr(&name_buf))) {
		if (strcmp(morph_buf_cstr(&name_buf), "ask_user") == 0)
			add_warning(p, "m:ask_user ignored; use ask_user tool");
		else
			add_warning(p, "unsupported m tag left as text");
		morph_buf_cleanup(&name_buf);
		p->pos = start;
		tag_end = find_tag_end(p, p->pos);
		if (tag_end >= p->len)
			tag_end = start + 3;
		else
			tag_end++;
		p->pos = tag_end;
		return append_text_node(nodes, p->input + start,
					tag_end - start, plain);
	}
	if (missing_colon)
		add_warning(p, "missing colon in opening m tag accepted");
	tag_end = find_tag_end(p, p->pos);
	if (tag_end >= p->len) {
		add_warning(p, "unterminated m tag left as text");
		morph_buf_cleanup(&name_buf);
		p->pos = p->len;
		return append_text_node(nodes, p->input + start,
					p->len - start, plain);
	}
	self_closing = tag_end > start && p->input[tag_end - 1] == '/';
	if (bodyless_tag(morph_buf_cstr(&name_buf)) && !self_closing) {
		self_closing = 1;
		add_warning(p, "bodyless tag treated as self closing");
	}
	node = cJSON_CreateObject();
	attrs = cJSON_CreateObject();
	children = cJSON_CreateArray();
	if (!node || !attrs || !children) {
		rc = -ENOMEM;
		goto out;
	}
	if (!cJSON_AddStringToObject(node, "type", "component") ||
	    !cJSON_AddStringToObject(node, "component",
				     morph_buf_cstr(&name_buf))) {
		rc = -ENOMEM;
		goto out;
	}
	rc = parse_attrs(p, self_closing ? tag_end - 1 : tag_end, attrs);
	if (rc < 0)
		goto out;
	p->pos = tag_end + 1;
	p->recognized++;
	rc = morph_buf_init(&body, 128);
	if (rc < 0)
		goto out;
	body_ready = 1;
	if (!self_closing) {
		rc = parse_nodes(p, morph_buf_cstr(&name_buf), depth + 1,
				 children, &body);
		if (rc < 0)
			goto out;
	}
	if (plain && morph_buf_append(plain, morph_buf_cstr(&body),
				      body.len) < 0) {
		rc = -ENOMEM;
		goto out;
	}
	if (!cJSON_AddItemToObject(node, "attrs", attrs)) {
		rc = -ENOMEM;
		goto out;
	}
	attrs = NULL;
	if (!cJSON_AddStringToObject(node, "text", morph_buf_cstr(&body))) {
		rc = -ENOMEM;
		goto out;
	}
	if (cJSON_GetArraySize(children) > 0) {
		if (!cJSON_AddItemToObject(node, "children", children)) {
			rc = -ENOMEM;
			goto out;
		}
		children = NULL;
	}
	cJSON_AddItemToArray(nodes, node);
	node = NULL;

out:
	if (body_ready)
		morph_buf_cleanup(&body);
	cJSON_Delete(children);
	cJSON_Delete(attrs);
	cJSON_Delete(node);
	morph_buf_cleanup(&name_buf);
	return rc;
}

static int parse_nodes(struct agent_ui_parser *p, const char *stop_name,
		       int depth, cJSON *nodes, morph_buf_t *plain)
{
	while (p->pos < p->len) {
		size_t text_start;
		size_t next_open;
		size_t next_close;
		size_t next;

		if (stop_name && consume_closing(p, stop_name))
			return 0;
		if (starts_with(p, "</m:") ||
		    missing_colon_closing_supported_at(p)) {
			size_t end = find_tag_end(p, p->pos);

			add_warning(p, "unmatched closing tag ignored");
			if (end >= p->len)
				p->pos = p->len;
			else
				p->pos = end + 1;
			continue;
		}
		if (opening_tag_prefix_len(p, NULL) > 0) {
			int rc = parse_element(p, depth, nodes, plain);

			if (rc < 0)
				return rc;
			continue;
		}
		text_start = p->pos;
		next_open = p->len;
		next_close = p->len;
		for (size_t i = p->pos + 1; i < p->len; i++) {
			struct agent_ui_parser probe = *p;

			probe.pos = i;
			if (opening_tag_prefix_len(&probe, NULL) > 0) {
				next_open = i;
				break;
			}
			if (i + 4 <= p->len &&
			    strncmp(p->input + i, "</m:", 4) == 0) {
				next_close = i;
				break;
			}
			if (i + 3 < p->len &&
			    p->input[i] == '<' &&
			    p->input[i + 1] == '/' &&
			    p->input[i + 2] == 'm') {
				struct agent_ui_parser close_probe = *p;

				close_probe.pos = i;
				if (!missing_colon_closing_supported_at(
					    &close_probe))
					continue;
				next_close = i;
				break;
			}
		}
		next = next_open < next_close ? next_open : next_close;
		p->pos = next;
		if (append_text_node(nodes, p->input + text_start,
				     next - text_start, plain) < 0)
			return -ENOMEM;
	}
	if (stop_name)
		add_warning(p, "missing closing tag accepted");
	return 0;
}

char *agent_ui_parse_tags_json(const char *input)
{
	struct agent_ui_parser parser;
	cJSON *root = NULL;
	cJSON *nodes = NULL;
	char *json = NULL;
	int rc;

	if (!input)
		input = "";
	memset(&parser, 0, sizeof(parser));
	parser.input = input;
	parser.len = strlen(input);
	root = cJSON_CreateObject();
	nodes = cJSON_CreateArray();
	parser.warnings = cJSON_CreateArray();
	if (!root || !nodes || !parser.warnings)
		goto out;
	if (!cJSON_AddStringToObject(root, "kind", "agent_ui") ||
	    !cJSON_AddNumberToObject(root, "version", 1))
		goto out;
	rc = parse_nodes(&parser, NULL, 0, nodes, NULL);
	if (rc < 0)
		goto out;
	if (!cJSON_AddNumberToObject(root, "recognized_tags",
				     parser.recognized))
		goto out;
	if (!cJSON_AddItemToObject(root, "nodes", nodes))
		goto out;
	nodes = NULL;
	if (!cJSON_AddItemToObject(root, "warnings", parser.warnings))
		goto out;
	parser.warnings = NULL;
	json = cJSON_PrintUnformatted(root);

out:
	cJSON_Delete(parser.warnings);
	cJSON_Delete(nodes);
	cJSON_Delete(root);
	return json;
}
