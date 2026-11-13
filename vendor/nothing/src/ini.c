#include "nb/ini.h"

#include "nb/file.h"
#include "nb/mem.h"
#include "nb/utf8.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

struct nb_ini {
    nb_map *sections;
};

/* Values keep their exact length in a prefix header so the owning map entry
 * can be released with the same size it was allocated with. */
typedef struct nb_ini_value {
    size_t len;
    char data[];
} nb_ini_value;

static nb_ini_value *ini_value_header(const void *data)
{
    return (nb_ini_value *)((char *)data - offsetof(nb_ini_value, data));
}

static char *ini_value_new(nb_str value)
{
    size_t header = offsetof(nb_ini_value, data);
    if (value.len > SIZE_MAX - header - 1u) {
        return NULL;
    }
    nb_ini_value *v = nb_try_alloc(header + value.len + 1u);
    if (!v) {
        return NULL;
    }
    v->len = value.len;
    if (value.len != 0) {
        memcpy(v->data, value.ptr, value.len);
    }
    v->data[value.len] = '\0';
    return v->data;
}

static void ini_value_free(void *data)
{
    if (!data) {
        return;
    }
    nb_ini_value *v = ini_value_header(data);
    nb_free(v, offsetof(nb_ini_value, data) + v->len + 1u);
}

static nb_str ini_value_str(const void *data)
{
    if (!data) {
        return NB_STR_NULL;
    }
    const nb_ini_value *v = ini_value_header(data);
    return nb_str_from_parts(v->data, v->len);
}

static nb_str section_name(const char *section)
{
    return section ? nb_str_from_cstr(section) : NB_STR_NULL;
}

static nb_map *section_lookup(const nb_ini *ini, nb_str section, bool create)
{
    if (!ini) {
        return NULL;
    }
    nb_map *map = nb_map_get(ini->sections, section);
    if (map || !create) {
        return map;
    }
    map = nb_map_new();
    if (!map) {
        return NULL;
    }
    if (!nb_map_put(ini->sections, section, map, NULL)) {
        nb_map_free(map);
        return NULL;
    }
    return map;
}

static bool set_str(nb_ini *ini, nb_str section, nb_str key, nb_str value)
{
    if (!ini || key.len == 0 || (!key.ptr) || (value.len != 0 && !value.ptr)) {
        return false;
    }
    char *copy = ini_value_new(value);
    if (!copy) {
        return false;
    }

    nb_map *map = section_lookup(ini, section, false);
    bool created = map == NULL;
    if (created) {
        map = nb_map_new();
    }
    if (!map) {
        ini_value_free(copy);
        return false;
    }
    void *old = NULL;
    if (!nb_map_put(map, key, copy, &old)) {
        ini_value_free(copy);
        if (created) {
            nb_map_free(map);
        }
        return false;
    }
    if (created && !nb_map_put(ini->sections, section, map, NULL)) {
        ini_value_free(copy);
        nb_map_free(map);
        return false;
    }
    if (old) {
        ini_value_free(old);
    }
    return true;
}

nb_ini *nb_ini_new(void)
{
    nb_ini *ini = nb_try_alloc(sizeof(*ini));
    if (!ini) {
        return NULL;
    }
    ini->sections = nb_map_new();
    if (!ini->sections) {
        nb_free(ini, sizeof(*ini));
        return NULL;
    }
    return ini;
}

void nb_ini_free(nb_ini *ini)
{
    if (!ini) {
        return;
    }
    nb_map_iter it;
    nb_str section;
    void *value;
    nb_map_iter_init(ini->sections, &it);
    while (nb_map_iter_next(&it, &section, &value)) {
        nb_map *map = value;
        nb_map_iter entry_it;
        nb_str key;
        void *entry_value;
        nb_map_iter_init(map, &entry_it);
        while (nb_map_iter_next(&entry_it, &key, &entry_value)) {
            ini_value_free(entry_value);
        }
        nb_map_free(map);
    }
    nb_map_free(ini->sections);
    nb_free(ini, sizeof(*ini));
}

static bool ini_is_space(unsigned char c)
{
    return isspace(c) != 0;
}

static bool ini_is_comment(unsigned char c)
{
    return c == ';' || c == '#';
}

