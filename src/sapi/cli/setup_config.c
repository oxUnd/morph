#include "setup_config.h"
#include "setup_role.h"
#include "util/array.h"
#include "util/buf.h"
#include "util/error.h"
#include "cJSON.h"
#include "tomlc17.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct config_edit {
	size_t start;
	size_t end;
	int inline_members;
	int section;
	morph_buf_t text;
};

struct config_edits {
	const char *source;
	const char *patch;
	const char *eol;
	morph_array_t items;
};

static const toml_datum_t *member(const toml_datum_t *table, const char *key)
{
	for (int i = 0; table && table->type == TOML_TABLE && i < table->u.tab.size; i++)
		if (strcmp(table->u.tab.key[i], key) == 0)
			return &table->u.tab.value[i];
	return NULL;
}

static int equivalent(const toml_datum_t *a, const toml_datum_t *b)
{
	if (!a || !b) return a == b;
	toml_result_t left = {.ok = true, .toptab = *a};
	toml_result_t right = {.ok = true, .toptab = *b};
	return toml_equiv(&left, &right);
}

static size_t source_offset(const char *source, const toml_datum_t *node)
{
	const char *line = source;
	for (int i = 1; i < node->lineno && *line; i++) {
		const char *next = strchr(line, '\n');
		line = next ? next + 1 : line + strlen(line);
	}
	return (size_t)(line - source) + (node->colno > 0 ? (size_t)node->colno - 1 : 0);
}

/* tomlc17 locates strings at their contents, after the opening quotes and
 * optional first newline. Other values are located at their opening token. */
static size_t value_offset(const char *source, const toml_datum_t *node)
{
	size_t at = source_offset(source, node);
	if (node->type != TOML_STRING) return at;
	size_t quotes = at;
	if (quotes && source[quotes - 1] == '\n') {
		quotes--;
		if (quotes && source[quotes - 1] == '\r') quotes--;
	}
	if (quotes >= 3 && (source[quotes - 1] == '"' || source[quotes - 1] == '\'') &&
	    source[quotes - 2] == source[quotes - 1] && source[quotes - 3] == source[quotes - 1])
		return quotes - 3;
	return at && (source[at - 1] == '"' || source[at - 1] == '\'') ? at - 1 : at;
}

/* The TOML parser validates syntax. This scanner only locates token ends for
 * source edits, preserving comments, quoted keys and unrelated values verbatim. */
static size_t string_end(const char *text, size_t start)
{
	char quote = text[start];
	int multi = text[start + 1] == quote && text[start + 2] == quote;
	size_t at = start + (multi ? 3u : 1u);
	while (text[at]) {
		if (quote == '"' && text[at] == '\\' && text[at + 1]) {
			at += 2;
			continue;
		}
		if (text[at] == quote) {
			size_t end = at;
			while (text[end] == quote) end++;
			if (!multi) return at + 1;
			if (end - at >= 3) return end;
			at = end;
		} else at++;
	}
	return at;
}

static size_t value_end(const char *text, const toml_datum_t *value)
{
	size_t start = value_offset(text, value);
	if (text[start] == '"' || text[start] == '\'') return string_end(text, start);
	if (text[start] == '[' || text[start] == '{') {
		unsigned depth = 0;
		for (size_t at = start; text[at]; at++) {
			if (text[at] == '"' || text[at] == '\'') {
				at = string_end(text, at) - 1;
			} else if (text[at] == '#') {
				while (text[at] && text[at] != '\n') at++;
				if (!text[at]) return at;
			} else if (text[at] == '[' || text[at] == '{') depth++;
			else if ((text[at] == ']' || text[at] == '}') && --depth == 0) return at + 1;
		}
		return strlen(text);
	}
	size_t end = start;
	while (text[end] && !strchr(" \t\r\n#,]}", text[end])) end++;
	if ((value->type == TOML_DATETIME || value->type == TOML_DATETIMETZ) &&
	    end - start == 10 && text[end] == ' ') {
		end++;
		while (text[end] && !strchr(" \t\r\n#,]}", text[end])) end++;
	}
	return end;
}

