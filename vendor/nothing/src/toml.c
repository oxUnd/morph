#include "nb/toml.h"

#include "nb/buf.h"
#include "nb/file.h"
#include "nb/mem.h"
#include "nb/utf8.h"
#include "nb/vec.h"

#include <locale.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>

#define TOML_MAX_DEPTH 128u

/*
 * TOML is parsed with a hand-written recursive-descent parser. There is no
 * regex or generated grammar; every syntax decision is made against the
 * current cursor and the resulting nodes form the immutable AST exposed by
 * nb/toml.h.
 */

typedef enum toml_table_state {
    TOML_TABLE_IMPLICIT = 0,
    TOML_TABLE_EXPLICIT,
    TOML_TABLE_DOTTED,
    TOML_TABLE_INLINE,
    TOML_TABLE_INLINE_DOTTED,
    TOML_TABLE_ARRAY_ELEMENT
} toml_table_state;

struct nb_toml_value {
    nb_toml_type type;
    toml_table_state state;
    nb_toml_location location;
    bool array_of_tables;
    union {
        struct {
            char *ptr;
            size_t len;
        } string;
        int64_t integer;
        double floating;
        bool boolean;
        nb_toml_datetime datetime;
        struct {
            nb_map *entries;
        } table;
        struct {
            nb_vec items;
        } array;
    } as;
};

struct nb_toml {
    nb_arena *arena;
    nb_toml_value root;
    nb_vec line_starts;
};

typedef struct toml_parser {
    nb_str text;
    size_t pos;
    nb_err *err;
    nb_toml *doc;
    nb_toml_value *current;
} toml_parser;

static bool toml_parse_document(toml_parser *p);

static void toml_set_location(toml_parser *p, nb_toml_value *value, size_t start, size_t end)
{
    value->location.start = start;
    value->location.end = end;
    size_t low = 0;
    size_t high = nb_vec_len(&p->doc->line_starts);
    while (low + 1u < high) {
        size_t mid = low + (high - low) / 2u;
        if (*(size_t *)nb_vec_at(&p->doc->line_starts, mid) <= start) {
            low = mid;
        } else {
            high = mid;
        }
    }
    size_t line_start = *(size_t *)nb_vec_at(&p->doc->line_starts, low);
    value->location.line = low < INT_MAX ? (int)low + 1 : INT_MAX;
    size_t column = start - line_start;
    value->location.column = column < INT_MAX ? (int)column + 1 : INT_MAX;
    value->location.inline_table =
        value->type == NB_TOML_TABLE &&
        (value->state == TOML_TABLE_INLINE || value->state == TOML_TABLE_INLINE_DOTTED);
    value->location.table_header =
        value->type == NB_TOML_TABLE &&
        (value->state == TOML_TABLE_EXPLICIT || value->state == TOML_TABLE_ARRAY_ELEMENT);
}

