#include "json.h"
#include "array.h"
#include "buf.h"
#include "utf8.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Member names are offsets because growing the key buffer can move it. */
struct json_path_segment {
	int is_index;
	size_t value;
};

struct json_path {
	morph_array_t segments;
	morph_buf_t keys;
};

static void json_path_cleanup(struct json_path *path)
{
	morph_array_cleanup(&path->segments);
	morph_buf_cleanup(&path->keys);
}

static int json_path_push(struct json_path *path, int is_index, size_t value)
{
	struct json_path_segment *segment;

	if (path->segments.nelts >= JSON_MAX_DEPTH)
		return -E2BIG;
	segment = morph_array_push(&path->segments);
	if (!segment)
		return -ENOMEM;
	segment->is_index = is_index;
	segment->value = value;
	return 0;
}

static int json_path_bare(struct json_path *path, const char **cursor)
{
	const char *text = *cursor;
	size_t offset = path->keys.len;
	int rc;

	while (*text && *text != '.' && *text != '[') {
		char value = *text++;

		if (value == ']')
			return -EINVAL;
		if (value == '\\') {
			value = *text;
			if (value != '.' && value != '\\' && value != '[' && value != ']')
				return -EINVAL;
			text++;
		}
		rc = morph_buf_putc(&path->keys, value);
		if (rc < 0)
			return rc;
	}
	if (path->keys.len == offset)
		return -EINVAL;
	rc = morph_buf_putc(&path->keys, '\0');
	if (rc < 0)
		return rc;
	rc = json_path_push(path, 0, offset);
	if (rc == 0)
		*cursor = text;
	return rc;
}

static int json_path_quoted(struct json_path *path, const char **cursor)
{
	const char *start = *cursor;
	const char *text = start + 1;
	struct json_error error = {0};
	size_t offset = path->keys.len;
	json *decoded;
	const char *key;
	int rc;

	while (*text && *text != '"') {
		if (*text == '\\') {
			text++;
			if (!*text)
				return -EINVAL;
		}
		text++;
	}
	if (*text != '"')
		return -EINVAL;
	text++;
	decoded = json_parse_n(start, (size_t)(text - start), &error);
	if (!decoded)
		return error.code < 0 ? error.code : -EINVAL;
	key = json_string_value(decoded);
	if (!key) {
		json_free(decoded);
		return -EINVAL;
	}
	rc = morph_buf_append(&path->keys, key, strlen(key) + 1);
	json_free(decoded);
	if (rc < 0)
		return rc;
	rc = json_path_push(path, 0, offset);
	if (rc == 0)
		*cursor = text;
	return rc;
}

static int json_path_bracket(struct json_path *path, const char **cursor)
{
	const char *text = *cursor + 1;
	size_t index = 0;
	int rc;

	if (*text == '"') {
		rc = json_path_quoted(path, &text);
		if (rc < 0)
			return rc;
	} else {
		if (*text < '0' || *text > '9')
			return -EINVAL;
		while (*text >= '0' && *text <= '9') {
			size_t digit = (size_t)(*text - '0');

			if (index > (SIZE_MAX - digit) / 10)
				return -ERANGE;
			index = index * 10 + digit;
			text++;
		}
		rc = json_path_push(path, 1, index);
		if (rc < 0)
			return rc;
	}
	if (*text != ']')
		return -EINVAL;
	*cursor = text + 1;
	return 0;
}

static int json_path_parse(const char *text, struct json_path *path)
{
	struct json_path_segment segment;
	int rc;

	memset(path, 0, sizeof(*path));
	if (!text || !*text)
		return 0;
	if (utf8valid(text))
		return -EILSEQ;
	rc = morph_array_init(&path->segments, 8, sizeof(segment));
	if (rc < 0)
		return rc;
	rc = morph_buf_init(&path->keys, 64);
	if (rc < 0) {
		morph_array_cleanup(&path->segments);
		return rc;
	}
	rc = *text == '[' ? json_path_bracket(path, &text) : json_path_bare(path, &text);
	while (rc == 0 && *text) {
		if (*text == '[') {
			rc = json_path_bracket(path, &text);
		} else if (*text == '.') {
			text++;
			rc = json_path_bare(path, &text);
		} else {
			rc = -EINVAL;
		}
	}
	if (rc < 0)
		json_path_cleanup(path);
	return rc;
}

static const struct json_path_segment *json_path_segment_at(const struct json_path *path,
							    size_t index)
{
	return morph_array_get(&path->segments, index);
}

static const char *json_path_key(const struct json_path *path,
				 const struct json_path_segment *segment)
{
	return path->keys.data + segment->value;
}

