#define _GNU_SOURCE
#include "json.h"
#include "json_internal.h"
#include "arena.h"
#include "utf8.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

static cJSON *take_backend_string(char *text);
static char *print_backend_arena(struct arena *arena, const cJSON *value);

static void free_wrappers(json *value)
{
	json *child = value ? value->child : NULL;

	while (child) {
		json *next = child->next;

		free_wrappers(child);
		child = next;
	}
	free(value);
}

static json *wrap_backend(cJSON *backend)
{
	json *value = calloc(1, sizeof(*value));
	json *tail = NULL;

	if (!value)
		return NULL;
	value->backend = backend;
	for (cJSON *item = backend->child; item; item = item->next) {
		json *child = wrap_backend(item);

		if (!child) {
			free_wrappers(value);
			return NULL;
		}
		child->parent = value;
		child->prev = tail;
		if (tail)
			tail->next = child;
		else
			value->child = child;
		tail = child;
	}
	value->last = tail;
	return value;
}

static json *own_backend(cJSON *backend)
{
	json *value;

	if (!backend) {
		errno = ENOMEM;
		return NULL;
	}
	value = wrap_backend(backend);
	if (!value) {
		cJSON_Delete(backend);
		errno = ENOMEM;
	}
	return value;
}

static const char *parse_message(int error)
{
	switch (error) {
	case 0:
		return NULL;
	case -ENOMEM:
		return "JSON allocation failed";
	case -EILSEQ:
		return "Invalid UTF-8 in JSON string";
	case -ENOTSUP:
		return "Embedded NUL is unsupported";
	case -ERANGE:
		return "JSON number is outside the finite double range";
	case -E2BIG:
		return "JSON nesting exceeds the depth limit";
	default:
		return "Invalid JSON syntax";
	}
}

static json *parse_document(const char *text, size_t length, bool prefix, size_t *consumed,
			    struct json_error *error)
{
	cJSON *backend = NULL;
	json *value = NULL;
	size_t offset = 0;
	int rc = json_parse_backend(text, length, prefix, &backend, consumed, &offset);

	if (rc == 0) {
		value = own_backend(backend);
		if (!value)
			rc = -ENOMEM;
	}
	if (error)
		*error = (struct json_error){
			.code = rc, .offset = offset, .message = parse_message(rc)};
	if (rc != 0)
		errno = -rc;
	return value;
}

json *json_parse(const char *text)
{
	return json_parse_n(text, text ? strlen(text) : 0, NULL);
}

json *json_parse_n(const char *text, size_t length, struct json_error *error)
{
	return parse_document(text, length, false, NULL, error);
}

json *json_parse_ex(const char *text, const char **end, bool require_end, struct json_error *error)
{
	struct json_error local = {0};
	size_t consumed = 0;
	json *value = parse_document(text, text ? strlen(text) : 0, !require_end, &consumed,
				     error ? error : &local);

	if (end)
		*end = text ? text + (value ? consumed : (error ? error->offset : local.offset))
			    : NULL;
	return value;
}

json *json_parse_prefix(const char *text, const char **end, struct json_error *error)
{
	return json_parse_ex(text, end, false, error);
}

json *json_object(void)
{
	return own_backend(cJSON_CreateObject());
}

json *json_array(void)
{
	return own_backend(cJSON_CreateArray());
}

static int validate_string(const char *text, size_t length)
{
	if (!text)
		return -EINVAL;
	if (memchr(text, '\0', length))
		return -ENOTSUP;
	return utf8nvalid(text, length) ? -EILSEQ : 0;
}

json *json_take_string(char *text)
{
	int rc = validate_string(text, text ? strlen(text) : 0);
	cJSON *backend;

	if (rc != 0) {
		free(text);
		errno = -rc;
		return NULL;
	}
	backend = take_backend_string(text);
	return own_backend(backend);
}

static json *copy_string(const char *text, size_t length)
{
	char *copy;

	copy = malloc(length + 1);
	if (!copy) {
		errno = ENOMEM;
		return NULL;
	}
	memcpy(copy, text, length);
	copy[length] = '\0';
	return own_backend(take_backend_string(copy));
}