/* Cuts an unquoted value at the first comment that is preceded by whitespace,
 * then removes trailing whitespace. space_before covers the case where the
 * comment is the first character of the value. */
static nb_str ini_unquoted_value(nb_str raw, bool space_before)
{
    size_t end = raw.len;
    for (size_t i = 0; i < raw.len; i++) {
        unsigned char c = (unsigned char)raw.ptr[i];
        if (ini_is_comment(c) &&
            ((i == 0 && space_before) ||
             (i != 0 && ini_is_space((unsigned char)raw.ptr[i - 1u])))) {
            end = i;
            break;
        }
    }
    return nb_str_rtrim(nb_str_from_parts(raw.ptr, end));
}

/* Decodes one quoted value into scratch. raw starts at the opening quote. */
static nb_status ini_parse_quoted(nb_str raw, nb_buf *scratch, nb_str *value,
                                  size_t line_number, nb_err *err)
{
    char quote = raw.ptr[0];
    size_t i = 1u;

    nb_buf_clear(scratch);
    while (i < raw.len) {
        unsigned char c = (unsigned char)raw.ptr[i];
        if (c == (unsigned char)quote) {
            size_t tail = i + 1u;
            bool separated = false;
            while (tail < raw.len &&
                   ini_is_space((unsigned char)raw.ptr[tail])) {
                tail++;
                separated = true;
            }
            if (tail < raw.len &&
                (!separated ||
                 !ini_is_comment((unsigned char)raw.ptr[tail]))) {
                nb_err_set(err, NB_ERR_PARSE,
                           "INI line %zu: unexpected text after quoted value",
                           line_number);
                return NB_ERR_PARSE;
            }
            *value = nb_buf_str(scratch);
            return NB_OK;
        }
        if (c == '\\') {
            if (i + 1u >= raw.len) {
                nb_err_set(err, NB_ERR_PARSE,
                           "INI line %zu: unterminated quoted value",
                           line_number);
                return NB_ERR_PARSE;
            }
            char escaped = raw.ptr[i + 1u];
            char decoded;
            bool known = true;
            switch (escaped) {
            case '\\': decoded = '\\'; break;
            case '"': decoded = '"'; break;
            case '\'': decoded = '\''; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            default:
                decoded = '\0';
                known = false;
                break;
            }
            if (known) {
                if (!nb_buf_append_char(scratch, decoded)) {
                    nb_err_set(err, NB_ERR_NOMEM,
                               "INI line %zu: out of memory", line_number);
                    return NB_ERR_NOMEM;
                }
            } else if (!nb_buf_append_char(scratch, '\\') ||
                       !nb_buf_append_char(scratch, escaped)) {
                nb_err_set(err, NB_ERR_NOMEM,
                           "INI line %zu: out of memory", line_number);
                return NB_ERR_NOMEM;
            }
            i += 2u;
            continue;
        }
        if (!nb_buf_append_char(scratch, (char)c)) {
            nb_err_set(err, NB_ERR_NOMEM, "INI line %zu: out of memory",
                       line_number);
            return NB_ERR_NOMEM;
        }
        i++;
    }

    nb_err_set(err, NB_ERR_PARSE, "INI line %zu: unterminated quoted value",
               line_number);
    return NB_ERR_PARSE;
}