static json *json_path_array_at(const json *array, size_t index)
{
	json *value;

	if (index <= INT_MAX)
		return json_at(array, (int)index);
	value = json_first(array);
	while (value && index) {
		value = json_next(value);
		index--;
	}
	return value;
}

static int json_path_child(const json *parent, const struct json_path *path,
			   const struct json_path_segment *segment, json **child)
{
	if (!parent)
		return -ENOENT;
	if (segment->is_index) {
		if (!json_is_array_value(parent))
			return -EINVAL;
		*child = json_path_array_at(parent, segment->value);
	} else {
		if (!json_is_object_value(parent))
			return -EINVAL;
		*child = json_member(parent, json_path_key(path, segment));
	}
	return *child ? 0 : -ENOENT;
}

static int json_path_find(const json *root, const struct json_path *path, size_t count,
			  json **result)
{
	json *current = (json *)root;

	if (!current)
		return -ENOENT;
	for (size_t i = 0; i < count; i++) {
		int rc = json_path_child(current, path, json_path_segment_at(path, i), &current);

		if (rc < 0)
			return rc;
	}
	*result = current;
	return 0;
}

json *json_get(const json *root, const char *text)
{
	struct json_path path;
	json *result = NULL;

	if (json_path_parse(text, &path) < 0)
		return NULL;
	json_path_find(root, &path, path.segments.nelts, &result);
	json_path_cleanup(&path);
	return result;
}

bool json_exists(const json *root, const char *path)
{
	return json_get(root, path) != NULL;
}

const char *json_str_or(const json *root, const char *path, const char *fallback)
{
	const char *result;

	return json_read_string(json_get(root, path), &result) == 0 ? result : fallback;
}

const char *json_str(const json *root, const char *path)
{
	return json_str_or(root, path, NULL);
}

int64_t json_int_or(const json *root, const char *path, int64_t fallback)
{
	int64_t result;

	return json_read_int(json_get(root, path), &result) == 0 ? result : fallback;
}

int64_t json_int(const json *root, const char *path)
{
	return json_int_or(root, path, 0);
}

double json_num_or(const json *root, const char *path, double fallback)
{
	double result;

	return json_read_number(json_get(root, path), &result) == 0 ? result : fallback;
}

double json_num(const json *root, const char *path)
{
	return json_num_or(root, path, 0);
}

bool json_bool_or(const json *root, const char *path, bool fallback)
{
	bool result;

	return json_read_bool(json_get(root, path), &result) == 0 ? result : fallback;
}

bool json_bool(const json *root, const char *path)
{
	return json_bool_or(root, path, false);
}

bool json_is_string(const json *root, const char *path)
{
	return json_is_string_value(json_get(root, path));
}

bool json_is_number(const json *root, const char *path)
{
	return json_is_number_value(json_get(root, path));
}

bool json_is_bool(const json *root, const char *path)
{
	return json_is_bool_value(json_get(root, path));
}

bool json_is_array(const json *root, const char *path)
{
	return json_is_array_value(json_get(root, path));
}

bool json_is_object(const json *root, const char *path)
{
	return json_is_object_value(json_get(root, path));
}

bool json_is_null(const json *root, const char *path)
{
	return json_is_null_value(json_get(root, path));
}

/* Only this final attachment changes the original tree. */
static int json_path_insert(json *parent, const struct json_path *path,
			    const struct json_path_segment *segment, json **owned)
{
	int rc;

	if (segment->is_index) {
		if (!json_is_array_value(parent))
			return -EINVAL;
		if (segment->value != json_size(parent))
			return -ERANGE;
		rc = json_array_add(parent, *owned);
	} else {
		if (!json_is_object_value(parent))
			return -EINVAL;
		rc = json_object_add(parent, json_path_key(path, segment), *owned);
	}
	if (rc == 0)
		*owned = NULL;
	return rc;
}

/* Build a missing suffix away from the caller's tree. The supplied value is
 * already a private copy; failures can destroy this entire unfinished branch. */
static int json_path_build_branch(const struct json_path *path, size_t missing, json **value,
				  json **branch)
{
	*branch = *value;
	*value = NULL;
	for (size_t i = path->segments.nelts; i > missing + 1; i--) {
		const struct json_path_segment *segment = json_path_segment_at(path, i - 1);
		json *parent = segment->is_index ? json_array() : json_object();
		int rc;

		if (!parent)
			return -ENOMEM;
		rc = json_path_insert(parent, path, segment, branch);
		if (rc < 0) {
			json_free(parent);
			return rc;
		}
		*branch = parent;
	}
	return 0;
}

static int json_path_wrap_array(json **value)
{
	json *array = json_array();
	int rc;

	if (!array)
		return -ENOMEM;
	rc = json_array_add(array, *value);
	if (rc < 0) {
		json_free(array);
		return rc;
	}
	*value = array;
	return 0;
}