json *json_string_bytes_n(const char *text, size_t length)
{
	if (!text || length == SIZE_MAX) {
		errno = text ? EOVERFLOW : EINVAL;
		return NULL;
	}
	if (memchr(text, '\0', length)) {
		errno = ENOTSUP;
		return NULL;
	}
	return copy_string(text, length);
}

json *json_string_n(const char *text, size_t length)
{
	int rc;

	if (length == SIZE_MAX) {
		errno = EOVERFLOW;
		return NULL;
	}
	rc = validate_string(text, length);
	if (rc != 0) {
		errno = -rc;
		return NULL;
	}
	return copy_string(text, length);
}

json *json_string(const char *text)
{
	return json_string_n(text, text ? strlen(text) : 0);
}

json *json_number(double number)
{
	if (!isfinite(number)) {
		errno = ERANGE;
		return NULL;
	}
	return own_backend(cJSON_CreateNumber(number));
}

json *json_boolean(bool value)
{
	return own_backend(cJSON_CreateBool(value));
}

json *json_null(void)
{
	return own_backend(cJSON_CreateNull());
}

json *json_clone(const json *value, bool recurse)
{
	if (!value) {
		errno = EINVAL;
		return NULL;
	}
	return own_backend(cJSON_Duplicate(value->backend, recurse));
}

static void unlink_wrapper(json *parent, json *value)
{
	if (value->prev)
		value->prev->next = value->next;
	else
		parent->child = value->next;
	if (value->next)
		value->next->prev = value->prev;
	else
		parent->last = value->prev;
	value->parent = NULL;
	value->next = NULL;
	value->prev = NULL;
}

json *json_detach(json *parent, json *child)
{
	if (!parent || !child || child->parent != parent)
		return NULL;
	if (!cJSON_DetachItemViaPointer(parent->backend, child->backend))
		return NULL;
	unlink_wrapper(parent, child);
	return child;
}

void json_free(json *value)
{
	if (!value)
		return;
	if (value->parent)
		(void)json_detach(value->parent, value);
	cJSON_Delete(value->backend);
	free_wrappers(value);
}

static int validate_serialization(const json *value)
{
	if (!value)
		return -EINVAL;
	if (json_is_string_value(value) && utf8valid(json_string_value(value)))
		return -EILSEQ;
	for (const json *child = value->child; child; child = child->next) {
		int rc = validate_serialization(child);

		if (rc != 0)
			return rc;
	}
	return 0;
}

static char *print_backend(const cJSON *backend, bool pretty)
{
	locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
	locale_t previous;
	char *text;

	if (!locale)
		return NULL;
	previous = uselocale(locale);
	if (!previous) {
		int saved = errno;

		freelocale(locale);
		errno = saved;
		return NULL;
	}
	text = pretty ? cJSON_Print(backend) : cJSON_PrintUnformatted(backend);
	(void)uselocale(previous);
	freelocale(locale);
	if (!text)
		errno = ENOMEM;
	return text;
}

static char *stringify(const json *value, bool pretty)
{
	int rc = validate_serialization(value);
	char *text;

	if (rc != 0) {
		errno = -rc;
		return NULL;
	}
	text = print_backend(value->backend, pretty);
	return text;
}

char *json_stringify(const json *value)
{
	return stringify(value, false);
}

char *json_stringify_pretty(const json *value)
{
	return stringify(value, true);
}

char *json_stringify_arena(struct arena *arena, const json *value)
{
	int rc = arena ? validate_serialization(value) : -EINVAL;
	char *text;

	if (rc != 0) {
		errno = -rc;
		return NULL;
	}
	text = print_backend_arena(arena, value->backend);
	if (!text)
		errno = ENOMEM;
	return text;
}

void json_text_free(void *text)
{
	cJSON_free(text);
}

enum json_type json_typeof(const json *value)
{
	if (!value)
		return JSON_MISSING;
	if (cJSON_IsObject(value->backend))
		return JSON_OBJECT_TYPE;
	if (cJSON_IsArray(value->backend))
		return JSON_ARRAY_TYPE;
	if (cJSON_IsString(value->backend))
		return JSON_STRING_TYPE;
	if (cJSON_IsNumber(value->backend))
		return JSON_NUMBER_TYPE;
	if (cJSON_IsBool(value->backend))
		return JSON_BOOL_TYPE;
	return JSON_NULL_TYPE;
}

