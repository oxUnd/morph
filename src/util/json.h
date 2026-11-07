#ifndef MORPH_JSON_H
#define MORPH_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct json json;
struct arena;

#define JSON_MAX_DEPTH 128
#define JSON_SAFE_INTEGER INT64_C(9007199254740991)

enum json_type {
	JSON_MISSING,
	JSON_NULL_TYPE,
	JSON_BOOL_TYPE,
	JSON_NUMBER_TYPE,
	JSON_STRING_TYPE,
	JSON_ARRAY_TYPE,
	JSON_OBJECT_TYPE,
};

struct json_error {
	int code;
	size_t offset;
	const char *message;
};

/* Parsing is strict by default. Errors belong to this call, never a global.
 * Numbers use finite IEEE double; embedded NUL strings are unsupported.
 * parse_ex can explicitly accept a valid prefix and reports its end. */
json *json_parse(const char *text);
json *json_parse_n(const char *text, size_t length, struct json_error *error);
json *json_parse_ex(const char *text, const char **end, bool require_end, struct json_error *error);
json *json_parse_prefix(const char *text, const char **end, struct json_error *error);

json *json_object(void);
json *json_array(void);
json *json_string(const char *text);
json *json_string_n(const char *text, size_t length);
/* Internal streaming fragments may end within a UTF-8 codepoint. This explicit
 * byte-preserving constructor is for in-process events, not JSON wire output.
 * Embedded NUL remains unsupported. Prefer json_string for complete text. */
json *json_string_bytes_n(const char *text, size_t length);
json *json_number(double number);
json *json_boolean(bool value);
json *json_null(void);
/* Consumes a malloc-allocated string, including on failure. */
json *json_take_string(char *text);
json *json_clone(const json *value, bool recurse);
void json_free(json *value);

/* Returned strings are malloc-owned; free them with json_text_free or free.
 * Arena serialization registers the buffer with the supplied arena. */
char *json_stringify(const json *value);
char *json_stringify_pretty(const json *value);
char *json_stringify_arena(struct arena *arena, const json *value);
void json_text_free(void *text);

enum json_type json_typeof(const json *value);
const char *json_string_value(const json *value);
double json_number_value(const json *value);
int json_int_value(const json *value);
bool json_bool_value(const json *value);
bool json_is_string_value(const json *value);
bool json_is_number_value(const json *value);
bool json_is_bool_value(const json *value);
bool json_is_array_value(const json *value);
bool json_is_object_value(const json *value);
bool json_is_null_value(const json *value);
bool json_is_true(const json *value);
bool json_is_false(const json *value);
int json_read_int(const json *value, int64_t *result);
int json_read_uint(const json *value, uint64_t *result);
int json_read_number(const json *value, double *result);
int json_read_bool(const json *value, bool *result);
int json_read_string(const json *value, const char **result);

/* Literal keys never interpret dots or brackets. Duplicate keys retain order;
 * lookup/replace/remove operate on the first match. CI uses ASCII folding. */
json *json_member(const json *object, const char *key);
json *json_member_ci(const json *object, const char *key);
bool json_has_member(const json *object, const char *key);
bool json_has_member_ci(const json *object, const char *key);
json *json_at(const json *array, int index);
size_t json_size(const json *container);
int json_count(const json *container);
const char *json_key(const json *value);
json *json_first(const json *container);
json *json_next(const json *value);
json *json_prev(const json *value);
#define json_foreach(item, container)                                                              \
	for ((item) = json_first(container); (item); (item) = json_next(item))

/* Attach/replace transfer only on success. Attached nodes, cycles, invalid
 * types and excessive depth fail without changing either tree. Adds permit
 * duplicate literal keys; replace requires an existing member/index.
 * Every status function returns zero on success and a negative error on failure. */
