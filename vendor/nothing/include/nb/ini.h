#ifndef NB_INI_H
#define NB_INI_H

#include "nb/base.h"
#include "nb/buf.h"
#include "nb/err.h"
#include "nb/map.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A small INI document. Section names and keys are copied. The global
 * section is represented by the empty string. Values are stored as strings.
 */
typedef struct nb_ini nb_ini;

NB_EXPORT nb_ini *nb_ini_new(void);
NB_EXPORT void nb_ini_free(nb_ini *ini);

/*
 * Parses text and merges it into ini. Parsing is atomic: ini is unchanged
 * on invalid input or allocation failure. Successful parsing invalidates
 * previously borrowed values, section names and iterators.
 *
 * A NUL byte anywhere in text is invalid (NB_ERR_PARSE); values are not
 * truncated at NUL. A comment may follow a section header only when it is
 * separated from the closing ']' by whitespace, so "[a];note" and
 * "[a]#note" are parse errors while "[a] ;note", "[a]\t#note" and "[a]"
 * are accepted. An empty key ("=value") is a parse error.
 *
 * Values may be wrapped in single or double quotes. Inside quotes, the
 * escapes \\, \", \', \n, \r and \t are decoded; other backslash sequences
 * are kept verbatim. Unquoted values are trimmed. A ';' or '#' starts a
 * comment only at the start of a line or when preceded by whitespace, and
 * only outside quotes.
 */
NB_EXPORT nb_status nb_ini_parse(nb_ini *ini, nb_str text, nb_err *err);
NB_EXPORT nb_ini *nb_ini_from_string(nb_str text, nb_err *err);
NB_EXPORT nb_ini *nb_ini_load(const char *path, nb_err *err);

NB_EXPORT bool nb_ini_has(const nb_ini *ini, const char *section,
                          const char *key);
NB_EXPORT const char *nb_ini_get(const nb_ini *ini, const char *section,
                                 const char *key);
/* Length-preserving access. Returns NB_STR_NULL when absent. */
NB_EXPORT nb_str nb_ini_get_str(const nb_ini *ini, const char *section,
                                const char *key);
NB_EXPORT const char *nb_ini_get_default(const nb_ini *ini,
                                         const char *section,
                                         const char *key,
                                         const char *fallback);
NB_EXPORT bool nb_ini_get_bool(const nb_ini *ini, const char *section,
                               const char *key, bool fallback);
NB_EXPORT int64_t nb_ini_get_int(const nb_ini *ini, const char *section,
                                 const char *key, int64_t fallback);

/*
 * Copies the value; failure leaves sections, keys and values unchanged.
 *
 * section is NULL or "" for the global section. A non-empty section must
 * not contain ']', CR, LF or NUL, and must not start or end with whitespace.
 * key must be non-empty, must not contain '=', CR, LF or NUL, must not start
 * or end with whitespace, and must not start with '[', ';' or '#' because
 * those characters would be reinterpreted when the document is written out.
 * A key must not begin with a UTF-8 BOM either; a global key would otherwise
 * become the first bytes of the document and be stripped on the next parse.
 * Invalid arguments return NB_ERR_INVAL before any allocation or
 * modification; NB_ERR_NOMEM means an allocation actually failed.
 */
NB_EXPORT nb_status nb_ini_set(nb_ini *ini, const char *section,
                               const char *key, const char *value);
NB_EXPORT bool nb_ini_remove(nb_ini *ini, const char *section,
                             const char *key);
NB_EXPORT size_t nb_ini_section_count(const nb_ini *ini);
NB_EXPORT size_t nb_ini_count(const nb_ini *ini, const char *section);

/* Appends the document to out. The global section is written first, then
 * named sections in insertion order; key order within each section is
 * preserved. Failures preserve out's contents and cursor (allocation errors
 * may set its flag). */
NB_EXPORT nb_status nb_ini_write(const nb_ini *ini, nb_buf *out);

/* Serializes the document and atomically replaces path. */
NB_EXPORT nb_status nb_ini_save(const nb_ini *ini, const char *path,
                                nb_err *err);

typedef struct nb_ini_iter {
    nb_map_iter sections;
    nb_map_iter entries;
    bool entries_open;
    const char *section;
} nb_ini_iter;

NB_EXPORT void nb_ini_iter_init(const nb_ini *ini, nb_ini_iter *iter);
NB_EXPORT bool nb_ini_iter_next(nb_ini_iter *iter, const char **section,
                                const char **key, const char **value);

#ifdef __cplusplus
}
#endif

#endif /* NB_INI_H */