const char *json_string_value(const json *value)
{
	return value ? cJSON_GetStringValue(value->backend) : NULL;
}

double json_number_value(const json *value)
{
	return value ? cJSON_GetNumberValue(value->backend) : NAN;
}

int json_int_value(const json *value)
{
	return json_is_number_value(value) ? value->backend->valueint : 0;
}

bool json_bool_value(const json *value)
{
	return json_is_true(value);
}

bool json_is_string_value(const json *value)
{
	return json_typeof(value) == JSON_STRING_TYPE;
}

bool json_is_number_value(const json *value)
{
	return json_typeof(value) == JSON_NUMBER_TYPE;
}

bool json_is_bool_value(const json *value)
{
	return json_typeof(value) == JSON_BOOL_TYPE;
}

bool json_is_array_value(const json *value)
{
	return json_typeof(value) == JSON_ARRAY_TYPE;
}

bool json_is_object_value(const json *value)
{
	return json_typeof(value) == JSON_OBJECT_TYPE;
}

bool json_is_null_value(const json *value)
{
	return json_typeof(value) == JSON_NULL_TYPE;
}

bool json_is_true(const json *value)
{
	return value && cJSON_IsTrue(value->backend);
}

bool json_is_false(const json *value)
{
	return value && cJSON_IsFalse(value->backend);
}

int json_read_number(const json *value, double *result)
{
	if (!result)
		return -EINVAL;
	if (!value)
		return -ENOENT;
	if (!json_is_number_value(value))
		return -EINVAL;
	*result = value->backend->valuedouble;
	return 0;
}

int json_read_int(const json *value, int64_t *result)
{
	double number;
	int rc;

	if (!result)
		return -EINVAL;
	rc = json_read_number(value, &number);
	if (rc != 0)
		return rc;
	if (number < -0x1p63 || number >= 0x1p63 || trunc(number) != number)
		return -ERANGE;
	*result = (int64_t)number;
	return 0;
}

int json_read_uint(const json *value, uint64_t *result)
{
	double number;
	int rc;

	if (!result)
		return -EINVAL;
	rc = json_read_number(value, &number);
	if (rc != 0)
		return rc;
	if (number < 0 || number >= 0x1p64 || trunc(number) != number)
		return -ERANGE;
	*result = (uint64_t)number;
	return 0;
}

int json_read_bool(const json *value, bool *result)
{
	if (!result)
		return -EINVAL;
	if (!value)
		return -ENOENT;
	if (!json_is_bool_value(value))
		return -EINVAL;
	*result = json_bool_value(value);
	return 0;
}

int json_read_string(const json *value, const char **result)
{
	if (!result)
		return -EINVAL;
	if (!value)
		return -ENOENT;
	if (!json_is_string_value(value))
		return -EINVAL;
	*result = json_string_value(value);
	return 0;
}

static unsigned char fold_ascii(unsigned char ch)
{
	return ch >= 'A' && ch <= 'Z' ? (unsigned char)(ch + ('a' - 'A')) : ch;
}

static bool key_equal(const char *a, const char *b, bool case_sensitive)
{
	if (!a || !b)
		return a == b;
	while (*a && *b) {
		unsigned char ac = (unsigned char)*a++;
		unsigned char bc = (unsigned char)*b++;

		if ((case_sensitive ? ac : fold_ascii(ac)) !=
		    (case_sensitive ? bc : fold_ascii(bc)))
			return false;
	}
	return *a == *b;
}

static json *find_member(const json *object, const char *key, bool case_sensitive)
{
	if (!json_is_object_value(object) || !key)
		return NULL;
	for (json *child = object->child; child; child = child->next) {
		if (key_equal(child->backend->string, key, case_sensitive))
			return child;
	}
	return NULL;
}

json *json_member(const json *object, const char *key)
{
	return find_member(object, key, true);
}