int json_object_add(json *object, const char *key, json *value);
int json_array_add(json *array, json *value);
int json_object_replace(json *object, const char *key, json *value);
int json_object_replace_ci(json *object, const char *key, json *value);
int json_array_replace(json *array, int index, json *value);
/* NULL target checks detached ownership only; otherwise also rejects cycles. */
int json_validate_take(const json *target, const json *owned);
int json_assign_take(json *target, json **owned);
json *json_detach(json *parent, json *child);
json *json_object_detach(json *object, const char *key);
json *json_object_detach_ci(json *object, const char *key);
int json_object_remove(json *object, const char *key);
int json_object_remove_ci(json *object, const char *key);
int json_array_remove(json *array, int index);
char *json_string_update(json *value, const char *text);
int json_number_update(json *value, double number);
bool json_equal(const json *a, const json *b, bool case_sensitive);

json *json_add_string(json *object, const char *key, const char *text);
json *json_add_number(json *object, const char *key, double number);
json *json_add_bool(json *object, const char *key, bool value);
json *json_add_null(json *object, const char *key);
json *json_add_array(json *object, const char *key);
json *json_add_object(json *object, const char *key);

/* Paths are case sensitive. Empty/NULL path denotes the root. Escape '.',
 * '[', ']' and '\\' in bare keys, or use ["literal.key"]. Array indexes are
 * unsigned decimal. Reads are borrowed; mutation invalidates affected nodes. */
json *json_get(const json *root, const char *path);
bool json_exists(const json *root, const char *path);
const char *json_str(const json *root, const char *path);
const char *json_str_or(const json *root, const char *path, const char *fallback);
int64_t json_int(const json *root, const char *path);
int64_t json_int_or(const json *root, const char *path, int64_t fallback);
double json_num(const json *root, const char *path);
double json_num_or(const json *root, const char *path, double fallback);
bool json_bool(const json *root, const char *path);
bool json_bool_or(const json *root, const char *path, bool fallback);
bool json_is_string(const json *root, const char *path);
bool json_is_number(const json *root, const char *path);
bool json_is_bool(const json *root, const char *path);
bool json_is_array(const json *root, const char *path);
bool json_is_object(const json *root, const char *path);
bool json_is_null(const json *root, const char *path);

/* Generic setters copy strings and deep-copy node values. Missing branches
 * are built before attachment. Existing scalar intermediates are errors;
 * index == length appends, larger indexes fail without filling holes.
 * Explicit take variants clear *owned on success and retain it on failure.
 * They may copy and release the supplied node; node identity is not retained.
 * Invalid UTF-8 is -EILSEQ; embedded NUL is -ENOTSUP; range/depth errors are
 * -ERANGE/-E2BIG. Missing members are -ENOENT and allocation failure -ENOMEM. */
enum json_value_kind {
	JSON_VALUE_NULL,
	JSON_VALUE_BOOL,
	JSON_VALUE_INT,
	JSON_VALUE_UINT,
	JSON_VALUE_NUMBER,
	JSON_VALUE_STRING,
	JSON_VALUE_NODE,
};
struct json_value {
	enum json_value_kind kind;
	union {
		bool boolean;
		int64_t integer;
		uint64_t unsigned_integer;
		double number;
		const char *string;
		const json *node;
	} data;
};
struct json_value json_arg_null(void);
struct json_value json_arg_bool(bool value);
struct json_value json_arg_int(int64_t value);
struct json_value json_arg_uint(uint64_t value);
struct json_value json_arg_number(double value);
struct json_value json_arg_string(const char *value);
struct json_value json_arg_node(const json *value);
struct json_value json_arg_value(struct json_value value);
int json_set_value(json *root, const char *path, struct json_value value);
int json_append_value(json *root, const char *path, struct json_value value);
int json_set_take(json *root, const char *path, json **owned);
int json_append_take(json *root, const char *path, json **owned);
int json_erase(json *root, const char *path);

#ifdef __cplusplus
}
#endif

#include "json_generic.h"

#endif