static nb_status ini_parse_section(nb_str line, size_t line_number,
                                  nb_buf *section, nb_err *err)
{
    size_t close = nb_str_find(line, NB_STR_LIT("]"));
    if (close == SIZE_MAX) {
        nb_err_set(err, NB_ERR_PARSE, "INI line %zu: missing ']'",
                   line_number);
        return NB_ERR_PARSE;
    }
    nb_str name = nb_str_trim(
        nb_str_from_parts(line.ptr + 1, close - 1u));
    nb_str raw_rest =
        nb_str_from_parts(line.ptr + close + 1u, line.len - close - 1u);
    nb_str rest = nb_str_trim(raw_rest);
    if (rest.len != 0) {
        if (!ini_is_comment((unsigned char)rest.ptr[0])) {
            nb_err_set(err, NB_ERR_PARSE,
                       "INI line %zu: unexpected text after section",
                       line_number);
            return NB_ERR_PARSE;
        }
        /* "[a];note" and "[a]#note" would make the header ambiguous, so a
         * comment after ']' must be separated by whitespace. */
        if (!ini_is_space((unsigned char)raw_rest.ptr[0])) {
            nb_err_set(err, NB_ERR_PARSE,
                       "INI line %zu: comment after section must be preceded "
                       "by whitespace",
                       line_number);
            return NB_ERR_PARSE;
        }
    }
    nb_buf_clear(section);
    if (!nb_buf_append(section, name.ptr, name.len)) {
        nb_err_set(err, NB_ERR_NOMEM, "INI line %zu: out of memory",
                   line_number);
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

static nb_status ini_parse_entry(nb_ini *ini, nb_str line, size_t line_number,
                                 nb_buf *section, nb_buf *scratch,
                                 nb_err *err)
{
    size_t equals = nb_str_find(line, NB_STR_LIT("="));
    if (equals == SIZE_MAX) {
        nb_err_set(err, NB_ERR_PARSE,
                   "INI line %zu: expected 'key = value'", line_number);
        return NB_ERR_PARSE;
    }
    nb_str key = nb_str_trim(nb_str_from_parts(line.ptr, equals));
    if (key.len == 0) {
        nb_err_set(err, NB_ERR_PARSE, "INI line %zu: empty key",
                   line_number);
        return NB_ERR_PARSE;
    }

    nb_str raw = nb_str_from_parts(line.ptr + equals + 1u,
                                   line.len - equals - 1u);
    bool space_before =
        raw.len != 0 && ini_is_space((unsigned char)raw.ptr[0]);
    raw = nb_str_ltrim(raw);
    nb_str value;
    if (raw.len != 0 && (raw.ptr[0] == '"' || raw.ptr[0] == '\'')) {
        nb_status status = ini_parse_quoted(raw, scratch, &value,
                                            line_number, err);
        if (status != NB_OK) {
            return status;
        }
    } else {
        value = ini_unquoted_value(raw, space_before);
    }
    if (!set_str(ini, nb_buf_str(section), key, value)) {
        nb_err_set(err, NB_ERR_NOMEM, "INI line %zu: out of memory",
                   line_number);
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}

static nb_status parse_into(nb_ini *ini, nb_str text, nb_err *err)
{
    if (!ini || (text.len != 0 && !text.ptr)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid INI input");
        return NB_ERR_INVAL;
    }
    /* Values are length-delimited internally, so accepting an embedded NUL
     * would make nb_ini_get() and nb_ini_get_str() disagree. */
    if (text.len != 0 && memchr(text.ptr, '\0', text.len)) {
        nb_err_set(err, NB_ERR_PARSE, "INI input contains a NUL byte");
        return NB_ERR_PARSE;
    }
    /* PHP's INI scanner eats a UTF-8 BOM before the first token. */
    text = nb_utf8_strip_bom(text);

    nb_buf *section = nb_buf_new();
    nb_buf *scratch = nb_buf_new();
    nb_status status = NB_OK;
    size_t pos = 0;
    size_t line_number = 0;

    if (!section || !scratch) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory parsing INI");
        status = NB_ERR_NOMEM;
    } else {
        while (pos < text.len) {
            const char *nl = memchr(text.ptr + pos, '\n', text.len - pos);
            size_t end = nl ? (size_t)(nl - text.ptr) : text.len;
            nb_str line = nb_str_from_parts(text.ptr + pos, end - pos);
            pos = end + (nl ? 1u : 0u);
            line_number++;

            line = nb_str_rtrim(line);
            if (line.len != 0 && line.ptr[line.len - 1u] == '\r') {
                line.len--;
                line = nb_str_rtrim(line);
            }
            line = nb_str_trim(line);
            if (line.len == 0 || line.ptr[0] == ';' || line.ptr[0] == '#') {
                continue;
            }

            if (line.ptr[0] == '[') {
                status = ini_parse_section(line, line_number, section, err);
            } else {
                status = ini_parse_entry(ini, line, line_number, section,
                                         scratch, err);
            }
            if (status != NB_OK) {
                break;
            }
        }
    }

    nb_buf_free(section);
    nb_buf_free(scratch);
    return status;
}

static bool ini_copy(nb_ini *dst, const nb_ini *src)
{
    nb_map_iter sections;
    nb_str section;
    void *section_value;
    nb_map_iter_init(src->sections, &sections);
    while (nb_map_iter_next(&sections, &section, &section_value)) {
        nb_map_iter entries;
        nb_str key;
        void *value;
        nb_map_iter_init(section_value, &entries);
        while (nb_map_iter_next(&entries, &key, &value)) {
            if (!set_str(dst, section, key, ini_value_str(value))) {
                return false;
            }
        }
    }
    return true;
}

nb_status nb_ini_parse(nb_ini *ini, nb_str text, nb_err *err)
{
    if (!ini || (text.len != 0 && !text.ptr)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid INI document or input");
        return NB_ERR_INVAL;
    }
    if (text.len != 0 && memchr(text.ptr, '\0', text.len)) {
        nb_err_set(err, NB_ERR_PARSE, "INI input contains a NUL byte");
        return NB_ERR_PARSE;
    }
    nb_ini *temporary = nb_ini_new();
    if (!temporary) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory parsing INI");
        return NB_ERR_NOMEM;
    }
    if (!ini_copy(temporary, ini)) {
        nb_ini_free(temporary);
        nb_err_set(err, NB_ERR_NOMEM, "out of memory copying INI document");
        return NB_ERR_NOMEM;
    }
    nb_status status = parse_into(temporary, text, err);
    if (status == NB_OK) {
        nb_map *previous = ini->sections;
        ini->sections = temporary->sections;
        temporary->sections = previous;
    }
    nb_ini_free(temporary);
    return status;
}

nb_ini *nb_ini_from_string(nb_str text, nb_err *err)
{
    nb_ini *ini = nb_ini_new();
    if (!ini) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory parsing INI");
        return NULL;
    }
    if (parse_into(ini, text, err) != NB_OK) {
        nb_ini_free(ini);
        return NULL;
    }
    return ini;
}