static int key_text(morph_buf_t *out, const char *key)
{
	int bare = *key != '\0';
	for (const char *p = key; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) bare = 0;
	if (bare) return morph_buf_puts(out, key);
	cJSON *string = cJSON_CreateString(key);
	char *encoded = string ? cJSON_PrintUnformatted(string) : NULL;
	cJSON_Delete(string);
	if (!encoded) return -ENOMEM;
	int rc = morph_buf_puts(out, encoded);
	free(encoded);
	return rc;
}

static int child_path(morph_buf_t *out, const char *parent, const char *key)
{
	int rc = morph_buf_init(out, 128);
	if (rc == 0 && *parent) rc = morph_buf_printf(out, "%s.", parent);
	if (rc == 0) rc = key_text(out, key);
	return rc;
}

static int patch_value(struct config_edits *edits, const toml_datum_t *value,
		morph_buf_t *out, unsigned depth)
{
	if (depth > 64) return -ELOOP;
	if (value->type != TOML_TABLE) {
		size_t start = value_offset(edits->patch, value);
		return morph_buf_append(out, edits->patch + start, value_end(edits->patch, value) - start);
	}
	int rc = morph_buf_putc(out, '{');
	for (int i = 0; rc == 0 && i < value->u.tab.size; i++) {
		if (i) rc = morph_buf_puts(out, ", ");
		if (rc == 0) rc = key_text(out, value->u.tab.key[i]);
		if (rc == 0) rc = morph_buf_puts(out, " = ");
		if (rc == 0) rc = patch_value(edits, &value->u.tab.value[i], out, depth + 1);
	}
	if (rc == 0) rc = morph_buf_putc(out, '}');
	return rc;
}

static struct config_edit *edit_at(struct config_edits *edits, size_t start, size_t end,
		int section)
{
	for (size_t i = 0; start == end && i < edits->items.nelts; i++) {
		struct config_edit *edit = morph_array_get(&edits->items, i);
		if (edit->start == start && edit->end == end && edit->section == section) return edit;
	}
	struct config_edit *edit = morph_array_push(&edits->items);
	if (!edit) return NULL;
	*edit = (struct config_edit){.start = start, .end = end, .section = section};
	if (morph_buf_init(&edit->text, 128) != 0) return NULL;
	return edit;
}

static int new_table(struct config_edits *edits, const toml_datum_t *table,
		const char *path, unsigned depth)
{
	if (depth > 64) return -ELOOP;
	struct config_edit *edit = edit_at(edits, strlen(edits->source), strlen(edits->source), 1);
	if (!edit) return -ENOMEM;
	int rc = morph_buf_printf(&edit->text, "%s[%s]%s", edits->eol, path, edits->eol);
	for (int i = 0; rc == 0 && i < table->u.tab.size; i++) {
		if (table->u.tab.value[i].type == TOML_TABLE) continue;
		rc = key_text(&edit->text, table->u.tab.key[i]);
		if (rc == 0) rc = morph_buf_puts(&edit->text, " = ");
		if (rc == 0) rc = patch_value(edits, &table->u.tab.value[i], &edit->text, depth);
		if (rc == 0) rc = morph_buf_puts(&edit->text, edits->eol);
	}
	for (int i = 0; rc == 0 && i < table->u.tab.size; i++) {
		if (table->u.tab.value[i].type != TOML_TABLE) continue;
		morph_buf_t child = {0};
		rc = child_path(&child, path, table->u.tab.key[i]);
		if (rc == 0) rc = new_table(edits, &table->u.tab.value[i],
			morph_buf_cstr(&child), depth + 1);
		morph_buf_cleanup(&child);
	}
	return rc;
}

