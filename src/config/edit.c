#include "edit.h"
#include "nb/cleanup.h"
#include "config/config.h"
#include "cJSON.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/toml.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct config_text_edit {
	size_t start;
	size_t end;
	size_t inline_members;
	int section;
	nb_buf text;
};

struct config_text_edits {
	const char *source;
	const char *patch;
	const char *eol;
	nb_vec items;
};

static void config_text_edits_cleanup(struct config_text_edits *edits)
{
	for (size_t i = 0; i < nb_vec_len(&edits->items); i++) {
		struct config_text_edit *edit = nb_vec_at(&edits->items, i);
		nb_buf_destroy(&edit->text);
	}
	nb_vec_destroy(&edits->items);
}

static const nb_toml_value *config_edit_member(const nb_toml_value *table, const char *key)
{
	for (size_t i = 0;
	     table && nb_toml_type_of(table) == NB_TOML_TABLE && i < nb_toml_table_size(table); i++)
		if (strcmp(nb_toml_table_key_at(table, (size_t)(i)).ptr, key) == 0)
			return nb_toml_table_value_at(table, (size_t)(i));
	return NULL;
}

static int config_edit_equivalent(const nb_toml_value *a, const nb_toml_value *b)
{
	return nb_toml_value_equal(a, b);
}

static size_t config_edit_source_offset(const char *source, const nb_toml_value *node)
{
	(void)source;
	return nb_toml_location_of(node).start;
}

static size_t config_edit_value_offset(const char *source, const nb_toml_value *node)
{
	return config_edit_source_offset(source, node);
}

/* The TOML parser validates syntax. This scanner only locates token ends for
 * source edits, preserving comments, quoted keys and unrelated values verbatim. */
static size_t config_edit_string_end(const char *text, size_t start)
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
			while (text[end] == quote)
				end++;
			if (!multi)
				return at + 1;
			if (end - at >= 3)
				return end;
			at = end;
		} else
			at++;
	}
	return at;
}

static size_t config_edit_value_end(const char *text, const nb_toml_value *value)
{
	(void)text;
	return nb_toml_location_of(value).end;
}