nb_ini *nb_ini_load(const char *path, nb_err *err)
{
    nb_buf buffer;
    nb_buf_init(&buffer);
    nb_status status = nb_file_read_all(path, &buffer, 0, err);
    nb_ini *ini = status == NB_OK ? nb_ini_from_string(nb_buf_str(&buffer), err)
                                 : NULL;
    nb_buf_destroy(&buffer);
    return ini;
}

bool nb_ini_has(const nb_ini *ini, const char *section, const char *key)
{
    if (!key) {
        return false;
    }
    nb_map *map = section_lookup(ini, section_name(section), false);
    return map && nb_map_has_cstr(map, key);
}

const char *nb_ini_get(const nb_ini *ini, const char *section, const char *key)
{
    if (!key) {
        return NULL;
    }
    nb_map *map = section_lookup(ini, section_name(section), false);
    return map ? (const char *)nb_map_get_cstr(map, key) : NULL;
}

nb_str nb_ini_get_str(const nb_ini *ini, const char *section, const char *key)
{
    if (!key) {
        return NB_STR_NULL;
    }
    nb_map *map = section_lookup(ini, section_name(section), false);
    if (!map) {
        return NB_STR_NULL;
    }
    return ini_value_str(nb_map_get_cstr(map, key));
}

const char *nb_ini_get_default(const nb_ini *ini, const char *section,
                               const char *key, const char *fallback)
{
    const char *value = nb_ini_get(ini, section, key);
    return value ? value : fallback;
}

static bool parse_bool(const char *value, bool *out)
{
    static const char *yes[] = { "1", "true", "yes", "on" };
    static const char *no[] = { "0", "false", "no", "off" };
    for (size_t i = 0; i < NB_ARRAY_SIZE(yes); i++) {
        if (nb_str_eq_cstr(nb_str_from_cstr(value), yes[i])) {
            *out = true;
            return true;
        }
    }
    for (size_t i = 0; i < NB_ARRAY_SIZE(no); i++) {
        if (nb_str_eq_cstr(nb_str_from_cstr(value), no[i])) {
            *out = false;
            return true;
        }
    }
    return false;
}

bool nb_ini_get_bool(const nb_ini *ini, const char *section,
                     const char *key, bool fallback)
{
    const char *value = nb_ini_get(ini, section, key);
    bool result;
    if (value && parse_bool(value, &result)) {
        return result;
    }
    return fallback;
}

int64_t nb_ini_get_int(const nb_ini *ini, const char *section,
                       const char *key, int64_t fallback)
{
    const char *value = nb_ini_get(ini, section, key);
    int64_t result;
    if (value && nb_str_to_i64(nb_str_from_cstr(value), &result)) {
        return result;
    }
    return fallback;
}