static int add_member(struct config_edits *edits, const toml_datum_t *anchor,
		const char *relative, const toml_datum_t *value)
{
	int inlined = anchor && (anchor->flag & TOML_FLAG_INLINED);
	size_t at = anchor ? source_offset(edits->source, anchor) : 0;
	if (inlined) at++;
	else if (anchor) {
		const char *nl = strchr(edits->source + at, '\n');
		at = nl ? (size_t)(nl - edits->source) + 1 : strlen(edits->source);
	}
	struct config_edit *edit = edit_at(edits, at, at, 0);
	if (!edit) return -ENOMEM;
	int rc = 0;
	if (inlined) {
		if (edit->text.len) rc = morph_buf_puts(&edit->text, ", ");
		edit->inline_members = anchor->u.tab.size;
	} else if (!edit->text.len && at && edits->source[at - 1] != '\n') {
		rc = morph_buf_puts(&edit->text, edits->eol);
	}
	if (rc == 0) rc = morph_buf_printf(&edit->text, "%s = ", relative);
	if (rc == 0) rc = patch_value(edits, value, &edit->text, 0);
	if (rc == 0 && !inlined) rc = morph_buf_puts(&edit->text, edits->eol);
	return rc;
}

static int collect_edits(struct config_edits *edits, const toml_datum_t *before,
		const toml_datum_t *after, const char *path, const toml_datum_t *anchor,
		const char *relative, unsigned depth)
{
	if (depth > 64) return -ELOOP;
	if (before && ((before->flag & TOML_FLAG_INLINED) ||
	    ((before->flag & (TOML_FLAG_STDEXPR | TOML_FLAG_EXPLICIT)) ==
	     (TOML_FLAG_STDEXPR | TOML_FLAG_EXPLICIT)))) {
		anchor = before;
		relative = "";
	}
	for (int i = 0; i < after->u.tab.size; i++) {
		const char *key = after->u.tab.key[i];
		const toml_datum_t *next = &after->u.tab.value[i];
		const toml_datum_t *previous = member(before, key);
		if (equivalent(previous, next)) continue;
		morph_buf_t child = {0};
		morph_buf_t local = {0};
		int rc = child_path(&child, path, key);
		if (rc == 0) rc = child_path(&local, relative, key);
		if (rc == 0 && previous && previous->type == TOML_TABLE && next->type == TOML_TABLE) {
			rc = collect_edits(edits, previous, next, morph_buf_cstr(&child), anchor,
				morph_buf_cstr(&local), depth + 1);
		} else if (rc == 0 && !previous && next->type == TOML_TABLE &&
			   (!anchor || !(anchor->flag & TOML_FLAG_INLINED))) {
			rc = new_table(edits, next, morph_buf_cstr(&child), depth + 1);
		} else if (rc == 0 && !previous) {
			rc = add_member(edits, anchor, morph_buf_cstr(&local), next);
		} else if (rc == 0) {
			/* Setup only replaces scalar values and adds tables. */
			if (previous->type == TOML_TABLE && !(previous->flag & TOML_FLAG_INLINED)) {
				rc = MORPH_ERR_FORMAT;
			} else {
				size_t start = value_offset(edits->source, previous);
				struct config_edit *edit = edit_at(edits, start, value_end(edits->source, previous), 0);
				rc = edit ? patch_value(edits, next, &edit->text, 0) : -ENOMEM;
			}
		}
		morph_buf_cleanup(&local);
		morph_buf_cleanup(&child);
		if (rc != 0) return rc;
	}
	return 0;
}

static int compare_edits(const void *a, const void *b)
{
	const struct config_edit *left = a;
	const struct config_edit *right = b;
	if (left->start != right->start) return left->start < right->start ? -1 : 1;
	if (left->end != right->end) return left->end < right->end ? -1 : 1;
	return left->section - right->section;
}

