#ifndef NB_TOML_H
#define NB_TOML_H

#include "nb/base.h"
#include "nb/err.h"
#include "nb/map.h"
#include "nb/str.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TOML 1.1 document parsed into an immutable AST.
 *
 * The root is always a table. Strings, keys, table entries, array elements
 * and iterators are borrowed views valid until nb_toml_free(). Values never
 * contain a trailing NUL requirement: string values are length-delimited and
 * may contain U+0000 when written as an escape in the source document.
 */
typedef struct nb_toml nb_toml;
typedef struct nb_toml_value nb_toml_value;

typedef enum nb_toml_type {
    NB_TOML_NONE = 0,
    NB_TOML_TABLE,
    NB_TOML_ARRAY,
    NB_TOML_STRING,
    NB_TOML_INTEGER,
    NB_TOML_FLOAT,
    NB_TOML_BOOLEAN,
    NB_TOML_DATETIME
} nb_toml_type;

typedef enum nb_toml_datetime_kind {
    NB_TOML_LOCAL_DATE = 1,
    NB_TOML_LOCAL_TIME,
    NB_TOML_LOCAL_DATETIME,
    NB_TOML_OFFSET_DATETIME
} nb_toml_datetime_kind;

typedef struct nb_toml_datetime {
    nb_toml_datetime_kind kind;
    /* Date fields are zero for a local time. */
    int32_t year;
    int32_t month;
    int32_t day;
    /* Time fields are zero for a local date. */
    int32_t hour;
    int32_t minute;
    int32_t second;
    uint32_t nanosecond;
    /* Minutes east of UTC for NB_TOML_OFFSET_DATETIME; 0 for Z. */
    int32_t offset_minutes;
} nb_toml_datetime;

/* Parses one TOML 1.1 document. Returns NULL on invalid input or allocation
 * failure and sets err to NB_ERR_PARSE or NB_ERR_NOMEM. */
NB_EXPORT nb_toml *nb_toml_parse(nb_str text, nb_err *err);

/* Reads and parses path. File errors are reported before parse errors. */
NB_EXPORT nb_toml *nb_toml_load(const char *path, nb_err *err);

NB_EXPORT void nb_toml_free(nb_toml *toml);

/* Root table; never NULL for a successfully parsed document. */
NB_EXPORT const nb_toml_value *nb_toml_root(const nb_toml *toml);

NB_EXPORT nb_toml_type nb_toml_type_of(const nb_toml_value *value);

/* Source positions are byte offsets into the parsed input. A table header
 * covers its bracketed header, while scalar/inline values cover their token.
 * Implicit tables have line == 0 and no source token. */
typedef struct nb_toml_location {
    size_t start;
    size_t end;
    int line;
    int column;
    bool inline_table;
    bool table_header;
} nb_toml_location;

NB_EXPORT nb_toml_location nb_toml_location_of(const nb_toml_value *value);
/* Typed value views return an empty/zero value on a type mismatch. String
 * storage has a trailing NUL; embedded NULs remain represented by len. */
NB_EXPORT nb_str nb_toml_string(const nb_toml_value *value);
NB_EXPORT int64_t nb_toml_integer(const nb_toml_value *value);
NB_EXPORT double nb_toml_float(const nb_toml_value *value);
NB_EXPORT bool nb_toml_boolean(const nb_toml_value *value);
NB_EXPORT bool nb_toml_value_equal(const nb_toml_value *a, const nb_toml_value *b);
/* Indexed access follows insertion order, returning an empty key/NULL when
 * index is outside the table. Prefer iterators for complete traversal. */
NB_EXPORT nb_str nb_toml_table_key_at(const nb_toml_value *table, size_t index);
NB_EXPORT const nb_toml_value *nb_toml_table_value_at(const nb_toml_value *table, size_t index);

/* Table access. get_path walks named table segments only; array traversal is
 * intentionally explicit so callers choose the element. */
NB_EXPORT size_t nb_toml_table_size(const nb_toml_value *table);
NB_EXPORT const nb_toml_value *nb_toml_table_get(const nb_toml_value *table,
                                                nb_str key);
NB_EXPORT const nb_toml_value *nb_toml_table_get_cstr(
    const nb_toml_value *table, const char *key);
NB_EXPORT const nb_toml_value *nb_toml_get_path(const nb_toml *toml,
                                               const nb_str *keys,
                                               size_t count);

typedef struct nb_toml_table_iter {
    nb_map_iter inner;
} nb_toml_table_iter;

/* Insertion-order iteration. key points into the table and is valid while
 * the document is alive and unmodified (documents are immutable). */
NB_EXPORT void nb_toml_table_iter_init(const nb_toml_value *table,
                                       nb_toml_table_iter *iter);
NB_EXPORT bool nb_toml_table_iter_next(nb_toml_table_iter *iter, nb_str *key,
                                       const nb_toml_value **value);

/* Array access. index must be less than nb_toml_array_size(). */
NB_EXPORT size_t nb_toml_array_size(const nb_toml_value *array);
NB_EXPORT const nb_toml_value *nb_toml_array_get(const nb_toml_value *array,
                                                size_t index);

/* Typed access. Each returns false and leaves *out unchanged when value has
 * a different type or is NULL. */
NB_EXPORT bool nb_toml_get_string(const nb_toml_value *value, nb_str *out);
NB_EXPORT bool nb_toml_get_integer(const nb_toml_value *value, int64_t *out);
NB_EXPORT bool nb_toml_get_float(const nb_toml_value *value, double *out);
NB_EXPORT bool nb_toml_get_bool(const nb_toml_value *value, bool *out);
NB_EXPORT bool nb_toml_get_datetime(const nb_toml_value *value,
                                    nb_toml_datetime *out);

#ifdef __cplusplus
}
#endif

#endif /* NB_TOML_H */