static bool ini_contains_byte(nb_str text, char byte)
{
    return text.len != 0 &&
           memchr(text.ptr, (unsigned char)byte, text.len) != NULL;
}

static bool ini_section_is_valid(nb_str section)
{
    if (section.len == 0) {
        return true;
    }
    if (ini_contains_byte(section, ']') ||
        ini_contains_byte(section, '\r') ||
        ini_contains_byte(section, '\n') ||
        ini_contains_byte(section, '\0')) {
        return false;
    }
    return !ini_is_space((unsigned char)section.ptr[0]) &&
           !ini_is_space((unsigned char)section.ptr[section.len - 1u]);
}

static bool ini_key_is_valid(nb_str key)
{
    if (key.len == 0 || ini_contains_byte(key, '=') ||
        ini_contains_byte(key, '\r') || ini_contains_byte(key, '\n') ||
        ini_contains_byte(key, '\0')) {
        return false;
    }
    unsigned char first = (unsigned char)key.ptr[0];
    unsigned char last = (unsigned char)key.ptr[key.len - 1u];
    if (ini_is_space(first) || ini_is_space(last)) {
        return false;
    }
    /* A leading BOM in a global key would land at the start of the written
     * document and be stripped again when that document is parsed. */
    if (key.len >= NB_UTF8_BOM_LEN &&
        memcmp(key.ptr, NB_UTF8_BOM, NB_UTF8_BOM_LEN) == 0) {
        return false;
    }
    return first != '[' && first != ';' && first != '#';
}

nb_status nb_ini_set(nb_ini *ini, const char *section,
                     const char *key, const char *value)
{
    if (!ini || !key || !value) {
        return NB_ERR_INVAL;
    }
    nb_str section_view = section_name(section);
    nb_str key_view = nb_str_from_cstr(key);
    if (!ini_section_is_valid(section_view) ||
        !ini_key_is_valid(key_view)) {
        return NB_ERR_INVAL;
    }
    return set_str(ini, section_view, key_view, nb_str_from_cstr(value))
               ? NB_OK
               : NB_ERR_NOMEM;
}

bool nb_ini_remove(nb_ini *ini, const char *section, const char *key)
{
    if (!ini || !key) {
        return false;
    }
    nb_map *map = section_lookup(ini, section_name(section), false);
    if (!map) {
        return false;
    }
    void *value = nb_map_del_cstr(map, key);
    if (!value) {
        return false;
    }
    ini_value_free(value);
    return true;
}

size_t nb_ini_section_count(const nb_ini *ini)
{
    return ini ? nb_map_size(ini->sections) : 0u;
}

size_t nb_ini_count(const nb_ini *ini, const char *section)
{
    nb_map *map = section_lookup(ini, section_name(section), false);
    return map ? nb_map_size(map) : 0u;
}