json *json_member_ci(const json *object, const char *key)
{
	return find_member(object, key, false);
}

bool json_has_member(const json *object, const char *key)
{
	return json_member(object, key) != NULL;
}

bool json_has_member_ci(const json *object, const char *key)
{
	return json_member_ci(object, key) != NULL;
}

json *json_at(const json *array, int index)
{
	json *child;

	if (!json_is_array_value(array) || index < 0)
		return NULL;
	child = array->child;
	while (child && index--)
		child = child->next;
	return child;
}

size_t json_size(const json *container)
{
	size_t count = 0;

	if (!json_is_object_value(container) && !json_is_array_value(container))
		return 0;
	for (json *child = container->child; child; child = child->next)
		count++;
	return count;
}

int json_count(const json *container)
{
	size_t count = json_size(container);

	return count > INT_MAX ? INT_MAX : (int)count;
}

const char *json_key(const json *value)
{
	return value ? value->backend->string : NULL;
}

json *json_first(const json *container)
{
	return container ? container->child : NULL;
}

json *json_next(const json *value)
{
	return value ? value->next : NULL;
}

json *json_prev(const json *value)
{
	return value ? value->prev : NULL;
}

int json_validate_take(const json *target, const json *owned)
{
	if (!owned || owned->parent)
		return -EINVAL;
	for (const json *ancestor = target; ancestor; ancestor = ancestor->parent) {
		if (ancestor == owned)
			return -EINVAL;
	}
	return 0;
}

static size_t tree_height(const json *value)
{
	size_t height = 1;

	for (const json *child = value->child; child; child = child->next) {
		size_t child_height = tree_height(child) + 1;

		if (child_height > height)
			height = child_height;
	}
	return height;
}

static size_t node_depth(const json *value)
{
	size_t depth = 0;

	for (; value; value = value->parent)
		depth++;
	return depth;
}

static int validate_attachment(const json *parent, const json *value)
{
	int rc = json_validate_take(parent, value);

	if (rc != 0)
		return rc;
	return node_depth(parent) + tree_height(value) > JSON_MAX_DEPTH ? -E2BIG : 0;
}

static void append_wrapper(json *parent, json *value)
{
	json *tail = parent->last;

	value->parent = parent;
	value->prev = tail;
	if (tail)
		tail->next = value;
	else
		parent->child = value;
	parent->last = value;
}

int json_object_add(json *object, const char *key, json *value)
{
	int rc;

	if (!json_is_object_value(object) || !key)
		return -EINVAL;
	rc = validate_string(key, strlen(key));
	if (rc == 0)
		rc = validate_attachment(object, value);
	if (rc != 0)
		return rc;
	if (!cJSON_AddItemToObject(object->backend, key, value->backend))
		return -ENOMEM;
	append_wrapper(object, value);
	return 0;
}

int json_array_add(json *array, json *value)
{
	int rc;

	if (!json_is_array_value(array))
		return -EINVAL;
	rc = validate_attachment(array, value);
	if (rc != 0)
		return rc;
	if (!cJSON_AddItemToArray(array->backend, value->backend))
		return -ENOMEM;
	append_wrapper(array, value);
	return 0;
}

static char *copy_backend_key(const char *key)
{
	size_t size = strlen(key) + 1;
	char *copy = cJSON_malloc(size);

	if (copy)
		memcpy(copy, key, size);
	return copy;
}

static void set_backend_key(cJSON *backend, char *owned_key)
{
	if (!(backend->type & cJSON_StringIsConst))
		cJSON_free(backend->string);
	backend->type &= ~cJSON_StringIsConst;
	backend->string = owned_key;
}

static int replace_child(json *parent, json *old, json *value, const char *key)
{
	char *copied_key = NULL;
	int rc = validate_attachment(parent, value);

	if (rc != 0)
		return rc;
	if (key) {
		copied_key = copy_backend_key(key);
		if (!copied_key)
			return -ENOMEM;
		set_backend_key(value->backend, copied_key);
	}
	(void)cJSON_ReplaceItemViaPointer(parent->backend, old->backend, value->backend);
	value->parent = parent;
	value->prev = old->prev;
	value->next = old->next;
	if (value->prev)
		value->prev->next = value;
	else
		parent->child = value;
	if (value->next)
		value->next->prev = value;
	else
		parent->last = value;
	free_wrappers(old);
	return 0;
}