static int config_edit_key_text(nb_buf *out, const char *key)
{
	int bare = *key != '\0';
	for (const char *p = key; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
			bare = 0;
	if (bare)
		return (nb_buf_append_cstr(out, key) ? 0 : NB_ERR_NOMEM);
	cJSON *string = cJSON_CreateString(key);
	char *encoded = string ? cJSON_PrintUnformatted(string) : NULL;
	cJSON_Delete(string);
	if (!encoded)
		return NB_ERR_NOMEM;
	int rc = (nb_buf_append_cstr(out, encoded) ? 0 : NB_ERR_NOMEM);
	free(encoded);
	return rc;
}

static int config_edit_child_path(nb_buf *out, const char *parent, const char *key)
{
	int rc = (nb_buf_init(out), nb_buf_reserve(out, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && *parent)
		rc = (nb_buf_printf(out, "%s.", parent) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = config_edit_key_text(out, key);
	return rc;
}

static int config_edit_patch_value(struct config_text_edits *edits, const nb_toml_value *value,
				   nb_buf *out, unsigned depth)
{
	if (depth > 64)
		return NB_ERR_LOOP;
	if (nb_toml_type_of(value) != NB_TOML_TABLE) {
		size_t start = config_edit_value_offset(edits->patch, value);
		return (nb_buf_append(out, edits->patch + start,
				      config_edit_value_end(edits->patch, value) - start)
				? 0
				: NB_ERR_NOMEM);
	}
	int rc = (nb_buf_append_char(out, '{') ? 0 : NB_ERR_NOMEM);
	for (size_t i = 0; rc == 0 && i < nb_toml_table_size(value); i++) {
		if (i)
			rc = (nb_buf_append_cstr(out, ", ") ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = config_edit_key_text(out,
						  nb_toml_table_key_at(value, (size_t)(i)).ptr);
		if (rc == 0)
			rc = (nb_buf_append_cstr(out, " = ") ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = config_edit_patch_value(
				edits, nb_toml_table_value_at(value, (size_t)(i)), out, depth + 1);
	}
	if (rc == 0)
		rc = (nb_buf_append_char(out, '}') ? 0 : NB_ERR_NOMEM);
	return rc;
}

static struct config_text_edit *config_edit_edit_at(struct config_text_edits *edits, size_t start,
						  size_t end, int section)
{
	for (size_t i = 0; start == end && i < edits->items.len; i++) {
		struct config_text_edit *edit = nb_vec_at(&edits->items, i);
		if (edit->start == start && edit->end == end && edit->section == section)
			return edit;
	}
	struct config_text_edit *edit = nb_vec_push(&edits->items);
	if (!edit)
		return NULL;
	*edit = (struct config_text_edit){.start = start, .end = end, .section = section};
	if ((nb_buf_init(&edit->text), nb_buf_reserve(&edit->text, 128) ? 0 : NB_ERR_NOMEM) != 0)
		return NULL;
	return edit;
}

static int config_edit_new_table(struct config_text_edits *edits, const nb_toml_value *table,
				 const char *path, unsigned depth)
{
	if (depth > 64)
		return NB_ERR_LOOP;
	struct config_text_edit *edit =
		config_edit_edit_at(edits, strlen(edits->source), strlen(edits->source), 1);
	if (!edit)
		return NB_ERR_NOMEM;
	int rc = (nb_buf_printf(&edit->text, "%s[%s]%s", edits->eol, path, edits->eol)
			  ? 0
			  : NB_ERR_NOMEM);
	for (size_t i = 0; rc == 0 && i < nb_toml_table_size(table); i++) {
		if (nb_toml_type_of(nb_toml_table_value_at(table, (size_t)(i))) == NB_TOML_TABLE)
			continue;
		rc = config_edit_key_text(&edit->text,
					  nb_toml_table_key_at(table, (size_t)(i)).ptr);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&edit->text, " = ") ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = config_edit_patch_value(edits,
						     nb_toml_table_value_at(table, (size_t)(i)),
						     &edit->text, depth);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&edit->text, edits->eol) ? 0 : NB_ERR_NOMEM);
	}
	for (size_t i = 0; rc == 0 && i < nb_toml_table_size(table); i++) {
		if (nb_toml_type_of(nb_toml_table_value_at(table, (size_t)(i))) != NB_TOML_TABLE)
			continue;
		nb_buf child NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = config_edit_child_path(&child, path,
					    nb_toml_table_key_at(table, (size_t)(i)).ptr);
		if (rc == 0)
			rc = config_edit_new_table(edits,
						   nb_toml_table_value_at(table, (size_t)(i)),
						   nb_buf_cstr(&child), depth + 1);
		nb_buf_cleanup(&child);
	}
	return rc;
}

static int config_edit_add_member(struct config_text_edits *edits, const nb_toml_value *anchor,
				  const char *relative, const nb_toml_value *value)
{
	int inlined = anchor && nb_toml_location_of(anchor).inline_table;
	size_t at = anchor ? config_edit_source_offset(edits->source, anchor) : 0;
	if (inlined)
		at++;
	else if (anchor) {
		const char *nl = strchr(edits->source + at, '\n');
		at = nl ? (size_t)(nl - edits->source) + 1 : strlen(edits->source);
	}
	struct config_text_edit *edit = config_edit_edit_at(edits, at, at, 0);
	if (!edit)
		return NB_ERR_NOMEM;
	int rc = 0;
	if (inlined) {
		if (edit->text.len)
			rc = (nb_buf_append_cstr(&edit->text, ", ") ? 0 : NB_ERR_NOMEM);
		edit->inline_members = nb_toml_table_size(anchor);
	} else if (!edit->text.len && at && edits->source[at - 1] != '\n') {
		rc = (nb_buf_append_cstr(&edit->text, edits->eol) ? 0 : NB_ERR_NOMEM);
	}
	if (rc == 0)
		rc = (nb_buf_printf(&edit->text, "%s = ", relative) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = config_edit_patch_value(edits, value, &edit->text, 0);
	if (rc == 0 && !inlined)
		rc = (nb_buf_append_cstr(&edit->text, edits->eol) ? 0 : NB_ERR_NOMEM);
	return rc;
}

static int config_edit_collect_edits(struct config_text_edits *edits, const nb_toml_value *before,
				     const nb_toml_value *after, const char *path,
				     const nb_toml_value *anchor, const char *relative,
				     unsigned depth)
{
	if (depth > 64)
		return NB_ERR_LOOP;
	if (before && (nb_toml_location_of(before).inline_table ||
		       nb_toml_location_of(before).table_header)) {
		anchor = before;
		relative = "";
	}
	for (size_t i = 0; i < nb_toml_table_size(after); i++) {
		const char *key = nb_toml_table_key_at(after, (size_t)(i)).ptr;
		const nb_toml_value *next = nb_toml_table_value_at(after, (size_t)(i));
		const nb_toml_value *previous = config_edit_member(before, key);
		if (config_edit_equivalent(previous, next))
			continue;
		nb_buf child NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf local NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = config_edit_child_path(&child, path, key);
		if (rc == 0)
			rc = config_edit_child_path(&local, relative, key);
		if (rc == 0 && previous && nb_toml_type_of(previous) == NB_TOML_TABLE &&
		    nb_toml_type_of(next) == NB_TOML_TABLE) {
			rc = config_edit_collect_edits(edits, previous, next, nb_buf_cstr(&child),
						       anchor, nb_buf_cstr(&local), depth + 1);
		} else if (rc == 0 && !previous && nb_toml_type_of(next) == NB_TOML_TABLE &&
			   (!anchor || !nb_toml_location_of(anchor).inline_table)) {
			rc = config_edit_new_table(edits, next, nb_buf_cstr(&child), depth + 1);
		} else if (rc == 0 && !previous) {
			rc = config_edit_add_member(edits, anchor, nb_buf_cstr(&local), next);
		} else if (rc == 0) {
			/* Setup only replaces scalar values and adds tables. */
			if (nb_toml_type_of(previous) == NB_TOML_TABLE &&
			    !nb_toml_location_of(previous).inline_table) {
				rc = NB_ERR_FORMAT;
			} else {
				size_t start = config_edit_value_offset(edits->source, previous);
				struct config_text_edit *edit = config_edit_edit_at(
					edits, start,
					config_edit_value_end(edits->source, previous), 0);
				rc = edit ? config_edit_patch_value(edits, next, &edit->text, 0)
					  : NB_ERR_NOMEM;
			}
		}
		nb_buf_cleanup(&local);
		nb_buf_cleanup(&child);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int config_edit_compare_edits(const void *a, const void *b)
{
	const struct config_text_edit *left = a;
	const struct config_text_edit *right = b;
	if (left->start != right->start)
		return left->start < right->start ? -1 : 1;
	if (left->end != right->end)
		return left->end < right->end ? -1 : 1;
	return left->section - right->section;
}

static int config_edit_merged_matches(const nb_toml_value *before, const nb_toml_value *patch,
				      const nb_toml_value *result, unsigned depth)
{
	if (depth > 64)
		return 0;
	if (!before || !patch || nb_toml_type_of(before) != NB_TOML_TABLE ||
	    nb_toml_type_of(patch) != NB_TOML_TABLE)
		return config_edit_equivalent(patch ? patch : before, result);
	if (!result || nb_toml_type_of(result) != NB_TOML_TABLE)
		return 0;
	size_t count = nb_toml_table_size(before);
	for (size_t i = 0; i < nb_toml_table_size(patch); i++)
		if (!config_edit_member(before, nb_toml_table_key_at(patch, (size_t)(i)).ptr))
			count++;
	if (nb_toml_table_size(result) != count)
		return 0;
	for (size_t i = 0; i < nb_toml_table_size(before) + nb_toml_table_size(patch); i++) {
		const char *key = i < nb_toml_table_size(before)
					  ? nb_toml_table_key_at(before, (size_t)(i)).ptr
					  : nb_toml_table_key_at(
						    patch, (size_t)(i - nb_toml_table_size(before)))
						    .ptr;
		if (!config_edit_merged_matches(config_edit_member(before, key),
					       config_edit_member(patch, key),
					       config_edit_member(result, key), depth + 1))
			return 0;
	}
	return 1;
}

int config_text_merge_copy(const char *old, const char *patch, char **out)
{
	nb_err parse_error = {0};
	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (!old || !patch)
		return NB_ERR_INVAL;
	if (strlen(old) > INT_MAX || strlen(patch) > INT_MAX)
		return NB_ERR_TOO_BIG;
	nb_toml *before NB_CLEANUP(nb_toml_cleanup) =
		nb_toml_parse(nb_str_from_cstr(old), &parse_error);
	nb_toml *after NB_CLEANUP(nb_toml_cleanup) =
		nb_toml_parse(nb_str_from_cstr(patch), &parse_error);
	struct config_text_edits edits NB_CLEANUP(config_text_edits_cleanup) = {
		.source = old, .patch = patch, .eol = strstr(old, "\r\n") ? "\r\n" : "\n"};
	nb_buf result NB_CLEANUP(nb_buf_cleanup) = {0};
	struct config_text_edit sizing = {0};
	int rc = before && after ? (nb_vec_init(&edits.items, sizeof(sizing)),
				    nb_vec_reserve(&edits.items, 16) ? 0 : NB_ERR_NOMEM)
				 : NB_ERR_PARSE;
	if (rc == 0)
		rc = config_edit_collect_edits(&edits, nb_toml_root(before), nb_toml_root(after),
					       "", NULL, "", 0);
	if (rc == 0)
		rc = (nb_buf_init(&result), nb_buf_reserve(&result, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc == 0) {
		if (edits.items.len > 1)
			qsort(((void *)edits.items.data), edits.items.len, edits.items.elem_size,
			      config_edit_compare_edits);
		size_t cursor = 0;
		for (size_t i = 0; rc == 0 && i < edits.items.len; i++) {
			struct config_text_edit *edit = nb_vec_at(&edits.items, i);
			if (edit->start < cursor) {
				rc = NB_ERR_FORMAT;
				break;
			}
			rc = (nb_buf_append(&result, old + cursor, edit->start - cursor)
				      ? 0
				      : NB_ERR_NOMEM);
			if (rc == 0)
				rc = (nb_buf_append(&result, edit->text.ptr, edit->text.len)
					      ? 0
					      : NB_ERR_NOMEM);
			if (rc == 0 && edit->inline_members)
				rc = (nb_buf_append_cstr(&result, ", ") ? 0 : NB_ERR_NOMEM);
			cursor = edit->end;
		}
		if (rc == 0)
			rc = (nb_buf_append_cstr(&result, old + cursor) ? 0 : NB_ERR_NOMEM);
	}
	if (rc == 0 && result.len > INT_MAX)
		rc = NB_ERR_TOO_BIG;
	if (rc == 0) {
		nb_toml *checked NB_CLEANUP(nb_toml_cleanup) = nb_toml_parse(
			nb_str_from_parts(nb_buf_cstr(&result), result.len), &parse_error);
		rc = checked ? 0 : NB_ERR_PARSE;
		if (rc == 0 &&
		    !config_edit_merged_matches(nb_toml_root(before), nb_toml_root(after),
						nb_toml_root(checked), 0))
			rc = NB_ERR_FORMAT;
		nb_toml_cleanup(&checked);
	}
	if (rc == 0) {
		*out = nb_buf_ok(&result) ? strdup(nb_buf_cstr(&result)) : NULL;
		if (!*out)
			rc = NB_ERR_NOMEM;
	}
	nb_buf_cleanup(&result);
	nb_toml_cleanup(&before);
	nb_toml_cleanup(&after);
	return rc;
}

static int config_edit_model_name(const char *key)
{
	return strcmp(key, "text") == 0 || strcmp(key, "vision") == 0 ||
	       strcmp(key, "image") == 0 || strcmp(key, "video") == 0;
}

static int config_edit_unused_credential(const nb_toml_value *table, const char *key)
{
	if (strcmp(key, "api_key") != 0 && strcmp(key, "api_key_env") != 0)
		return 0;
	const nb_toml_value *value = config_edit_member(table, key);
	if (!value || nb_toml_type_of(value) != NB_TOML_STRING)
		return 0;
	const nb_toml_value *direct = config_edit_member(table, "api_key");
	return !nb_toml_string(value).ptr[0] ||
	       (strcmp(key, "api_key_env") == 0 && direct &&
		nb_toml_type_of(direct) == NB_TOML_STRING && nb_toml_string(direct).ptr[0]);
}

/* The parsed value position identifies the assignment. Skip quoted tokens and
 * comments while locating its key, including dotted keys in inline tables. */
static size_t config_edit_assignment_start(const char *text, size_t value, size_t *separator)
{
	size_t start = 0;
	*separator = 0;
	for (size_t at = 0; at < value; at++) {
		if (text[at] == '"' || text[at] == '\'')
			at = config_edit_string_end(text, at) - 1;
		else if (text[at] == '#') {
			while (at < value && text[at] != '\n')
				at++;
			start = at + 1;
		} else if (text[at] == '\n' || text[at] == '{' || text[at] == ',') {
			start = at + 1;
			if (text[at] != '\n')
				*separator = at;
		}
	}
	while (start < value && strchr(" \t\r\n", text[start]))
		start++;
	return start;
}

static int config_edit_remove_credential(struct config_text_edits *edits,
					 const nb_toml_value *value, int inlined)
{
	const char *text = edits->source;
	size_t separator;
	size_t start =
		config_edit_assignment_start(text,
			config_edit_value_offset(text, value), &separator);
	size_t end = config_edit_value_end(text, value);
	if (inlined) {
		size_t next = end;
		while (text[next]) {
			if (strchr(" \t\r\n", text[next]))
				next++;
			else if (text[next] == '#') {
				while (text[next] && text[next] != '\n')
					next++;
			} else
				break;
		}
		if (text[next] == ',')
			separator = next;
		if (text[separator] == ',' &&
		    !config_edit_edit_at(edits, separator, separator + 1, 0))
			return NB_ERR_NOMEM;
	} else {
		size_t line = start;
		while (line && text[line - 1] != '\n')
			line--;
		size_t tail = end;
		while (text[tail] == ' ' || text[tail] == '\t' || text[tail] == '\r')
			tail++;
		if (!text[tail] || text[tail] == '\n') {
			start = line;
			end = tail + (text[tail] == '\n' ? 1u : 0u);
		}
	}
	return config_edit_edit_at(edits, start, end, 0) ? 0 : NB_ERR_NOMEM;
}

static int config_edit_credential_result_matches(const nb_toml_value *before,
						 const nb_toml_value *after, int level)
{
	if (!before || nb_toml_type_of(before) != NB_TOML_TABLE)
		return config_edit_equivalent(before, after);
	if (!after || nb_toml_type_of(after) != NB_TOML_TABLE)
		return 0;
	size_t count = nb_toml_table_size(before);
	for (size_t i = 0; i < nb_toml_table_size(before); i++) {
		const char *key = nb_toml_table_key_at(before, (size_t)(i)).ptr;
		const nb_toml_value *next = config_edit_member(after, key);
		if (level == 2 && config_edit_unused_credential(before, key)) {
			if (next)
				return 0;
			count--;
			continue;
		}
		const nb_toml_value *previous = nb_toml_table_value_at(before, (size_t)(i));
		int child = level == 0 && strcmp(key, "model") == 0    ? 1
			    : level == 1 && config_edit_model_name(key) ? 2
								       : -1;
		if (child < 0 ? !config_edit_equivalent(previous, next)
			      : !config_edit_credential_result_matches(previous, next, child))
			return 0;
	}
	return count == nb_toml_table_size(after);
}

int config_text_credentials_copy(const char *text, char **out)
{
	nb_err parse_error = {0};
	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (!text)
		return NB_ERR_INVAL;
	if (strlen(text) > INT_MAX)
		return NB_ERR_TOO_BIG;
	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) =
		nb_toml_parse(nb_str_from_cstr(text), &parse_error);
	struct config_text_edits edits NB_CLEANUP(config_text_edits_cleanup) = {.source = text};
	struct config_text_edit sizing = {0};
	nb_buf result NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = parsed ? (nb_vec_init(&edits.items, sizeof(sizing)),
			   nb_vec_reserve(&edits.items, 8) ? 0 : NB_ERR_NOMEM)
			: NB_ERR_PARSE;
	const nb_toml_value *models =
		parsed ? config_edit_member(nb_toml_root(parsed), "model") : NULL;
	for (size_t i = 0; rc == 0 && models && nb_toml_type_of(models) == NB_TOML_TABLE &&
			   i < nb_toml_table_size(models);
	     i++) {
		const nb_toml_value *entry = nb_toml_table_value_at(models, (size_t)(i));
		if (!config_edit_model_name(nb_toml_table_key_at(models, (size_t)(i)).ptr) ||
		    nb_toml_type_of(entry) != NB_TOML_TABLE)
			continue;
		for (size_t k = 0; rc == 0 && k < nb_toml_table_size(entry); k++)
			if (config_edit_unused_credential(
				    entry, nb_toml_table_key_at(entry, (size_t)(k)).ptr))
				rc = config_edit_remove_credential(
					&edits, nb_toml_table_value_at(entry, (size_t)(k)),
					(nb_toml_location_of(models).inline_table ||
					 nb_toml_location_of(entry).inline_table));
	}
	if (rc == 0)
		rc = (nb_buf_init(&result), nb_buf_reserve(&result, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc == 0) {
		if (edits.items.len > 1)
			qsort(((void *)edits.items.data), edits.items.len, edits.items.elem_size,
			      config_edit_compare_edits);
		size_t cursor = 0;
		for (size_t i = 0; rc == 0 && i < edits.items.len; i++) {
			struct config_text_edit *edit = nb_vec_at(&edits.items, i);
			if (edit->start > cursor)
				rc = (nb_buf_append(&result, text + cursor, edit->start - cursor)
					      ? 0
					      : NB_ERR_NOMEM);
			if (edit->end > cursor)
				cursor = edit->end;
		}
		if (rc == 0)
			rc = (nb_buf_append_cstr(&result, text + cursor) ? 0 : NB_ERR_NOMEM);
	}
	if (rc == 0) {
		nb_toml *checked NB_CLEANUP(nb_toml_cleanup) = nb_toml_parse(
			nb_str_from_parts(nb_buf_cstr(&result), result.len), &parse_error);
		rc = checked ? 0 : NB_ERR_PARSE;
		if (rc == 0 && !config_edit_credential_result_matches(nb_toml_root(parsed),
								      nb_toml_root(checked), 0))
			rc = NB_ERR_FORMAT;
		nb_toml_cleanup(&checked);
	}
	if (rc == 0) {
		*out = nb_buf_ok(&result) ? strdup(nb_buf_cstr(&result)) : NULL;
		if (!*out)
			rc = NB_ERR_NOMEM;
	}
	nb_buf_cleanup(&result);
	nb_toml_cleanup(&parsed);
	return rc;
}

static const char *config_edit_string_member(const nb_toml_value *table, const char *key)
{
	const nb_toml_value *value = config_edit_member(table, key);
	return value && nb_toml_type_of(value) == NB_TOML_STRING ? nb_toml_string(value).ptr : "";
}

static int config_edit_same_mcp(const nb_toml_value *before, const nb_toml_value *added)
{
	if (strcmp(config_edit_string_member(before, "name"),
		   config_edit_string_member(added, "name")) == 0)
		return 1;
	const char *url = config_edit_string_member(before, "url");
	const char *target = config_edit_string_member(added, "url");
	if (*url && *target && strcspn(url, "?") == strcspn(target, "?") &&
	    strncmp(url, target, strcspn(url, "?")) == 0)
		return 1;
	const char *name = config_edit_string_member(added, "name");
	const char *package = strcmp(name, "playwright") == 0		? "@playwright/mcp"
			      : strcmp(name, "macos-computer-use") == 0 ? "macos-computer-use-kit"
									: NULL;
	const nb_toml_value *args = config_edit_member(before, "args");
	for (size_t i = 0; package && args && nb_toml_type_of(args) == NB_TOML_ARRAY &&
			   i < nb_toml_array_size(args);
	     i++) {
		const nb_toml_value *arg = nb_toml_array_get(args, (size_t)(i));
		size_t len = strlen(package);
		if (nb_toml_type_of(arg) == NB_TOML_STRING &&
		    strncmp(nb_toml_string(arg).ptr, package, len) == 0 &&
		    (!nb_toml_string(arg).ptr[len] || nb_toml_string(arg).ptr[len] == '@' ||
		     nb_toml_string(arg).ptr[len] == '='))
			return 1;
	}
	const char *command = config_edit_string_member(before, "command");
	const char *base = strrchr(command, '/');
	return strcmp(name, "macos-computer-use") == 0 &&
	       strcmp(base ? base + 1 : command, "macos-cu") == 0;
}

int config_text_mcp_add_copy(const char *text, const char *server, char **out)
{
	nb_err parse_error = {0};
	if (!out)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (!text || !server)
		return NB_ERR_INVAL;
	if (strlen(text) > INT_MAX || strlen(server) > INT_MAX)
		return NB_ERR_TOO_BIG;
	struct config_validation_error error = {0};
	int rc = config_validate_text(server, &error);
	if (rc != 0)
		return rc;
	nb_toml *before NB_CLEANUP(nb_toml_cleanup) =
		nb_toml_parse(nb_str_from_cstr(text), &parse_error);
	nb_toml *preset NB_CLEANUP(nb_toml_cleanup) =
		nb_toml_parse(nb_str_from_cstr(server), &parse_error);
	const nb_toml_value *mcp = before ? config_edit_member(nb_toml_root(before), "mcp") : NULL;
	const nb_toml_value *servers = config_edit_member(mcp, "servers");
	const nb_toml_value *list =
		preset ? config_edit_member(config_edit_member(nb_toml_root(preset), "mcp"),
					    "servers")
		       : NULL;
	const nb_toml_value *added =
		list && nb_toml_type_of(list) == NB_TOML_ARRAY && nb_toml_array_size(list) == 1
			? nb_toml_array_get(list, (size_t)(0))
			: NULL;
	rc = before && added && nb_toml_type_of(added) == NB_TOML_TABLE ? 0 : NB_ERR_PARSE;
	size_t count = servers && nb_toml_type_of(servers) == NB_TOML_ARRAY
			       ? nb_toml_array_size(servers)
			       : 0;
	int exists = 0;
	for (size_t i = 0; rc == 0 && i < count; i++)
		if (config_edit_same_mcp(nb_toml_array_get(servers, (size_t)(i)), added))
			exists = 1;
	nb_buf result NB_CLEANUP(nb_buf_cleanup) = {0};
	if (rc == 0 && exists) {
		*out = strdup(text);
		if (!*out)
			rc = NB_ERR_NOMEM;
	} else if (rc == 0 && count >= MCP_SERVER_MAX) {
		rc = NB_ERR_NO_SPACE;
	} else if (rc == 0) {
		rc = (nb_buf_init(&result), nb_buf_reserve(&result, 4096) ? 0 : NB_ERR_NOMEM);
		if (rc == 0 && servers && nb_toml_location_of(servers).line != 0) {
			size_t at = count ? config_edit_value_end(
						    text,
						    nb_toml_array_get(servers, (size_t)(count - 1)))
					  : config_edit_value_offset(text, servers) + 1;
			rc = (nb_buf_append(&result, text, at) ? 0 : NB_ERR_NOMEM);
			if (rc == 0 && count)
				rc = (nb_buf_append_cstr(&result, ", ") ? 0 : NB_ERR_NOMEM);
			struct config_text_edits edits NB_CLEANUP(config_text_edits_cleanup) = {
				.patch = server};
			if (rc == 0)
				rc = config_edit_patch_value(&edits, added, &result, 0);
			if (rc == 0)
				rc = (nb_buf_append_cstr(&result, text + at) ? 0 : NB_ERR_NOMEM);
		} else if (rc == 0 && mcp && nb_toml_location_of(mcp).inline_table) {
			nb_buf patch NB_CLEANUP(nb_buf_cleanup) = {0};
			rc = (nb_buf_init(&patch), nb_buf_reserve(&patch, 4096) ? 0 : NB_ERR_NOMEM);
			if (rc == 0)
				rc = (nb_buf_append_cstr(&patch, "[mcp]\nservers = [")
					      ? 0
					      : NB_ERR_NOMEM);
			struct config_text_edits edits NB_CLEANUP(config_text_edits_cleanup) = {
				.patch = server};
			if (rc == 0)
				rc = config_edit_patch_value(&edits, added, &patch, 0);
			if (rc == 0)
				rc = (nb_buf_append_cstr(&patch, "]\n") ? 0 : NB_ERR_NOMEM);
			char *merged = NULL;
			if (rc == 0)
				rc = config_text_merge_copy(text, nb_buf_cstr(&patch), &merged);
			if (rc == 0)
				rc = (nb_buf_append_cstr(&result, merged) ? 0 : NB_ERR_NOMEM);
			free(merged);
			nb_buf_cleanup(&patch);
		} else if (rc == 0) {
			rc = (nb_buf_append_cstr(&result, text) ? 0 : NB_ERR_NOMEM);
			const char *eol = strstr(text, "\r\n") ? "\r\n" : "\n";
			if (rc == 0)
				rc = (nb_buf_append_cstr(&result, eol) ? 0 : NB_ERR_NOMEM);
			for (const char *p = server; rc == 0 && *p; p++)
				rc = *p == '\n'
					     ? (nb_buf_append_cstr(&result, eol) ? 0 : NB_ERR_NOMEM)
					     : (nb_buf_append_char(&result, *p) ? 0 : NB_ERR_NOMEM);
		}
		if (rc == 0 && result.len > INT_MAX)
			rc = NB_ERR_TOO_BIG;
		if (rc == 0)
			rc = config_validate_text(nb_buf_cstr(&result), &error);
		if (rc == 0) {
			*out = nb_buf_ok(&result) ? strdup(nb_buf_cstr(&result)) : NULL;
			if (!*out)
				rc = NB_ERR_NOMEM;
		}
	}
	nb_buf_cleanup(&result);
	nb_toml_cleanup(&preset);
	nb_toml_cleanup(&before);
	return rc;
}