static bool toml_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool toml_is_hex(char c)
{
    return toml_is_digit(c) || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool toml_is_space(char c)
{
    return c == ' ' || c == '\t';
}

static bool toml_is_bare(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           toml_is_digit(c) || c == '_' || c == '-';
}

static bool toml_at_end(const toml_parser *p)
{
    return p->pos >= p->text.len;
}

static char toml_peek(const toml_parser *p)
{
    return toml_at_end(p) ? '\0' : p->text.ptr[p->pos];
}

static char toml_peek_at(const toml_parser *p, size_t offset)
{
    return p->pos + offset < p->text.len ? p->text.ptr[p->pos + offset] : '\0';
}

static bool toml_match(toml_parser *p, const char *literal, size_t len)
{
    if (p->text.len - p->pos < len) {
        return false;
    }
    if (memcmp(p->text.ptr + p->pos, literal, len) != 0) {
        return false;
    }
    p->pos += len;
    return true;
}

static bool toml_nomem(toml_parser *p)
{
    if (p->err) {
        nb_err_set(p->err, NB_ERR_NOMEM, "TOML: out of memory");
    }
    return false;
}

static bool toml_error(toml_parser *p, size_t position, const char *fmt, ...)
{
    if (!p->err) {
        return false;
    }
    size_t line = 1;
    size_t column = 1;
    size_t limit = position < p->text.len ? position : p->text.len;
    for (size_t i = 0; i < limit; i++) {
        unsigned char byte = (unsigned char)p->text.ptr[i];
        if (byte == '\n') {
            line++;
            column = 1;
        } else if ((byte & 0xC0u) != 0x80u) {
            column++;
        }
    }
    char detail[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    nb_err_set(p->err, NB_ERR_PARSE, "TOML line %zu, column %zu: %s",
               line, column, detail);
    return false;
}

static bool toml_newline_at(const toml_parser *p)
{
    char c = toml_peek(p);
    if (c == '\n') {
        return true;
    }
    return c == '\r' && toml_peek_at(p, 1) == '\n';
}

static bool toml_consume_newline(toml_parser *p)
{
    if (toml_peek(p) == '\n') {
        p->pos++;
        return true;
    }
    if (toml_peek(p) == '\r') {
        if (toml_peek_at(p, 1) == '\n') {
            p->pos += 2;
            return true;
        }
        return toml_error(p, p->pos, "bare carriage return");
    }
    return false;
}

static void toml_skip_spaces(toml_parser *p)
{
    while (!toml_at_end(p) && toml_is_space(toml_peek(p))) {
        p->pos++;
    }
}

static bool toml_check_comment_byte(toml_parser *p, unsigned char c)
{
    if (c == '\t' || (c >= 0x20u && c != 0x7fu)) {
        return true;
    }
    return toml_error(p, p->pos, "control character in comment");
}

static bool toml_skip_comment(toml_parser *p)
{
    if (toml_peek(p) != '#') {
        return true;
    }
    p->pos++;
    while (!toml_at_end(p)) {
        char c = toml_peek(p);
        if (c == '\n') {
            return true;
        }
        if (c == '\r') {
            if (toml_peek_at(p, 1) == '\n') {
                return true;
            }
            return toml_error(p, p->pos, "bare carriage return in comment");
        }
        if (!toml_check_comment_byte(p, (unsigned char)c)) {
            return false;
        }
        p->pos++;
    }
    return true;
}

static nb_toml_value *toml_value_new(toml_parser *p, nb_toml_type type)
{
    nb_toml_value *value =
        nb_arena_alloc0(p->doc->arena, sizeof(*value));
    if (!value) {
        toml_nomem(p);
        return NULL;
    }
    value->type = type;
    return value;
}

static nb_toml_value *toml_new_table(toml_parser *p, toml_table_state state)
{
    nb_toml_value *value = toml_value_new(p, NB_TOML_TABLE);
    if (!value) {
        return NULL;
    }
    value->state = state;
    value->as.table.entries = nb_map_new();
    if (!value->as.table.entries) {
        toml_nomem(p);
        return NULL;
    }
    return value;
}

static nb_toml_value *toml_new_array(toml_parser *p, bool array_of_tables)
{
    nb_toml_value *value = toml_value_new(p, NB_TOML_ARRAY);
    if (!value) {
        return NULL;
    }
    value->array_of_tables = array_of_tables;
    nb_vec_init(&value->as.array.items, sizeof(nb_toml_value *));
    return value;
}

static nb_toml_value *toml_new_string(toml_parser *p, const char *data,
                                      size_t len)
{
    if (len == SIZE_MAX) {
        toml_error(p, p->pos, "string is too long");
        return NULL;
    }
    nb_toml_value *value = toml_value_new(p, NB_TOML_STRING);
    if (!value) {
        return NULL;
    }
    char *copy = nb_arena_alloc(p->doc->arena, len + 1u);
    if (!copy) {
        toml_nomem(p);
        return NULL;
    }
    if (len != 0) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    value->as.string.ptr = copy;
    value->as.string.len = len;
    return value;
}

static bool toml_dup_string(toml_parser *p, const char *data, size_t len,
                            nb_str *out)
{
    if (len == SIZE_MAX) {
        return toml_error(p, p->pos, "key is too long");
    }
    char *copy = nb_arena_alloc(p->doc->arena, len + 1u);
    if (!copy) {
        return toml_nomem(p);
    }
    if (len != 0) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    *out = nb_str_from_parts(copy, len);
    return true;
}

static void toml_value_cleanup(nb_toml_value *value)
{
    if (!value) {
        return;
    }
    if (value->type == NB_TOML_TABLE) {
        nb_map *entries = value->as.table.entries;
        if (!entries) {
            return;
        }
        nb_map_iter iter;
        nb_str key;
        void *item = NULL;
        nb_map_iter_init(entries, &iter);
        while (nb_map_iter_next(&iter, &key, &item)) {
            toml_value_cleanup(item);
        }
        nb_map_free(entries);
        value->as.table.entries = NULL;
    } else if (value->type == NB_TOML_ARRAY) {
        nb_vec *items = &value->as.array.items;
        for (size_t i = 0; i < nb_vec_len(items); i++) {
            toml_value_cleanup(*(nb_toml_value **)nb_vec_at(items, i));
        }
        nb_vec_destroy(items);
    }
}

void nb_toml_free(nb_toml *toml)
{
    if (!toml) {
        return;
    }
    toml_value_cleanup(&toml->root);
    nb_vec_destroy(&toml->line_starts);
    if (toml->arena) {
        nb_arena_free(toml->arena);
    }
    nb_free(toml, sizeof(*toml));
}

const nb_toml_value *nb_toml_root(const nb_toml *toml)
{
    return toml ? &toml->root : NULL;
}

nb_toml_type nb_toml_type_of(const nb_toml_value *value)
{
    return value ? value->type : NB_TOML_NONE;
}

size_t nb_toml_table_size(const nb_toml_value *table)
{
    if (!table || table->type != NB_TOML_TABLE ||
        !table->as.table.entries) {
        return 0;
    }
    return nb_map_size(table->as.table.entries);
}

const nb_toml_value *nb_toml_table_get(const nb_toml_value *table, nb_str key)
{
    if (!table || table->type != NB_TOML_TABLE ||
        !table->as.table.entries) {
        return NULL;
    }
    return nb_map_get(table->as.table.entries, key);
}

const nb_toml_value *nb_toml_table_get_cstr(const nb_toml_value *table,
                                            const char *key)
{
    if (!key) {
        return NULL;
    }
    return nb_toml_table_get(table, nb_str_from_cstr(key));
}

const nb_toml_value *nb_toml_get_path(const nb_toml *toml, const nb_str *keys,
                                      size_t count)
{
    if (!toml || (count != 0 && !keys)) {
        return NULL;
    }
    const nb_toml_value *value = &toml->root;
    for (size_t i = 0; i < count; i++) {
        value = nb_toml_table_get(value, keys[i]);
        if (!value) {
            return NULL;
        }
    }
    return value;
}

void nb_toml_table_iter_init(const nb_toml_value *table,
                             nb_toml_table_iter *iter)
{
    if (!iter) {
        return;
    }
    memset(iter, 0, sizeof(*iter));
    if (table && table->type == NB_TOML_TABLE && table->as.table.entries) {
        nb_map_iter_init(table->as.table.entries, &iter->inner);
    }
}

bool nb_toml_table_iter_next(nb_toml_table_iter *iter, nb_str *key,
                             const nb_toml_value **value)
{
    if (!iter) {
        return false;
    }
    void *item = NULL;
    if (!nb_map_iter_next(&iter->inner, key, &item)) {
        return false;
    }
    if (value) {
        *value = item;
    }
    return true;
}

size_t nb_toml_array_size(const nb_toml_value *array)
{
    if (!array || array->type != NB_TOML_ARRAY) {
        return 0;
    }
    return nb_vec_len(&array->as.array.items);
}

const nb_toml_value *nb_toml_array_get(const nb_toml_value *array,
                                       size_t index)
{
    if (!array || array->type != NB_TOML_ARRAY) {
        return NULL;
    }
    if (index >= nb_vec_len(&array->as.array.items)) {
        return NULL;
    }
    return *(nb_toml_value **)nb_vec_at(&array->as.array.items, index);
}

bool nb_toml_get_string(const nb_toml_value *value, nb_str *out)
{
    if (!value || value->type != NB_TOML_STRING || !out) {
        return false;
    }
    *out = nb_str_from_parts(value->as.string.ptr, value->as.string.len);
    return true;
}

bool nb_toml_get_integer(const nb_toml_value *value, int64_t *out)
{
    if (!value || value->type != NB_TOML_INTEGER || !out) {
        return false;
    }
    *out = value->as.integer;
    return true;
}

bool nb_toml_get_float(const nb_toml_value *value, double *out)
{
    if (!value || value->type != NB_TOML_FLOAT || !out) {
        return false;
    }
    *out = value->as.floating;
    return true;
}

bool nb_toml_get_bool(const nb_toml_value *value, bool *out)
{
    if (!value || value->type != NB_TOML_BOOLEAN || !out) {
        return false;
    }
    *out = value->as.boolean;
    return true;
}

bool nb_toml_get_datetime(const nb_toml_value *value, nb_toml_datetime *out)
{
    if (!value || value->type != NB_TOML_DATETIME || !out) {
        return false;
    }
    *out = value->as.datetime;
    return true;
}

nb_toml *nb_toml_parse(nb_str text, nb_err *err)
{
    if (!text.ptr && text.len != 0) {
        if (err) {
            nb_err_set(err, NB_ERR_INVAL, "TOML: invalid input");
        }
        return NULL;
    }

    size_t bad_offset = 0;
    if (nb_utf8_validate(text, &bad_offset) != NB_UTF8_OK) {
        if (err) {
            nb_err_set(err, NB_ERR_PARSE,
                       "TOML: invalid UTF-8 at byte %zu", bad_offset);
        }
        return NULL;
    }
    const char *nul = text.len ? memchr(text.ptr, '\0', text.len) : NULL;
    if (nul) {
        if (err) {
            nb_err_set(err, NB_ERR_PARSE,
                       "TOML: raw NUL byte at byte %zu",
                       (size_t)(nul - text.ptr));
        }
        return NULL;
    }

    nb_toml *doc = nb_try_alloc(sizeof(*doc));
    if (!doc) {
        if (err) {
            nb_err_set(err, NB_ERR_NOMEM, "TOML: out of memory");
        }
        return NULL;
    }
    memset(doc, 0, sizeof(*doc));
    doc->arena = nb_arena_new(0);
    if (!doc->arena) {
        nb_free(doc, sizeof(*doc));
        if (err) {
            nb_err_set(err, NB_ERR_NOMEM, "TOML: out of memory");
        }
        return NULL;
    }
    nb_vec_init(&doc->line_starts, sizeof(size_t));
    size_t first_line = 0;
    bool lines_ok = nb_vec_push_copy(&doc->line_starts, &first_line);
    for (size_t i = 0; lines_ok && i < text.len; i++) {
        if (text.ptr[i] == '\n') {
            size_t next = i + 1u;
            lines_ok = nb_vec_push_copy(&doc->line_starts, &next);
        }
    }
    if (!lines_ok) {
        nb_toml_free(doc);
        nb_err_set(err, NB_ERR_NOMEM, "TOML: out of memory");
        return NULL;
    }
    doc->root.type = NB_TOML_TABLE;
    doc->root.state = TOML_TABLE_EXPLICIT;
    doc->root.as.table.entries = nb_map_new();
    if (!doc->root.as.table.entries) {
        nb_toml_free(doc);
        if (err) {
            nb_err_set(err, NB_ERR_NOMEM, "TOML: out of memory");
        }
        return NULL;
    }

    toml_parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.text = text;
    parser.err = err;
    parser.doc = doc;
    parser.current = &doc->root;

    if (!toml_parse_document(&parser)) {
        nb_toml_free(doc);
        return NULL;
    }
    return doc;
}

nb_toml *nb_toml_load(const char *path, nb_err *err)
{
    if (!path || path[0] == '\0') {
        if (err) {
            nb_err_set(err, NB_ERR_INVAL, "TOML: invalid path");
        }
        return NULL;
    }
    nb_buf buffer;
    nb_buf_init(&buffer);
    nb_status status = nb_file_read_all(path, &buffer, 0, err);
    if (status != NB_OK) {
        nb_buf_destroy(&buffer);
        return NULL;
    }
    nb_toml *doc = nb_toml_parse(nb_buf_str(&buffer), err);
    nb_buf_destroy(&buffer);
    return doc;
}

static nb_toml_value *toml_parse_value(toml_parser *p, size_t depth);
static nb_toml_value *toml_parse_number(toml_parser *p);
static nb_toml_value *toml_parse_datetime(toml_parser *p);
static nb_toml_value *toml_parse_array(toml_parser *p, size_t depth);
static nb_toml_value *toml_parse_inline_table(toml_parser *p, size_t depth);
static bool toml_parse_quoted(toml_parser *p, char quote,
                              bool allow_multiline, nb_str *out);

static bool toml_at_value_end(const toml_parser *p)
{
    if (toml_at_end(p)) {
        return true;
    }
    char c = toml_peek(p);
    return toml_is_space(c) || c == '\n' || c == '\r' || c == ',' ||
           c == ']' || c == '}' || c == '#';
}

static bool toml_parse_simple_key(toml_parser *p, nb_str *out)
{
    toml_skip_spaces(p);
    char c = toml_peek(p);
    if (c == '"' || c == '\'') {
        return toml_parse_quoted(p, c, false, out);
    }
    size_t start = p->pos;
    while (!toml_at_end(p) && toml_is_bare(toml_peek(p))) {
        p->pos++;
    }
    if (p->pos == start) {
        return toml_error(p, p->pos, "expected a key");
    }
    *out = nb_str_from_parts(p->text.ptr + start, p->pos - start);
    return true;
}

static bool toml_parse_key_path(toml_parser *p, nb_vec *keys)
{
    while (true) {
        nb_str key;
        if (!toml_parse_simple_key(p, &key)) {
            return false;
        }
        if (!nb_vec_push_copy(keys, &key)) {
            return toml_nomem(p);
        }
        if (nb_vec_len(keys) > TOML_MAX_DEPTH) {
            return toml_error(p, p->pos, "too many key segments");
        }
        toml_skip_spaces(p);
        if (toml_peek(p) != '.') {
            return true;
        }
        p->pos++;
    }
}

static bool toml_insert_dotted(toml_parser *p, nb_toml_value *table,
                               const nb_vec *keys, nb_toml_value *value,
                               bool inline_mode)
{
    if (!table || table->type != NB_TOML_TABLE ||
        !table->as.table.entries) {
        return toml_error(p, p->pos, "invalid table context");
    }
    size_t count = nb_vec_len(keys);
    if (count == 0) {
        return toml_error(p, p->pos, "empty key");
    }
    for (size_t i = 0; i + 1u < count; i++) {
        nb_str key = *(nb_str *)nb_vec_at(keys, i);
        nb_toml_value *existing = nb_map_get(table->as.table.entries, key);
        if (!existing) {
            nb_toml_value *child = toml_new_table(
                p, inline_mode ? TOML_TABLE_INLINE_DOTTED : TOML_TABLE_DOTTED);
            if (!child) {
                return false;
            }
            if (!nb_map_put(table->as.table.entries, key, child, NULL)) {
                toml_value_cleanup(child);
                return toml_nomem(p);
            }
            existing = child;
        }
        nb_toml_value *next = existing;
        if (next->type != NB_TOML_TABLE) {
            return toml_error(p, p->pos,
                              "dotted key traverses a non-table value");
        }
        if (inline_mode) {
            if (next->state != TOML_TABLE_INLINE_DOTTED) {
                return toml_error(p, p->pos,
                                  "cannot extend an inline table value");
            }
        } else {
            if (next->state == TOML_TABLE_INLINE ||
                next->state == TOML_TABLE_INLINE_DOTTED) {
                return toml_error(p, p->pos,
                                  "cannot extend an inline table");
            }
            if (next->state == TOML_TABLE_EXPLICIT) {
                return toml_error(
                    p, p->pos,
                    "cannot extend an explicitly defined table with a dotted key");
            }
            if (next->state == TOML_TABLE_IMPLICIT) {
                next->state = TOML_TABLE_DOTTED;
            }
        }
        table = next;
    }
    nb_str final = *(nb_str *)nb_vec_at(keys, count - 1u);
    if (nb_map_has(table->as.table.entries, final)) {
        return toml_error(p, p->pos, "duplicate key");
    }
    if (!nb_map_put(table->as.table.entries, final, value, NULL)) {
        return toml_nomem(p);
    }
    return true;
}

static nb_toml_value *toml_last_array_item(nb_toml_value *array)
{
    size_t count = nb_vec_len(&array->as.array.items);
    if (count == 0) {
        return NULL;
    }
    return *(nb_toml_value **)nb_vec_at(&array->as.array.items, count - 1u);
}

static bool toml_resolve_header(toml_parser *p, const nb_vec *keys,
                                bool array_header)
{
    size_t count = nb_vec_len(keys);
    if (count == 0) {
        return toml_error(p, p->pos, "empty table name");
    }
    nb_toml_value *table = &p->doc->root;
    for (size_t i = 0; i + 1u < count; i++) {
        nb_str key = *(nb_str *)nb_vec_at(keys, i);
        nb_toml_value *existing = nb_map_get(table->as.table.entries, key);
        if (!existing) {
            nb_toml_value *child = toml_new_table(p, TOML_TABLE_IMPLICIT);
            if (!child) {
                return false;
            }
            if (!nb_map_put(table->as.table.entries, key, child, NULL)) {
                toml_value_cleanup(child);
                return toml_nomem(p);
            }
            existing = child;
        }
        nb_toml_value *next = existing;
        if (next->type == NB_TOML_ARRAY && next->array_of_tables) {
            next = toml_last_array_item(next);
            if (!next) {
                return toml_error(p, p->pos,
                                  "array of tables has no element");
            }
        }
        if (next->type != NB_TOML_TABLE) {
            return toml_error(p, p->pos,
                              "table header traverses a non-table value");
        }
        if (next->state == TOML_TABLE_INLINE ||
            next->state == TOML_TABLE_INLINE_DOTTED) {
            return toml_error(p, p->pos, "cannot extend an inline table");
        }
        table = next;
    }

    nb_str key = *(nb_str *)nb_vec_at(keys, count - 1u);
    nb_toml_value *existing = nb_map_get(table->as.table.entries, key);
    if (array_header) {
        nb_toml_value *array = existing;
        if (!array) {
            array = toml_new_array(p, true);
            if (!array) {
                return false;
            }
            if (!nb_map_put(table->as.table.entries, key, array, NULL)) {
                toml_value_cleanup(array);
                return toml_nomem(p);
            }
        } else if (array->type != NB_TOML_ARRAY ||
                   !array->array_of_tables) {
            return toml_error(p, p->pos,
                              "array table conflicts with an existing value");
        }
        nb_toml_value *element =
            toml_new_table(p, TOML_TABLE_ARRAY_ELEMENT);
        if (!element) {
            return false;
        }
        if (!nb_vec_push_copy(&array->as.array.items, &element)) {
            toml_value_cleanup(element);
            return toml_nomem(p);
        }
        p->current = element;
        return true;
    }

    if (!existing) {
        nb_toml_value *table_value = toml_new_table(p, TOML_TABLE_EXPLICIT);
        if (!table_value) {
            return false;
        }
        if (!nb_map_put(table->as.table.entries, key, table_value, NULL)) {
            toml_value_cleanup(table_value);
            return toml_nomem(p);
        }
        p->current = table_value;
        return true;
    }
    if (existing->type != NB_TOML_TABLE) {
        return toml_error(p, p->pos,
                          "table header conflicts with an existing value");
    }
    if (existing->state != TOML_TABLE_IMPLICIT) {
        return toml_error(p, p->pos, "table is already defined");
    }
    existing->state = TOML_TABLE_EXPLICIT;
    p->current = existing;
    return true;
}

static bool toml_parse_table_header(toml_parser *p)
{
    size_t start = p->pos;
    bool array_header = toml_peek_at(p, 1) == '[';
    p->pos += array_header ? 2u : 1u;

    nb_vec keys;
    nb_vec_init(&keys, sizeof(nb_str));
    bool ok = toml_parse_key_path(p, &keys);
    if (ok) {
        toml_skip_spaces(p);
        if (toml_peek(p) != ']') {
            toml_error(p, p->pos, "expected ']' after table name");
            ok = false;
        } else {
            p->pos++;
            if (array_header) {
                if (toml_peek(p) != ']') {
                    toml_error(p, p->pos,
                               "expected ']]' after array table name");
                    ok = false;
                } else {
                    p->pos++;
                }
            }
        }
    }
    if (ok) {
        ok = toml_resolve_header(p, &keys, array_header);
        if (ok) {
            toml_set_location(p, p->current, start, p->pos);
        }
    }
    nb_vec_destroy(&keys);
    return ok;
}

static bool toml_parse_key_value(toml_parser *p)
{
    nb_vec keys;
    nb_vec_init(&keys, sizeof(nb_str));
    bool ok = toml_parse_key_path(p, &keys);
    if (ok) {
        toml_skip_spaces(p);
        if (toml_peek(p) != '=') {
            toml_error(p, p->pos, "expected '=' after key");
            ok = false;
        }
    }
    if (ok) {
        p->pos++;
        nb_toml_value *value = toml_parse_value(p, 1u);
        if (!value) {
            ok = false;
        } else if (!toml_insert_dotted(p, p->current, &keys, value, false)) {
            toml_value_cleanup(value);
            ok = false;
        }
    }
    nb_vec_destroy(&keys);
    return ok;
}

static bool toml_parse_document(toml_parser *p)
{
    while (true) {
        toml_skip_spaces(p);
        if (toml_at_end(p)) {
            return true;
        }
        if (toml_peek(p) == '#') {
            if (!toml_skip_comment(p)) {
                return false;
            }
        }
        if (toml_newline_at(p)) {
            if (!toml_consume_newline(p)) {
                return false;
            }
            continue;
        }
        if (toml_at_end(p)) {
            return true;
        }

        bool ok;
        if (toml_peek(p) == '[') {
            ok = toml_parse_table_header(p);
        } else {
            ok = toml_parse_key_value(p);
        }
        if (!ok) {
            return false;
        }
        toml_skip_spaces(p);
        if (toml_peek(p) == '#') {
            if (!toml_skip_comment(p)) {
                return false;
            }
        }
        if (toml_at_end(p)) {
            return true;
        }
        if (!toml_newline_at(p)) {
            return toml_error(p, p->pos,
                              "expected newline or end of file");
        }
        if (!toml_consume_newline(p)) {
            return false;
        }
    }
}

static bool toml_datetime_start(const toml_parser *p)
{
    if (p->pos + 4u < p->text.len &&
        toml_is_digit(toml_peek_at(p, 0)) &&
        toml_is_digit(toml_peek_at(p, 1)) &&
        toml_is_digit(toml_peek_at(p, 2)) &&
        toml_is_digit(toml_peek_at(p, 3)) &&
        toml_peek_at(p, 4) == '-') {
        return true;
    }
    return p->pos + 2u < p->text.len &&
           toml_is_digit(toml_peek_at(p, 0)) &&
           toml_is_digit(toml_peek_at(p, 1)) &&
           toml_peek_at(p, 2) == ':';
}

static nb_toml_value *toml_parse_bool(toml_parser *p)
{
    bool value;
    if (toml_match(p, "true", 4u)) {
        value = true;
    } else if (toml_match(p, "false", 5u)) {
        value = false;
    } else {
        toml_error(p, p->pos, "invalid value");
        return NULL;
    }
    if (!toml_at_value_end(p)) {
        toml_error(p, p->pos, "unexpected text after boolean");
        return NULL;
    }
    nb_toml_value *node = toml_value_new(p, NB_TOML_BOOLEAN);
    if (node) {
        node->as.boolean = value;
    }
    return node;
}

static nb_toml_value *toml_parse_value_inner(toml_parser *p, size_t depth);

static nb_toml_value *toml_parse_value(toml_parser *p, size_t depth)
{
    toml_skip_spaces(p);
    size_t start = p->pos;
    nb_toml_value *value = toml_parse_value_inner(p, depth);
    if (value) {
        toml_set_location(p, value, start, p->pos);
    }
    return value;
}

static nb_toml_value *toml_parse_value_inner(toml_parser *p, size_t depth)
{
    toml_skip_spaces(p);
    if (toml_at_end(p)) {
        toml_error(p, p->pos, "expected a value");
        return NULL;
    }
    char c = toml_peek(p);
    if (c == '"' || c == '\'') {
        nb_str text;
        if (!toml_parse_quoted(p, c, true, &text)) {
            return NULL;
        }
        return toml_new_string(p, text.ptr, text.len);
    }
    if (c == '[') {
        return toml_parse_array(p, depth);
    }
    if (c == '{') {
        return toml_parse_inline_table(p, depth);
    }
    if (c == 't' || c == 'f') {
        return toml_parse_bool(p);
    }
    if (c == '+' || c == '-' || toml_is_digit(c)) {
        if (toml_datetime_start(p)) {
            return toml_parse_datetime(p);
        }
        return toml_parse_number(p);
    }
    if (c == 'i' || c == 'n') {
        return toml_parse_number(p);
    }
    toml_error(p, p->pos, "expected a value");
    return NULL;
}

static int toml_digit_value(char c, unsigned base)
{
    int value;
    if (c >= '0' && c <= '9') {
        value = c - '0';
    } else if (c >= 'a' && c <= 'f') {
        value = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        value = c - 'A' + 10;
    } else {
        return -1;
    }
    return (unsigned)value < base ? value : -1;
}

static bool toml_scan_uint(nb_str token, size_t *index, unsigned base,
                           bool allow_leading_zero, uint64_t *out)
{
    size_t i = *index;
    uint64_t value = 0;
    bool saw_digit = false;
    bool last_digit = false;
    size_t digits = 0;
    size_t first_digit = i;
    while (i < token.len) {
        char c = token.ptr[i];
        if (c == '_') {
            if (!last_digit || i + 1u >= token.len ||
                toml_digit_value(token.ptr[i + 1u], base) < 0) {
                return false;
            }
            last_digit = false;
            i++;
            continue;
        }
        int digit = toml_digit_value(c, base);
        if (digit < 0) {
            break;
        }
        if (!saw_digit) {
            first_digit = i;
        }
        saw_digit = true;
        digits++;
        if (value > (UINT64_MAX - (uint64_t)digit) / base) {
            return false;
        }
        value = value * base + (uint64_t)digit;
        last_digit = true;
        i++;
    }
    if (!saw_digit) {
        return false;
    }
    if (!allow_leading_zero && digits > 1u &&
        token.ptr[first_digit] == '0') {
        return false;
    }
    *index = i;
    *out = value;
    return true;
}

static bool toml_scan_float_digits(nb_str token, size_t *index,
                                   bool *saw_digit)
{
    size_t i = *index;
    bool last_digit = false;
    bool saw = false;
    while (i < token.len) {
        char c = token.ptr[i];
        if (c == '_') {
            if (!last_digit || i + 1u >= token.len ||
                !toml_is_digit(token.ptr[i + 1u])) {
                return false;
            }
            last_digit = false;
            i++;
            continue;
        }
        if (!toml_is_digit(c)) {
            break;
        }
        saw = true;
        last_digit = true;
        i++;
    }
    if (!saw) {
        return false;
    }
    *index = i;
    *saw_digit = true;
    return true;
}

static bool toml_float_token_is_special(nb_str token, bool *negative,
                                        bool *is_nan)
{
    size_t i = 0;
    bool neg = false;
    if (token.len != 0 &&
        (token.ptr[0] == '+' || token.ptr[0] == '-')) {
        neg = token.ptr[0] == '-';
        i = 1;
    }
    if (token.len - i == 3u &&
        memcmp(token.ptr + i, "inf", 3u) == 0) {
        *negative = neg;
        *is_nan = false;
        return true;
    }
    if (token.len - i == 3u &&
        memcmp(token.ptr + i, "nan", 3u) == 0) {
        *negative = neg;
        *is_nan = true;
        return true;
    }
    return false;
}

static bool toml_parse_float_token(toml_parser *p, nb_str token,
                                   double *out)
{
    bool negative = false;
    bool is_nan = false;
    if (toml_float_token_is_special(token, &negative, &is_nan)) {
        if (is_nan) {
            *out = negative ? -NAN : NAN;
        } else {
            *out = negative ? -INFINITY : INFINITY;
        }
        return true;
    }

    size_t i = 0;
    if (token.len != 0 &&
        (token.ptr[0] == '+' || token.ptr[0] == '-')) {
        i = 1;
    }
    size_t integer_start = i;
    bool saw_integer = false;
    if (!toml_scan_float_digits(token, &i, &saw_integer)) {
        return toml_error(p, p->pos, "invalid float");
    }
    size_t digits = 0;
    for (size_t j = integer_start; j < i; j++) {
        if (token.ptr[j] != '_') {
            digits++;
        }
    }
    if (digits > 1u && token.ptr[integer_start] == '0') {
        return toml_error(p, p->pos, "leading zero in float");
    }

    bool has_fraction = false;
    bool has_exponent = false;
    if (i < token.len && token.ptr[i] == '.') {
        has_fraction = true;
        i++;
        bool saw_fraction = false;
        if (!toml_scan_float_digits(token, &i, &saw_fraction)) {
            return toml_error(p, p->pos, "invalid fractional part");
        }
    }
    if (i < token.len &&
        (token.ptr[i] == 'e' || token.ptr[i] == 'E')) {
        has_exponent = true;
        i++;
        if (i < token.len &&
            (token.ptr[i] == '+' || token.ptr[i] == '-')) {
            i++;
        }
        bool saw_exponent = false;
        if (!toml_scan_float_digits(token, &i, &saw_exponent)) {
            return toml_error(p, p->pos, "invalid exponent");
        }
    }
    if (i != token.len || (!has_fraction && !has_exponent)) {
        return toml_error(p, p->pos, "invalid float");
    }

    nb_buf clean;
    nb_buf_init(&clean);
    const char *decimal = localeconv()->decimal_point;
    if (!decimal || decimal[0] == '\0') {
        decimal = ".";
    }
    size_t decimal_len = strlen(decimal);
    for (size_t j = 0; j < token.len; j++) {
        if (token.ptr[j] == '_') {
            continue;
        }
        bool appended = token.ptr[j] == '.'
                            ? nb_buf_append(&clean, decimal, decimal_len)
                            : nb_buf_append_char(&clean, token.ptr[j]);
        if (!appended) {
            nb_buf_destroy(&clean);
            return toml_nomem(p);
        }
    }
    char *end = NULL;
    double value = strtod(nb_buf_cstr(&clean), &end);
    bool ok = end && *end == '\0' &&
              (size_t)(end - nb_buf_cstr(&clean)) == nb_buf_len(&clean);
    nb_buf_destroy(&clean);
    if (!ok) {
        return toml_error(p, p->pos, "invalid float");
    }
    *out = value;
    return true;
}

static nb_toml_value *toml_parse_integer_token(toml_parser *p, nb_str token)
{
    size_t i = 0;
    bool negative = false;
    if (token.len != 0 &&
        (token.ptr[0] == '+' || token.ptr[0] == '-')) {
        negative = token.ptr[0] == '-';
        i = 1;
        if (i == token.len) {
            toml_error(p, p->pos, "invalid integer");
            return NULL;
        }
    }

    unsigned base = 10;
    bool allow_leading_zero = false;
    bool has_sign = i != 0;
    if (i + 1u < token.len && token.ptr[i] == '0') {
        char prefix = token.ptr[i + 1u];
        if (prefix == 'x' || prefix == 'o' || prefix == 'b') {
            if (has_sign) {
                toml_error(p, p->pos,
                           "sign is not allowed on a based integer");
                return NULL;
            }
            base = prefix == 'x' ? 16u : (prefix == 'o' ? 8u : 2u);
            allow_leading_zero = true;
            i += 2u;
        }
    }

    uint64_t magnitude = 0;
    if (!toml_scan_uint(token, &i, base, allow_leading_zero, &magnitude) ||
        i != token.len) {
        toml_error(p, p->pos, "invalid integer");
        return NULL;
    }
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u
                              : (uint64_t)INT64_MAX;
    if (magnitude > limit) {
        toml_error(p, p->pos, "integer is out of range");
        return NULL;
    }

    int64_t value;
    if (negative) {
        if (magnitude == (uint64_t)INT64_MAX + 1u) {
            value = INT64_MIN;
        } else {
            value = -(int64_t)magnitude;
        }
    } else {
        value = (int64_t)magnitude;
    }
    nb_toml_value *node = toml_value_new(p, NB_TOML_INTEGER);
    if (node) {
        node->as.integer = value;
    }
    return node;
}

static nb_toml_value *toml_parse_number(toml_parser *p)
{
    size_t start = p->pos;
    while (!toml_at_end(p)) {
        char c = toml_peek(p);
        if (!(toml_is_digit(c) || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '_' || c == '+' ||
              c == '-' || c == '.')) {
            break;
        }
        p->pos++;
    }
    if (p->pos == start) {
        toml_error(p, p->pos, "expected a number");
        return NULL;
    }
    nb_str token = nb_str_from_parts(p->text.ptr + start, p->pos - start);

    size_t i = 0;
    if (token.len != 0 &&
        (token.ptr[0] == '+' || token.ptr[0] == '-')) {
        i = 1;
    }
    bool based = i + 1u < token.len && token.ptr[i] == '0' &&
                 (token.ptr[i + 1u] == 'x' ||
                  token.ptr[i + 1u] == 'o' ||
                  token.ptr[i + 1u] == 'b');
    if (based) {
        return toml_parse_integer_token(p, token);
    }

    bool special_negative = false;
    bool special_nan = false;
    if (toml_float_token_is_special(token, &special_negative,
                                    &special_nan)) {
        double value;
        if (!toml_parse_float_token(p, token, &value)) {
            return NULL;
        }
        nb_toml_value *node = toml_value_new(p, NB_TOML_FLOAT);
        if (node) {
            node->as.floating = value;
        }
        return node;
    }

    bool is_float = false;
    for (size_t j = 0; j < token.len; j++) {
        if (token.ptr[j] == '.' || token.ptr[j] == 'e' ||
            token.ptr[j] == 'E') {
            is_float = true;
            break;
        }
    }
    if (is_float) {
        double value;
        if (!toml_parse_float_token(p, token, &value)) {
            return NULL;
        }
        nb_toml_value *node = toml_value_new(p, NB_TOML_FLOAT);
        if (node) {
            node->as.floating = value;
        }
        return node;
    }
    return toml_parse_integer_token(p, token);
}

static bool toml_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int toml_days_in_month(int year, int month)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30,
                                31, 31, 30, 31, 30, 31 };
    if (month == 2 && toml_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

static bool toml_read_digits(const toml_parser *p, size_t offset,
                             size_t count, int *out)
{
    if (p->pos + offset + count > p->text.len) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; i < count; i++) {
        char c = p->text.ptr[p->pos + offset + i];
        if (!toml_is_digit(c)) {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out = value;
    return true;
}

static bool toml_parse_time_fields(toml_parser *p, int *hour, int *minute,
                                   int *second, uint32_t *nanosecond)
{
    int h = 0;
    int m = 0;
    int s = 0;
    if (!toml_read_digits(p, 0, 2, &h) || toml_peek_at(p, 2) != ':' ||
        !toml_read_digits(p, 3, 2, &m)) {
        return toml_error(p, p->pos, "expected HH:MM");
    }
    if (h > 23 || m > 59) {
        return toml_error(p, p->pos, "time field is out of range");
    }
    p->pos += 5u;

    uint32_t ns = 0;
    if (toml_peek(p) == ':') {
        p->pos++;
        if (!toml_read_digits(p, 0, 2, &s)) {
            return toml_error(p, p->pos, "expected seconds");
        }
        if (s > 60) {
            return toml_error(p, p->pos, "time field is out of range");
        }
        p->pos += 2u;
        if (toml_peek(p) == '.') {
            p->pos++;
            size_t digits = 0;
            while (toml_is_digit(toml_peek(p))) {
                if (digits < 9u) {
                    ns = ns * 10u + (uint32_t)(toml_peek(p) - '0');
                }
                digits++;
                p->pos++;
            }
            if (digits == 0) {
                return toml_error(
                    p, p->pos,
                    "fractional seconds need at least one digit");
            }
            while (digits < 9u) {
                ns *= 10u;
                digits++;
            }
        }
    }
    *hour = h;
    *minute = m;
    *second = s;
    *nanosecond = ns;
    return true;
}

static nb_toml_value *toml_parse_datetime(toml_parser *p)
{
    bool has_date = false;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    uint32_t nanosecond = 0;

    if (toml_read_digits(p, 0, 4, &year) && toml_peek_at(p, 4) == '-') {
        has_date = true;
        if (!toml_read_digits(p, 5, 2, &month) ||
            toml_peek_at(p, 7) != '-' ||
            !toml_read_digits(p, 8, 2, &day)) {
            toml_error(p, p->pos, "invalid date");
            return NULL;
        }
        if (month < 1 || month > 12 || day < 1 ||
            day > toml_days_in_month(year, month)) {
            toml_error(p, p->pos, "date field is out of range");
            return NULL;
        }
        p->pos += 10u;
    }

    bool has_time = false;
    if (has_date) {
        char separator = toml_peek(p);
        if (separator == 'T' || separator == 't') {
            p->pos++;
            has_time = true;
        } else if (separator == ' ' && toml_peek_at(p, 1) != '\0' &&
                   toml_is_digit(toml_peek_at(p, 1)) &&
                   toml_is_digit(toml_peek_at(p, 2)) &&
                   toml_peek_at(p, 3) == ':') {
            p->pos++;
            has_time = true;
        }
    } else {
        has_time = true;
    }

    if (has_time &&
        !toml_parse_time_fields(p, &hour, &minute, &second, &nanosecond)) {
        return NULL;
    }

    bool has_offset = false;
    int offset_minutes = 0;
    char c = toml_peek(p);
    if (c == 'Z' || c == 'z') {
        p->pos++;
        has_offset = true;
    } else if (c == '+' || c == '-') {
        int offset_hour = 0;
        int offset_minute = 0;
        if (!toml_read_digits(p, 1, 2, &offset_hour) ||
            toml_peek_at(p, 3) != ':' ||
            !toml_read_digits(p, 4, 2, &offset_minute)) {
            toml_error(p, p->pos, "invalid time offset");
            return NULL;
        }
        if (offset_hour > 23 || offset_minute > 59) {
            toml_error(p, p->pos, "time offset is out of range");
            return NULL;
        }
        offset_minutes = offset_hour * 60 + offset_minute;
        if (c == '-') {
            offset_minutes = -offset_minutes;
        }
        p->pos += 6u;
        has_offset = true;
    }

    if (!has_date && has_offset) {
        toml_error(p, p->pos, "a local time cannot have an offset");
        return NULL;
    }
    if (has_date && !has_time && has_offset) {
        toml_error(p, p->pos, "a local date cannot have an offset");
        return NULL;
    }

    nb_toml_value *node = toml_value_new(p, NB_TOML_DATETIME);
    if (!node) {
        return NULL;
    }
    nb_toml_datetime *dt = &node->as.datetime;
    memset(dt, 0, sizeof(*dt));
    if (!has_date) {
        dt->kind = NB_TOML_LOCAL_TIME;
    } else if (!has_time) {
        dt->kind = NB_TOML_LOCAL_DATE;
    } else if (has_offset) {
        dt->kind = NB_TOML_OFFSET_DATETIME;
    } else {
        dt->kind = NB_TOML_LOCAL_DATETIME;
    }
    dt->year = year;
    dt->month = month;
    dt->day = day;
    dt->hour = hour;
    dt->minute = minute;
    dt->second = second;
    dt->nanosecond = nanosecond;
    dt->offset_minutes = offset_minutes;
    return node;
}

static bool toml_read_hex(toml_parser *p, size_t count, uint32_t *out)
{
    if (p->pos + count > p->text.len) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < count; i++) {
        char c = p->text.ptr[p->pos + i];
        if (!toml_is_hex(c)) {
            return false;
        }
        int digit = toml_digit_value(c, 16u);
        value = (value << 4u) | (uint32_t)digit;
    }
    p->pos += count;
    *out = value;
    return true;
}

/* Returns 1 on success, 0 for an invalid scalar value, -1 for allocation
 * failure. The caller must keep NOMEM distinct from a syntax error. */
static int toml_append_codepoint(nb_buf *out, uint32_t codepoint)
{
    char encoded[4];
    size_t count = nb_utf8_encode(codepoint, encoded);
    if (count == 0) {
        return 0;
    }
    return nb_buf_append(out, encoded, count) ? 1 : -1;
}

static bool toml_parse_escape(toml_parser *p, nb_buf *out)
{
    p->pos++;
    if (toml_at_end(p)) {
        return toml_error(p, p->pos, "unterminated escape sequence");
    }
    char c = toml_peek(p);
    p->pos++;
    switch (c) {
    case 'b':
        return nb_buf_append_char(out, '\b') || toml_nomem(p);
    case 't':
        return nb_buf_append_char(out, '\t') || toml_nomem(p);
    case 'n':
        return nb_buf_append_char(out, '\n') || toml_nomem(p);
    case 'f':
        return nb_buf_append_char(out, '\f') || toml_nomem(p);
    case 'r':
        return nb_buf_append_char(out, '\r') || toml_nomem(p);
    case 'e':
        return nb_buf_append_char(out, '\x1b') || toml_nomem(p);
    case '"':
        return nb_buf_append_char(out, '"') || toml_nomem(p);
    case '\\':
        return nb_buf_append_char(out, '\\') || toml_nomem(p);
    case 'x': {
        uint32_t codepoint = 0;
        if (!toml_read_hex(p, 2, &codepoint)) {
            return toml_error(p, p->pos, "invalid hex escape");
        }
        int appended = toml_append_codepoint(out, codepoint);
        if (appended == 0) {
            return toml_error(p, p->pos,
                              "escape is not a Unicode scalar value");
        }
        if (appended < 0) {
            return toml_nomem(p);
        }
        return true;
    }
    case 'u':
    case 'U': {
        uint32_t codepoint = 0;
        size_t count = c == 'u' ? 4u : 8u;
        if (!toml_read_hex(p, count, &codepoint)) {
            return toml_error(p, p->pos, "invalid Unicode escape");
        }
        int appended = toml_append_codepoint(out, codepoint);
        if (appended == 0) {
            return toml_error(p, p->pos,
                              "escape is not a Unicode scalar value");
        }
        if (appended < 0) {
            return toml_nomem(p);
        }
        return true;
    }
    default:
        return toml_error(p, p->pos - 1u, "unknown escape sequence");
    }
}

static void toml_seal_value(nb_toml_value *value)
{
    if (!value) {
        return;
    }
    if (value->type == NB_TOML_TABLE) {
        if (value->state == TOML_TABLE_INLINE_DOTTED) {
            value->state = TOML_TABLE_INLINE;
        }
        nb_map_iter iter;
        nb_str key;
        void *item = NULL;
        nb_map_iter_init(value->as.table.entries, &iter);
        while (nb_map_iter_next(&iter, &key, &item)) {
            toml_seal_value(item);
        }
    } else if (value->type == NB_TOML_ARRAY) {
        for (size_t i = 0; i < nb_vec_len(&value->as.array.items); i++) {
            toml_seal_value(
                *(nb_toml_value **)nb_vec_at(&value->as.array.items, i));
        }
    }
}

static bool toml_parse_quoted(toml_parser *p, char quote,
                              bool allow_multiline, nb_str *out)
{
    bool multiline = allow_multiline && toml_peek_at(p, 1) == quote &&
                     toml_peek_at(p, 2) == quote;
    if (!allow_multiline && toml_peek_at(p, 1) == quote &&
        toml_peek_at(p, 2) == quote) {
        return toml_error(p, p->pos,
                          "multiline strings are not allowed in keys");
    }
    if (toml_peek(p) != quote) {
        return toml_error(p, p->pos, "expected a quoted string");
    }
    p->pos += multiline ? 3u : 1u;
    if (multiline) {
        if (toml_peek(p) == '\n') {
            p->pos++;
        } else if (toml_peek(p) == '\r') {
            if (toml_peek_at(p, 1) != '\n') {
                return toml_error(p, p->pos, "bare carriage return");
            }
            p->pos += 2u;
        }
    }

    nb_buf buffer;
    nb_buf_init(&buffer);
    bool basic = quote == '"';
    bool closed = false;
    while (!toml_at_end(p)) {
        char c = toml_peek(p);
        if (c == quote) {
            if (!multiline) {
                p->pos++;
                closed = true;
                break;
            }
            size_t run = 0;
            while (toml_peek_at(p, run) == quote) {
                run++;
            }
            if (run > 5u) {
                nb_buf_destroy(&buffer);
                return toml_error(p, p->pos,
                                  "too many consecutive quotes");
            }
            if (run >= 3u) {
                for (size_t i = 0; i + 3u < run; i++) {
                    if (!nb_buf_append_char(&buffer, quote)) {
                        nb_buf_destroy(&buffer);
                        return toml_nomem(p);
                    }
                }
                p->pos += run;
                closed = true;
                break;
            }
            for (size_t i = 0; i < run; i++) {
                if (!nb_buf_append_char(&buffer, quote)) {
                    nb_buf_destroy(&buffer);
                    return toml_nomem(p);
                }
            }
            p->pos += run;
            continue;
        }

        if (basic && c == '\\') {
            if (multiline) {
                size_t next = p->pos + 1u;
                while (next < p->text.len &&
                       toml_is_space(p->text.ptr[next])) {
                    next++;
                }
                bool newline =
                    next < p->text.len &&
                    (p->text.ptr[next] == '\n' ||
                     (p->text.ptr[next] == '\r' &&
                      next + 1u < p->text.len &&
                      p->text.ptr[next + 1u] == '\n'));
                if (newline) {
                    p->pos = next;
                    if (!toml_consume_newline(p)) {
                        nb_buf_destroy(&buffer);
                        return false;
                    }
                    while (!toml_at_end(p)) {
                        if (toml_is_space(toml_peek(p))) {
                            p->pos++;
                        } else if (toml_peek(p) == '\n') {
                            p->pos++;
                        } else if (toml_peek(p) == '\r' &&
                                   toml_peek_at(p, 1) == '\n') {
                            p->pos += 2u;
                        } else {
                            break;
                        }
                    }
                    continue;
                }
            }
            if (!toml_parse_escape(p, &buffer)) {
                nb_buf_destroy(&buffer);
                return false;
            }
            continue;
        }

        if (c == '\n') {
            if (!multiline) {
                nb_buf_destroy(&buffer);
                return toml_error(p, p->pos,
                                  "newline in single-line string");
            }
            if (!nb_buf_append_char(&buffer, '\n')) {
                nb_buf_destroy(&buffer);
                return toml_nomem(p);
            }
            p->pos++;
            continue;
        }
        if (c == '\r') {
            if (!multiline || toml_peek_at(p, 1) != '\n') {
                nb_buf_destroy(&buffer);
                return toml_error(p, p->pos, "bare carriage return");
            }
            if (!nb_buf_append_char(&buffer, '\n')) {
                nb_buf_destroy(&buffer);
                return toml_nomem(p);
            }
            p->pos += 2u;
            continue;
        }
        unsigned char byte = (unsigned char)c;
        if ((byte < 0x20u && byte != '\t') || byte == 0x7fu) {
            nb_buf_destroy(&buffer);
            return toml_error(p, p->pos, "control character in string");
        }
        if (!nb_buf_append_char(&buffer, c)) {
            nb_buf_destroy(&buffer);
            return toml_nomem(p);
        }
        p->pos++;
    }

    if (!closed) {
        nb_buf_destroy(&buffer);
        return toml_error(p, p->pos, "unterminated string");
    }
    nb_str text = nb_buf_str(&buffer);
    bool ok = toml_dup_string(p, text.ptr, text.len, out);
    nb_buf_destroy(&buffer);
    return ok;
}

static bool toml_skip_array_ws(toml_parser *p)
{
    while (true) {
        toml_skip_spaces(p);
        if (toml_peek(p) == '#') {
            if (!toml_skip_comment(p)) {
                return false;
            }
        }
        if (toml_peek(p) == '\n') {
            p->pos++;
            continue;
        }
        if (toml_peek(p) == '\r') {
            if (toml_peek_at(p, 1) != '\n') {
                return toml_error(p, p->pos, "bare carriage return");
            }
            p->pos += 2u;
            continue;
        }
        return true;
    }
}

static nb_toml_value *toml_parse_array(toml_parser *p, size_t depth)
{
    if (depth > TOML_MAX_DEPTH) {
        toml_error(p, p->pos, "nesting is too deep");
        return NULL;
    }
    p->pos++;
    nb_toml_value *array = toml_new_array(p, false);
    if (!array) {
        return NULL;
    }
    while (true) {
        if (!toml_skip_array_ws(p)) {
            toml_value_cleanup(array);
            return NULL;
        }
        if (toml_peek(p) == ']') {
            p->pos++;
            return array;
        }
        if (toml_at_end(p)) {
            toml_value_cleanup(array);
            toml_error(p, p->pos, "unterminated array");
            return NULL;
        }
        nb_toml_value *value = toml_parse_value(p, depth + 1u);
        if (!value) {
            toml_value_cleanup(array);
            return NULL;
        }
        if (!nb_vec_push_copy(&array->as.array.items, &value)) {
            toml_value_cleanup(value);
            toml_value_cleanup(array);
            toml_nomem(p);
            return NULL;
        }
        if (!toml_skip_array_ws(p)) {
            toml_value_cleanup(array);
            return NULL;
        }
        if (toml_peek(p) == ',') {
            p->pos++;
            continue;
        }
        if (toml_peek(p) == ']') {
            p->pos++;
            return array;
        }
        toml_value_cleanup(array);
        toml_error(p, p->pos, "expected ',' or ']' in array");
        return NULL;
    }
}

static bool toml_skip_inline_ws(toml_parser *p)
{
    return toml_skip_array_ws(p);
}

static nb_toml_value *toml_parse_inline_table(toml_parser *p, size_t depth)
{
    if (depth > TOML_MAX_DEPTH) {
        toml_error(p, p->pos, "nesting is too deep");
        return NULL;
    }
    p->pos++;
    nb_toml_value *table = toml_new_table(p, TOML_TABLE_INLINE);
    if (!table) {
        return NULL;
    }
    if (!toml_skip_inline_ws(p)) {
        toml_value_cleanup(table);
        return NULL;
    }
    if (toml_peek(p) == '}') {
        p->pos++;
        toml_seal_value(table);
        return table;
    }

    while (true) {
        nb_vec keys;
        nb_vec_init(&keys, sizeof(nb_str));
        if (!toml_parse_key_path(p, &keys)) {
            nb_vec_destroy(&keys);
            toml_value_cleanup(table);
            return NULL;
        }
        toml_skip_spaces(p);
        if (toml_peek(p) != '=') {
            nb_vec_destroy(&keys);
            toml_value_cleanup(table);
            toml_error(p, p->pos, "expected '=' in inline table");
            return NULL;
        }
        p->pos++;
        nb_toml_value *value = toml_parse_value(p, depth + 1u);
        if (!value) {
            nb_vec_destroy(&keys);
            toml_value_cleanup(table);
            return NULL;
        }
        if (!toml_insert_dotted(p, table, &keys, value, true)) {
            nb_vec_destroy(&keys);
            toml_value_cleanup(value);
            toml_value_cleanup(table);
            return NULL;
        }
        nb_vec_destroy(&keys);
        if (!toml_skip_inline_ws(p)) {
            toml_value_cleanup(table);
            return NULL;
        }
        if (toml_peek(p) == ',') {
            p->pos++;
            if (!toml_skip_inline_ws(p)) {
                toml_value_cleanup(table);
                return NULL;
            }
            if (toml_peek(p) == '}') {
                p->pos++;
                toml_seal_value(table);
                return table;
            }
            continue;
        }
        if (toml_peek(p) == '}') {
            p->pos++;
            toml_seal_value(table);
            return table;
        }
        toml_value_cleanup(table);
        toml_error(p, p->pos, "expected ',' or '}' in inline table");
        return NULL;
    }
}

nb_toml_location nb_toml_location_of(const nb_toml_value *value)
{
    nb_toml_location empty = {0};
    return value ? value->location : empty;
}

nb_str nb_toml_string(const nb_toml_value *value)
{
    nb_str out = NB_STR_NULL;
    nb_toml_get_string(value, &out);
    return out;
}

int64_t nb_toml_integer(const nb_toml_value *value)
{
    int64_t out = 0;
    nb_toml_get_integer(value, &out);
    return out;
}

double nb_toml_float(const nb_toml_value *value)
{
    double out = 0;
    nb_toml_get_float(value, &out);
    return out;
}

bool nb_toml_boolean(const nb_toml_value *value)
{
    bool out = false;
    nb_toml_get_bool(value, &out);
    return out;
}

nb_str nb_toml_table_key_at(const nb_toml_value *table, size_t index)
{
    nb_toml_table_iter iter;
    nb_toml_table_iter_init(table, &iter);
    nb_str key = NB_STR_NULL;
    const nb_toml_value *value = NULL;
    size_t at = 0;
    while (nb_toml_table_iter_next(&iter, &key, &value)) {
        if (at++ == index) {
            return key;
        }
    }
    return NB_STR_NULL;
}

const nb_toml_value *nb_toml_table_value_at(const nb_toml_value *table, size_t index)
{
    nb_toml_table_iter iter;
    nb_toml_table_iter_init(table, &iter);
    nb_str key;
    const nb_toml_value *value = NULL;
    size_t at = 0;
    while (nb_toml_table_iter_next(&iter, &key, &value)) {
        if (at++ == index) {
            return value;
        }
    }
    return NULL;
}

bool nb_toml_value_equal(const nb_toml_value *a, const nb_toml_value *b)
{
    if (!a || !b) {
        return a == b;
    }
    if (a->type != b->type) {
        return false;
    }
    switch (a->type) {
    case NB_TOML_STRING:
        return nb_str_eq(nb_toml_string(a), nb_toml_string(b));
    case NB_TOML_INTEGER:
        return a->as.integer == b->as.integer;
    case NB_TOML_FLOAT:
        return a->as.floating == b->as.floating || (isnan(a->as.floating) && isnan(b->as.floating));
    case NB_TOML_BOOLEAN:
        return a->as.boolean == b->as.boolean;
    case NB_TOML_DATETIME: {
        const nb_toml_datetime *x = &a->as.datetime;
        const nb_toml_datetime *y = &b->as.datetime;
        return x->kind == y->kind && x->year == y->year && x->month == y->month &&
               x->day == y->day && x->hour == y->hour && x->minute == y->minute &&
               x->second == y->second && x->nanosecond == y->nanosecond &&
               x->offset_minutes == y->offset_minutes;
    }
    case NB_TOML_ARRAY:
        if (nb_toml_array_size(a) != nb_toml_array_size(b)) {
            return false;
        }
        for (size_t i = 0; i < nb_toml_array_size(a); i++) {
            if (!nb_toml_value_equal(nb_toml_array_get(a, i), nb_toml_array_get(b, i))) {
                return false;
            }
        }
        return true;
    case NB_TOML_TABLE: {
        if (nb_toml_table_size(a) != nb_toml_table_size(b)) {
            return false;
        }
        nb_toml_table_iter iter;
        nb_toml_table_iter_init(a, &iter);
        nb_str key;
        const nb_toml_value *value;
        while (nb_toml_table_iter_next(&iter, &key, &value)) {
            if (!nb_toml_value_equal(value, nb_toml_table_get(b, key))) {
                return false;
            }
        }
        return true;
    }
    default:
        return true;
    }
}