static bool ini_value_needs_quotes(nb_str value)
{
    if (value.len == 0) {
        return true;
    }
    if (ini_is_space((unsigned char)value.ptr[0]) ||
        ini_is_space((unsigned char)value.ptr[value.len - 1u])) {
        return true;
    }
    for (size_t i = 0; i < value.len; i++) {
        unsigned char c = (unsigned char)value.ptr[i];
        if (c == '#' || c == ';' || c == '"' || c == '\'' || c == '\\' ||
            c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

static bool ini_append_quoted(nb_buf *out, nb_str value)
{
    if (!nb_buf_append_char(out, '"')) {
        return false;
    }
    for (size_t i = 0; i < value.len; i++) {
        char c = value.ptr[i];
        bool ok;
        switch (c) {
        case '\\': ok = nb_buf_append_cstr(out, "\\\\"); break;
        case '"': ok = nb_buf_append_cstr(out, "\\\""); break;
        case '\n': ok = nb_buf_append_cstr(out, "\\n"); break;
        case '\r': ok = nb_buf_append_cstr(out, "\\r"); break;
        case '\t': ok = nb_buf_append_cstr(out, "\\t"); break;
        default: ok = nb_buf_append_char(out, c); break;
        }
        if (!ok) {
            return false;
        }
    }
    return nb_buf_append_char(out, '"');
}

static bool ini_write_entries(nb_map *map, nb_buf *out)
{
    nb_map_iter entries;
    nb_str key;
    void *value;
    nb_map_iter_init(map, &entries);
    while (nb_map_iter_next(&entries, &key, &value)) {
        nb_str text = ini_value_str(value);
        if (!nb_buf_append_str(out, key) ||
            !nb_buf_append_cstr(out, " = ")) {
            return false;
        }
        if (ini_value_needs_quotes(text)) {
            if (!ini_append_quoted(out, text)) {
                return false;
            }
        } else if (!nb_buf_append_str(out, text)) {
            return false;
        }
        if (!nb_buf_append_char(out, '\n')) {
            return false;
        }
    }
    return true;
}

static nb_status ini_write_into(const nb_ini *ini, nb_buf *out)
{
    if (!ini || !out) {
        return NB_ERR_INVAL;
    }
    /* Global entries have no header, so they must precede every named
     * section; otherwise keys added after a section would be reparsed as
     * belonging to that section. */
    nb_map *global = nb_map_get(ini->sections, section_name(""));
    if (global && !ini_write_entries(global, out)) {
        return NB_ERR_NOMEM;
    }

    nb_map_iter sections;
    nb_str section;
    void *section_value;
    nb_map_iter_init(ini->sections, &sections);
    while (nb_map_iter_next(&sections, &section, &section_value)) {
        if (section.len == 0) {
            continue;
        }
        if (!nb_buf_append_char(out, '[') ||
            !nb_buf_append_str(out, section) ||
            !nb_buf_append_cstr(out, "]\n")) {
            return NB_ERR_NOMEM;
        }
        if (!ini_write_entries(section_value, out)) {
            return NB_ERR_NOMEM;
        }
    }
    return NB_OK;
}

nb_status nb_ini_write(const nb_ini *ini, nb_buf *out)
{
    if (!ini || !out) {
        return NB_ERR_INVAL;
    }
    nb_buf temporary;
    nb_buf_init(&temporary);
    nb_status status = ini_write_into(ini, &temporary);
    if (status == NB_OK && !nb_buf_append_str(out, nb_buf_str(&temporary))) {
        status = NB_ERR_NOMEM;
    }
    nb_buf_destroy(&temporary);
    return status;
}

nb_status nb_ini_save(const nb_ini *ini, const char *path, nb_err *err)
{
    if (!ini || !path || path[0] == '\0') {
        nb_err_set(err, NB_ERR_INVAL, "invalid INI save arguments");
        return NB_ERR_INVAL;
    }
    nb_buf *out = nb_buf_new();
    if (!out) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory saving INI");
        return NB_ERR_NOMEM;
    }
    nb_status status = nb_ini_write(ini, out);
    if (status == NB_OK) {
        status = nb_file_write_all_atomic(path, nb_buf_cstr(out),
                                          nb_buf_len(out), NULL, err);
    } else {
        nb_err_set(err, status, "failed to serialize INI document");
    }
    nb_buf_free(out);
    return status;
}

void nb_ini_iter_init(const nb_ini *ini, nb_ini_iter *iter)
{
    NB_ASSERT(iter != NULL);
    if (!ini) {
        memset(iter, 0, sizeof(*iter));
        return;
    }
    nb_map_iter_init(ini->sections, &iter->sections);
    iter->entries_open = false;
    iter->section = NULL;
}

bool nb_ini_iter_next(nb_ini_iter *iter, const char **section,
                      const char **key, const char **value)
{
    if (!iter) {
        return false;
    }
    for (;;) {
        if (iter->entries_open) {
            nb_str entry_key;
            void *entry_value;
            if (nb_map_iter_next(&iter->entries, &entry_key, &entry_value)) {
                if (section) {
                    *section = iter->section;
                }
                if (key) {
                    *key = entry_key.ptr;
                }
                if (value) {
                    *value = (const char *)entry_value;
                }
                return true;
            }
            iter->entries_open = false;
        }

        nb_str section_key;
        void *section_value;
        if (!nb_map_iter_next(&iter->sections, &section_key, &section_value)) {
            return false;
        }
        iter->section = section_key.ptr;
        nb_map_iter_init((nb_map *)section_value, &iter->entries);
        iter->entries_open = true;
    }
}