static int merged_matches(const toml_datum_t *before, const toml_datum_t *patch,
		const toml_datum_t *result, unsigned depth)
{
	if (depth > 64) return 0;
	if (!before || !patch || before->type != TOML_TABLE || patch->type != TOML_TABLE)
		return equivalent(patch ? patch : before, result);
	if (!result || result->type != TOML_TABLE) return 0;
	int count = before->u.tab.size;
	for (int i = 0; i < patch->u.tab.size; i++)
		if (!member(before, patch->u.tab.key[i])) count++;
	if (result->u.tab.size != count) return 0;
	for (int i = 0; i < before->u.tab.size + patch->u.tab.size; i++) {
		const char *key = i < before->u.tab.size ? before->u.tab.key[i] :
			patch->u.tab.key[i - before->u.tab.size];
		if (!merged_matches(member(before, key), member(patch, key), member(result, key),
			depth + 1)) return 0;
	}
	return 1;
}

int cli_setup_config_merge(const char *old, const char *patch, char **out)
{
	if (!out) return -EINVAL;
	*out = NULL;
	if (!old || !patch) return -EINVAL;
	if (strlen(old) > INT_MAX || strlen(patch) > INT_MAX) return -EFBIG;
	toml_result_t before = toml_parse(old, (int)strlen(old));
	toml_result_t after = toml_parse(patch, (int)strlen(patch));
	struct config_edits edits = {.source = old, .patch = patch,
		.eol = strstr(old, "\r\n") ? "\r\n" : "\n"};
	morph_buf_t result = {0};
	struct config_edit sizing = {0};
	int rc = before.ok && after.ok ? morph_array_init(&edits.items, 16,
		sizeof(sizing)) : MORPH_ERR_PARSE;
	if (rc == 0) rc = collect_edits(&edits, &before.toptab, &after.toptab, "", NULL, "", 0);
	if (rc == 0) rc = morph_buf_init(&result, 0);
	if (rc == 0) {
		if (edits.items.nelts > 1)
			qsort(edits.items.elts, edits.items.nelts, edits.items.size, compare_edits);
		size_t cursor = 0;
		for (size_t i = 0; rc == 0 && i < edits.items.nelts; i++) {
			struct config_edit *edit = morph_array_get(&edits.items, i);
			if (edit->start < cursor) { rc = MORPH_ERR_FORMAT; break; }
			rc = morph_buf_append(&result, old + cursor, edit->start - cursor);
			if (rc == 0) rc = morph_buf_append(&result, edit->text.data, edit->text.len);
			if (rc == 0 && edit->inline_members) rc = morph_buf_puts(&result, ", ");
			cursor = edit->end;
		}
		if (rc == 0) rc = morph_buf_puts(&result, old + cursor);
	}
	if (rc == 0 && result.len > INT_MAX) rc = -EFBIG;
	if (rc == 0) {
		toml_result_t checked = toml_parse(morph_buf_cstr(&result), (int)result.len);
		rc = checked.ok ? 0 : MORPH_ERR_PARSE;
		if (rc == 0 && !merged_matches(&before.toptab, &after.toptab, &checked.toptab, 0))
			rc = MORPH_ERR_FORMAT;
		toml_free(checked);
	}
	if (rc == 0) *out = morph_buf_detach(&result);
	for (size_t i = 0; i < edits.items.nelts; i++) {
		struct config_edit *edit = morph_array_get(&edits.items, i);
		morph_buf_cleanup(&edit->text);
	}
	morph_array_cleanup(&edits.items);
	morph_buf_cleanup(&result);
	toml_free(before);
	toml_free(after);
	return rc;
}

int cli_setup_config_role(const char *text)
{
	toml_result_t parsed = toml_parse(text, (int)strlen(text));
	toml_datum_t role = toml_get(toml_get(parsed.toptab, "setup"), "role");
	int selected = role.type == TOML_STRING ? cli_setup_role_find(role.u.s) : SETUP_GENERAL;
	toml_free(parsed);
	return selected;
}