static int replace_member(json *object, const char *key, json *value, bool case_sensitive)
{
	json *old;
	int rc;

	if (!json_is_object_value(object) || !key)
		return -EINVAL;
	rc = validate_string(key, strlen(key));
	if (rc != 0)
		return rc;
	old = find_member(object, key, case_sensitive);
	return old ? replace_child(object, old, value, key) : -ENOENT;
}

int json_object_replace(json *object, const char *key, json *value)
{
	return replace_member(object, key, value, true);
}

int json_object_replace_ci(json *object, const char *key, json *value)
{
	return replace_member(object, key, value, false);
}

int json_array_replace(json *array, int index, json *value)
{
	json *old;

	if (!json_is_array_value(array) || index < 0)
		return -EINVAL;
	old = json_at(array, index);
	return old ? replace_child(array, old, value, NULL) : -ENOENT;
}

int json_assign_take(json *target, json **owned)
{
	json *value;
	char *key = NULL;
	int rc;

	if (!target || !owned)
		return -EINVAL;
	value = *owned;
	rc = json_validate_take(target, value);
	if (rc != 0)
		return rc;
	if (node_depth(target) + tree_height(value) - 1 > JSON_MAX_DEPTH)
		return -E2BIG;
	if (target->backend->string) {
		key = copy_backend_key(target->backend->string);
		if (!key)
			return -ENOMEM;
	}
	set_backend_key(value->backend, key);
	if (target->parent)
		(void)cJSON_ReplaceItemViaPointer(target->parent->backend, target->backend,
						  value->backend);
	else
		cJSON_Delete(target->backend);
	for (json *child = target->child; child;) {
		json *next = child->next;

		free_wrappers(child);
		child = next;
	}
	target->backend = value->backend;
	target->child = value->child;
	target->last = value->last;
	for (json *child = target->child; child; child = child->next)
		child->parent = target;
	free(value);
	*owned = NULL;
	return 0;
}

json *json_object_detach(json *object, const char *key)
{
	return json_detach(object, json_member(object, key));
}

json *json_object_detach_ci(json *object, const char *key)
{
	return json_detach(object, json_member_ci(object, key));
}

int json_object_remove(json *object, const char *key)
{
	json *child;

	if (!json_is_object_value(object) || !key)
		return -EINVAL;
	child = json_object_detach(object, key);
	if (!child)
		return -ENOENT;
	json_free(child);
	return 0;
}

int json_object_remove_ci(json *object, const char *key)
{
	json *child;

	if (!json_is_object_value(object) || !key)
		return -EINVAL;
	child = json_object_detach_ci(object, key);
	if (!child)
		return -ENOENT;
	json_free(child);
	return 0;
}

int json_array_remove(json *array, int index)
{
	json *child;

	if (!json_is_array_value(array) || index < 0)
		return -EINVAL;
	child = json_detach(array, json_at(array, index));
	if (!child)
		return -ENOENT;
	json_free(child);
	return 0;
}

char *json_string_update(json *value, const char *text)
{
	int rc = validate_string(text, text ? strlen(text) : 0);
	char *copy;
	size_t size;

	if (!json_is_string_value(value) || rc != 0) {
		errno = rc ? -rc : EINVAL;
		return NULL;
	}
	size = strlen(text) + 1;
	copy = cJSON_malloc(size);
	if (!copy) {
		errno = ENOMEM;
		return NULL;
	}
	memcpy(copy, text, size);
	cJSON_free(value->backend->valuestring);
	value->backend->valuestring = copy;
	return copy;
}

int json_number_update(json *value, double number)
{
	if (!json_is_number_value(value))
		return -EINVAL;
	if (!isfinite(number))
		return -ERANGE;
	(void)cJSON_SetNumberValue(value->backend, number);
	return 0;
}