static int json_path_mutate(json *root, const struct json_path *path, json **value, bool append)
{
	json *current = root;

	for (size_t i = 0; i < path->segments.nelts; i++) {
		const struct json_path_segment *segment = json_path_segment_at(path, i);
		json *child = NULL;
		int rc = json_path_child(current, path, segment, &child);

		if (rc == -ENOENT) {
			json *branch = NULL;

			if (segment->is_index && segment->value > json_size(current))
				return -ERANGE;
			if (append) {
				rc = json_path_wrap_array(value);
				if (rc < 0)
					return rc;
			}
			rc = json_path_build_branch(path, i, value, &branch);
			if (rc == 0)
				rc = json_path_insert(current, path, segment, &branch);
			json_free(branch);
			return rc;
		}
		if (rc < 0)
			return rc;
		current = child;
	}
	if (!append)
		return json_assign_take(current, value);
	if (!json_is_array_value(current))
		return -EINVAL;
	int rc = json_array_add(current, *value);

	if (rc == 0)
		*value = NULL;
	return rc;
}

static int json_path_value(struct json_value arg, json **value)
{
	switch (arg.kind) {
	case JSON_VALUE_NULL:
		*value = json_null();
		break;
	case JSON_VALUE_BOOL:
		*value = json_boolean(arg.data.boolean);
		break;
	case JSON_VALUE_INT:
		if (arg.data.integer < -JSON_SAFE_INTEGER || arg.data.integer > JSON_SAFE_INTEGER)
			return -ERANGE;
		*value = json_number((double)arg.data.integer);
		break;
	case JSON_VALUE_UINT:
		if (arg.data.unsigned_integer > (uint64_t)JSON_SAFE_INTEGER)
			return -ERANGE;
		*value = json_number((double)arg.data.unsigned_integer);
		break;
	case JSON_VALUE_NUMBER:
		if (!isfinite(arg.data.number))
			return -ERANGE;
		*value = json_number(arg.data.number);
		break;
	case JSON_VALUE_STRING:
		if (!arg.data.string)
			return -EINVAL;
		if (utf8valid(arg.data.string))
			return -EILSEQ;
		*value = json_string(arg.data.string);
		break;
	case JSON_VALUE_NODE:
		if (!arg.data.node)
			return -EINVAL;
		*value = json_clone(arg.data.node, true);
		break;
	default:
		return -EINVAL;
	}
	return *value ? 0 : -ENOMEM;
}

static int json_path_set_arg(json *root, const char *text, struct json_value arg, bool append)
{
	struct json_path path;
	json *value = NULL;
	int rc;

	if (!root)
		return -EINVAL;
	rc = json_path_parse(text, &path);
	if (rc < 0)
		return rc;
	rc = json_path_value(arg, &value);
	if (rc == 0)
		rc = json_path_mutate(root, &path, &value, append);
	json_free(value);
	json_path_cleanup(&path);
	return rc;
}

int json_set_value(json *root, const char *path, struct json_value value)
{
	return json_path_set_arg(root, path, value, false);
}

int json_append_value(json *root, const char *path, struct json_value value)
{
	return json_path_set_arg(root, path, value, true);
}

static int json_path_take(json *root, const char *text, json **owned, bool append)
{
	struct json_path path;
	json *value;
	int rc;

	if (!root || !owned)
		return -EINVAL;
	rc = json_validate_take(root, *owned);
	if (rc < 0)
		return rc;
	rc = json_path_parse(text, &path);
	if (rc < 0)
		return rc;
	value = json_clone(*owned, true);
	if (!value) {
		json_path_cleanup(&path);
		return -ENOMEM;
	}
	rc = json_path_mutate(root, &path, &value, append);
	json_free(value);
	json_path_cleanup(&path);
	if (rc == 0) {
		json_free(*owned);
		*owned = NULL;
	}
	return rc;
}

int json_set_take(json *root, const char *path, json **owned)
{
	return json_path_take(root, path, owned, false);
}

int json_append_take(json *root, const char *path, json **owned)
{
	return json_path_take(root, path, owned, true);
}

int json_erase(json *root, const char *text)
{
	struct json_path path;
	json *parent;
	json *value = NULL;
	int rc;

	if (!root)
		return -EINVAL;
	rc = json_path_parse(text, &path);
	if (rc < 0)
		return rc;
	if (!path.segments.nelts) {
		json_path_cleanup(&path);
		return -EINVAL;
	}
	rc = json_path_find(root, &path, path.segments.nelts - 1, &parent);
	if (rc == 0)
		rc = json_path_child(parent, &path,
				     json_path_segment_at(&path, path.segments.nelts - 1), &value);
	if (rc == 0)
		json_free(value);
	json_path_cleanup(&path);
	return rc;
}