bool json_equal(const json *a, const json *b, bool case_sensitive)
{
	enum json_type type = json_typeof(a);

	if (!a || !b || type != json_typeof(b))
		return false;
	switch (type) {
	case JSON_NULL_TYPE:
		return true;
	case JSON_BOOL_TYPE:
		return json_bool_value(a) == json_bool_value(b);
	case JSON_NUMBER_TYPE:
		return json_number_value(a) == json_number_value(b);
	case JSON_STRING_TYPE:
		return strcmp(json_string_value(a), json_string_value(b)) == 0;
	case JSON_ARRAY_TYPE: {
		const json *left = a->child;
		const json *right = b->child;

		while (left && right) {
			if (!json_equal(left, right, case_sensitive))
				return false;
			left = left->next;
			right = right->next;
		}
		return left == right;
	}
	case JSON_OBJECT_TYPE:
		if (json_size(a) != json_size(b))
			return false;
		for (const json *left = a->child; left; left = left->next) {
			size_t occurrence = 0;
			const json *right = b->child;

			for (const json *previous = a->child; previous != left;
			     previous = previous->next) {
				if (key_equal(json_key(previous), json_key(left), case_sensitive))
					occurrence++;
			}
			for (; right; right = right->next) {
				if (key_equal(json_key(left), json_key(right), case_sensitive)) {
					if (!occurrence)
						break;
					occurrence--;
				}
			}
			if (!right || !json_equal(left, right, case_sensitive))
				return false;
		}
		return true;
	default:
		return false;
	}
}

static json *add_created(json *object, const char *key, json *created)
{
	int rc;

	if (!created)
		return NULL;
	rc = json_object_add(object, key, created);
	if (rc != 0) {
		json_free(created);
		errno = -rc;
		return NULL;
	}
	return created;
}

json *json_add_string(json *object, const char *key, const char *text)
{
	return add_created(object, key, json_string(text));
}

json *json_add_number(json *object, const char *key, double number)
{
	return add_created(object, key, json_number(number));
}

json *json_add_bool(json *object, const char *key, bool value)
{
	return add_created(object, key, json_boolean(value));
}

json *json_add_null(json *object, const char *key)
{
	return add_created(object, key, json_null());
}

json *json_add_array(json *object, const char *key)
{
	return add_created(object, key, json_array());
}

json *json_add_object(json *object, const char *key)
{
	return add_created(object, key, json_object());
}

struct json_value json_arg_null(void)
{
	return (struct json_value){.kind = JSON_VALUE_NULL};
}

struct json_value json_arg_bool(bool value)
{
	return (struct json_value){.kind = JSON_VALUE_BOOL, .data.boolean = value};
}

struct json_value json_arg_int(int64_t value)
{
	return (struct json_value){.kind = JSON_VALUE_INT, .data.integer = value};
}

struct json_value json_arg_uint(uint64_t value)
{
	return (struct json_value){.kind = JSON_VALUE_UINT, .data.unsigned_integer = value};
}

struct json_value json_arg_number(double value)
{
	return (struct json_value){.kind = JSON_VALUE_NUMBER, .data.number = value};
}

struct json_value json_arg_string(const char *value)
{
	return (struct json_value){.kind = JSON_VALUE_STRING, .data.string = value};
}

struct json_value json_arg_node(const json *value)
{
	return (struct json_value){.kind = JSON_VALUE_NODE, .data.node = value};
}

struct json_value json_arg_value(struct json_value value)
{
	return value;
}

static cJSON *take_backend_string(char *text)
{
	cJSON *value;

	if (!text)
		return NULL;
	value = cJSON_CreateStringReference(text);
	if (!value) {
		free(text);
		return NULL;
	}
	value->type &= ~cJSON_IsReference;
	return value;
}

static char *print_backend_arena(struct arena *arena, const cJSON *value)
{
	struct arena_cleanup *cleanup;
	char *text;

	if (!arena || !value)
		return NULL;
	text = print_backend(value, false);
	if (!text)
		return NULL;
	cleanup = arena_cleanup_add(arena, 0);
	if (!cleanup) {
		cJSON_free(text);
		return NULL;
	}
	cleanup->handler = cJSON_free;
	cleanup->data = text;
	return text;
}
