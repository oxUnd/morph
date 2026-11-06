#include "tool_registry.h"

#include "util/arena.h"
#include "util/error.h"
#include "util/log.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

/* Execution references own user data independently of movable registry entries. */
struct tool_binding {
	unsigned int refs;
	void *data;
	tool_user_data_destroy_fn destroy;
};

static int tool_is_disabled_locked(const struct tool_registry *reg, const char *name);

/** \brief Release one binding; destroy user data only after its final borrower. */
static void tool_binding_release(struct tool_binding *binding)
{
	if (binding != NULL && __atomic_sub_fetch(&binding->refs, 1, __ATOMIC_ACQ_REL) == 0) {
		if (binding->destroy != NULL && binding->data != NULL) {
			binding->destroy(binding->data);
		}
		free(binding);
	}
}

/** \brief Lock raw registry access; recursive for mutation tool callbacks. */
void tool_registry_lock(struct tool_registry *reg)
{
	if (reg != NULL && reg->mutex_ready) {
		pthread_mutex_lock(&reg->mutex);
	}
}

/** \brief End raw registry access. */
void tool_registry_unlock(struct tool_registry *reg)
{
	if (reg != NULL && reg->mutex_ready) {
		pthread_mutex_unlock(&reg->mutex);
	}
}

void tool_registry_init(struct morph_log *logger, struct tool_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
	reg->logger = logger;
	pthread_mutexattr_t attributes;
	int lock_rc = pthread_mutexattr_init(&attributes);
	if (lock_rc == 0) {
		lock_rc = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
		if (lock_rc == 0) {
			lock_rc = pthread_mutex_init(&reg->mutex, &attributes);
		}
		if (lock_rc == 0) {
			lock_rc = pthread_mutex_init(&reg->mutation_mutex, &attributes);
			if (lock_rc != 0) {
				pthread_mutex_destroy(&reg->mutex);
			}
		}
		pthread_mutexattr_destroy(&attributes);
	}
	reg->mutex_ready = lock_rc == 0;
	reg->init_error = -lock_rc;
	(void)morph_strmap_init(&reg->by_name, TOOL_REGISTRY_INITIAL_CAP);
	(void)morph_strmap_init(&reg->disabled_by_name, TOOL_DISABLED_MAX);
}

void tool_registry_cleanup(struct tool_registry *reg)
{
	tool_entry_cleanup_user_data(reg);
	if (!reg)
		return;
	for (int i = 0; i < reg->count; i++)
		arena_destroy(reg->entries[i].descriptor_arena);
	morph_array_cleanup(&reg->storage);
	reg->entries = NULL;
	reg->count = 0;
	morph_strmap_cleanup(&reg->by_name);
	morph_strmap_cleanup(&reg->disabled_by_name);
	if (reg->mutex_ready) {
		pthread_mutex_destroy(&reg->mutex);
		pthread_mutex_destroy(&reg->mutation_mutex);
	}
	reg->mutex_ready = 0;
}

void tool_entry_cleanup_user_data(struct tool_registry *reg)
{
	if (reg == NULL) {
		return;
	}
	for (int i = 0; i < reg->count; i++) {
		tool_binding_release(reg->entries[i].binding);
		reg->entries[i].binding = NULL;
		reg->entries[i].user_data = NULL;
		reg->entries[i].user_data_destroy = NULL;
	}
}

const char *tool_origin_name(enum tool_origin origin)
{
	switch (origin) {
	case TOOL_ORIGIN_BUILTIN:
		return "system built-in";
	case TOOL_ORIGIN_DYNAMIC_SESSION:
		return "dynamic session";
	case TOOL_ORIGIN_DYNAMIC_PERSISTENT:
		return "dynamic persistent";
	case TOOL_ORIGIN_MCP:
		return "mcp";
	case TOOL_ORIGIN_EXT:
		return "ext";
	default:
		return "unknown";
	}
}

static int find_tool(struct tool_registry *reg, const char *name)
{
	struct tool_entry *e;

	if (!reg || !name)
		return -1;
	e = (struct tool_entry *)morph_strmap_get(&reg->by_name, name);
	if (e)
		return (int)(e - reg->entries);
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->entries[i].desc.name, name) == 0)
			return i;
	}
	return -1;
}

static void rebuild_tool_name_index(struct tool_registry *reg)
{
	if (!reg)
		return;
	morph_strmap_clear(&reg->by_name);
	for (int i = 0; i < reg->count; i++)
		(void)morph_strmap_set(&reg->by_name, reg->entries[i].desc.name, &reg->entries[i]);
}

static void tool_enable_remove_disabled(struct tool_registry *reg, const char *name)
{
	if (!reg || !name)
		return;
	for (int i = 0; i < reg->disabled_count; i++) {
		if (strcmp(reg->disabled[i], name) != 0)
			continue;
		for (int j = i; j < reg->disabled_count - 1; j++) {
			memcpy(reg->disabled[j], reg->disabled[j + 1], sizeof(reg->disabled[j]));
		}
		memset(reg->disabled[reg->disabled_count - 1], 0,
		       sizeof(reg->disabled[reg->disabled_count - 1]));
		reg->disabled_count--;
		morph_strmap_clear(&reg->disabled_by_name);
		for (int j = 0; j < reg->disabled_count; j++) {
			(void)morph_strmap_set(&reg->disabled_by_name, reg->disabled[j],
					       reg->disabled[j]);
		}
		return;
	}
}

static int tool_schema_validate(const char *schema, int require_closed)
{
	cJSON *root;
	cJSON *type;
	cJSON *props;
	cJSON *required;
	cJSON *item;
	int rc = 0;

	if (!schema || !*schema)
		return -EINVAL;
	root = cJSON_Parse(schema);
	if (!root)
		return MORPH_ERR_PARSE;
	if (!cJSON_IsObject(root)) {
		rc = -EINVAL;
		goto out;
	}
	type = cJSON_GetObjectItem(root, "type");
	props = cJSON_GetObjectItem(root, "properties");
	if (!cJSON_IsString(type) || strcmp(type->valuestring, "object") != 0 ||
	    (props && !cJSON_IsObject(props))) {
		rc = -EINVAL;
		goto out;
	}
	if (require_closed) {
		cJSON *additional = cJSON_GetObjectItem(root, "additionalProperties");
		if (!cJSON_IsFalse(additional)) {
			rc = -EINVAL;
			goto out;
		}
	}
	required = cJSON_GetObjectItem(root, "required");
	if (required && cJSON_IsArray(required)) {
		cJSON_ArrayForEach(item, required)
		{
			if (!cJSON_IsString(item) ||
			    (require_closed && !cJSON_GetObjectItem(props, item->valuestring))) {
				rc = -EINVAL;
				goto out;
			}
		}
	} else if (required) {
		rc = -EINVAL;
	}

out:
	cJSON_Delete(root);
	return rc;
}

int tool_entry_set_descriptor(struct tool_entry *entry, const struct tool_spec *spec)
{
	if (!entry || !spec || !spec->name)
		return -EINVAL;
	const char *values[] = {spec->name,	    spec->title,	 spec->description,
				spec->input_schema, spec->output_schema, spec->input_format};
	struct tool_desc desc = {0};
	const char **fields[] = {&desc.name,	     &desc.title,	  &desc.description,
				 &desc.input_schema, &desc.output_schema, &desc.input_format};
	size_t bytes = 0;
	struct arena *arena;

	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		size_t len = values[i] ? strlen(values[i]) : 0;

		if (bytes > SIZE_MAX - sizeof(entry->descriptor_arena) ||
		    len > SIZE_MAX - bytes - sizeof(entry->descriptor_arena))
			return -EOVERFLOW;
		bytes += len + sizeof(entry->descriptor_arena);
	}
	arena = arena_create(bytes);
	if (!arena)
		return -ENOMEM;
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		*fields[i] = arena_strdup(arena, values[i] ? values[i] : "");
		if (!*fields[i]) {
			arena_destroy(arena);
			return -ENOMEM;
		}
	}
	desc.input_kind = spec->input_kind;
	arena_destroy(entry->descriptor_arena);
	entry->descriptor_arena = arena;
	entry->desc = desc;
	return 0;
}

static int tool_register_unlocked(struct tool_registry *reg, const struct tool_spec *spec)
{
	int rc;

	if (!reg || !spec || !spec->name || !spec->exec)
		return -EINVAL;
	if (reg->count >= INT_MAX)
		return -EOVERFLOW;
	if (find_tool(reg, spec->name) >= 0)
		return -EEXIST;
	if (spec->input_kind == TOOL_INPUT_JSON) {
		rc = tool_schema_validate(spec->input_schema, 0);
		if (rc != 0)
			return rc;
	} else if (spec->input_kind != TOOL_INPUT_TEXT) {
		return -EINVAL;
	}
	rc = tool_schema_validate(spec->output_schema, 0);
	if (rc != 0)
		return rc;
	struct tool_entry value = {0};
	struct tool_entry *e;

	rc = tool_entry_set_descriptor(&value, spec);
	if (rc != 0)
		return rc;
	if (!reg->storage.elts)
		rc = morph_array_init(&reg->storage, 8, sizeof(value));
	if (rc != 0) {
		arena_destroy(value.descriptor_arena);
		return rc;
	}
	e = morph_array_push(&reg->storage);
	if (!e) {
		arena_destroy(value.descriptor_arena);
		return -ENOMEM;
	}
	reg->entries = reg->storage.elts;
	*e = value;
	e->exec = spec->exec;
	e->get_environment = spec->get_environment;
	e->background_list = spec->background_list;
	e->background_wait = spec->background_wait;
	e->background_cancel = spec->background_cancel;
	e->user_data = spec->user_data;
	e->user_data_destroy = spec->user_data_destroy;
	e->origin = spec->origin;
	e->flags = spec->flags;
	e->timeout_seconds = spec->timeout_seconds > 0 ? spec->timeout_seconds : 0;
	struct tool_binding *binding = calloc(1, sizeof(*binding));
	if (binding == NULL) {
		arena_destroy(e->descriptor_arena);
		memset(e, 0, sizeof(*e));
		morph_array_pop(&reg->storage);
		rebuild_tool_name_index(reg);
		return -ENOMEM;
	}
	binding->refs = 1;
	binding->data = spec->user_data;
	binding->destroy = spec->user_data_destroy;
	e->binding = binding;
	reg->count++;
	rebuild_tool_name_index(reg);
	log_dbg(reg ? reg->logger : NULL, "tool registered: %s", spec->name);
	return 0;
}

static int tool_unregister_unlocked(struct tool_registry *reg, const char *name)
{
	int idx;

	if (!reg || !name)
		return -EINVAL;
	idx = find_tool(reg, name);
	if (idx < 0)
		return -ENOENT;
	struct tool_binding *binding = reg->entries[idx].binding;
	struct arena *descriptor_arena = reg->entries[idx].descriptor_arena;
	for (int i = idx; i < reg->count - 1; i++)
		reg->entries[i] = reg->entries[i + 1];
	memset(&reg->entries[reg->count - 1], 0, sizeof(reg->entries[reg->count - 1]));
	reg->count--;
	morph_array_pop(&reg->storage);
	rebuild_tool_name_index(reg);
	tool_enable_remove_disabled(reg, name);
	log_dbg(reg ? reg->logger : NULL, "tool unregistered: %s", name);
	arena_destroy(descriptor_arena);
	tool_binding_release(binding);
	return 0;
}

/** \brief Register an owned descriptor; user data transfers only on success. */
int tool_register(struct tool_registry *reg, const struct tool_spec *spec)
{
	int rc;
	if (reg == NULL) {
		return -EINVAL;
	}
	if (reg->init_error != 0) {
		return reg->init_error;
	}
	if (!reg->mutex_ready) {
		return MORPH_ERR_NOT_INITIALIZED;
	}
	pthread_mutex_lock(&reg->mutation_mutex);
	tool_registry_lock(reg);
	rc = tool_register_unlocked(reg, spec);
	tool_registry_unlock(reg);
	pthread_mutex_unlock(&reg->mutation_mutex);
	return rc;
}

/** \brief Remove discovery ownership; retained executions remain valid. */
int tool_unregister(struct tool_registry *reg, const char *name)
{
	int rc;
	struct tool_binding *binding = NULL;
	if (reg == NULL) {
		return -EINVAL;
	}
	if (reg->init_error != 0) {
		return reg->init_error;
	}
	if (!reg->mutex_ready) {
		return MORPH_ERR_NOT_INITIALIZED;
	}
	pthread_mutex_lock(&reg->mutation_mutex);
	tool_registry_lock(reg);
	struct tool_entry *entry = tool_lookup(reg, name);
	if (entry != NULL) {
		binding = entry->binding;
		if (binding != NULL) {
			__atomic_add_fetch(&binding->refs, 1, __ATOMIC_RELAXED);
		}
	}
	rc = tool_unregister_unlocked(reg, name);
	tool_registry_unlock(reg);
	pthread_mutex_unlock(&reg->mutation_mutex);
	tool_binding_release(binding);
	return rc;
}

/**
 * \brief Prepare a replacement before atomically publishing its owned binding.
 * \param[in,out] reg: Borrowed registry, internally synchronized.
 * \param[in] spec: Borrowed descriptor; user data transfers only on success.
 * \param[in] replace_flags: Flags required on an existing entry.
 * \return One for replacement, zero for insertion, or a negative error.
 * \note Old executions retain their binding; destruction occurs outside the short lock.
 */
int tool_register_replace(struct tool_registry *reg, const struct tool_spec *spec,
			  unsigned int replace_flags)
{
	struct tool_registry prepared;
	struct tool_entry previous = {0};
	int rc;
	int replaced = 0;
	if (reg == NULL || spec == NULL) {
		return -EINVAL;
	}
	if (reg->init_error != 0) {
		return reg->init_error;
	}
	if (!reg->mutex_ready) {
		return MORPH_ERR_NOT_INITIALIZED;
	}
	tool_registry_init(reg->logger, &prepared);
	rc = tool_register(&prepared, spec);
	if (rc != 0) {
		tool_registry_cleanup(&prepared);
		return rc;
	}
	pthread_mutex_lock(&reg->mutation_mutex);
	tool_registry_lock(reg);
	int index = find_tool(reg, spec->name);
	if (index >= 0) {
		if ((reg->entries[index].flags & replace_flags) != replace_flags) {
			rc = -EEXIST;
		} else {
			previous = reg->entries[index];
			reg->entries[index] = prepared.entries[0];
			memset(&prepared.entries[0], 0, sizeof(prepared.entries[0]));
			rebuild_tool_name_index(reg);
			replaced = 1;
		}
	} else {
		rc = tool_register_unlocked(reg, spec);
	}
	tool_registry_unlock(reg);
	pthread_mutex_unlock(&reg->mutation_mutex);
	/* The candidate's duplicate ownership is never consumed on insertion/failure. */
	if (prepared.entries[0].binding != NULL) {
		prepared.entries[0].binding->destroy = NULL;
	}
	tool_registry_cleanup(&prepared);
	arena_destroy(previous.descriptor_arena);
	tool_binding_release(previous.binding);
	return rc != 0 ? rc : replaced;
}

/**
 * \brief Register a descriptor copy retaining the original user-data binding.
 * \param[in,out] out: Initialized target, exclusively borrowed during copying.
 * \param[in] entry: Borrowed source view; caller keeps it stable until return.
 * \return Zero on success, negative error without consuming the source on failure.
 */
int tool_register_copy(struct tool_registry *out, const struct tool_entry *entry)
{
	if (out == NULL || entry == NULL) {
		return -EINVAL;
	}
	struct tool_spec spec = {
	    .origin = entry->origin,
	    .name = entry->desc.name,
	    .title = entry->desc.title,
	    .description = entry->desc.description,
	    .input_schema = entry->desc.input_schema,
	    .output_schema = entry->desc.output_schema,
	    .input_kind = entry->desc.input_kind,
	    .input_format = entry->desc.input_format,
	    .exec = entry->exec,
	    .get_environment = entry->get_environment,
	    .background_list = entry->background_list,
	    .background_wait = entry->background_wait,
	    .background_cancel = entry->background_cancel,
	    .user_data = entry->user_data,
	    .flags = entry->flags,
	    .timeout_seconds = entry->timeout_seconds,
	};
	int rc = tool_register(out, &spec);
	if (rc == 0) {
		struct tool_entry *copy = tool_lookup(out, spec.name);
		tool_binding_release(copy->binding);
		copy->binding = entry->binding;
		if (copy->binding != NULL) {
			__atomic_add_fetch(&copy->binding->refs, 1, __ATOMIC_RELAXED);
		}
	}
	return rc;
}

/**
 * \brief Copy a registry, retaining user data and copying descriptors.
 * \param[in,out] out: Initialized distinct target, exclusively borrowed until return.
 * \param[in] source: Borrowed registry, locked internally for a coherent snapshot.
 * \return Zero on success, negative error otherwise; partial copies require cleanup.
 */
int tool_registry_copy(struct tool_registry *out, struct tool_registry *source)
{
	int rc = 0;
	if (out == NULL || source == NULL || out == source) {
		return -EINVAL;
	}
	tool_registry_lock(source);
	out->permission_context = source->permission_context;
	for (int i = 0; i < source->count && rc == 0; i++) {
		const struct tool_entry *entry = &source->entries[i];
		rc = tool_register_copy(out, entry);
	}
	for (int i = 0; i < source->disabled_count && rc == 0; i++) {
		rc = tool_disable(out, source->disabled[i]);
	}
	tool_registry_unlock(source);
	return rc;
}

struct tool_entry *tool_lookup(struct tool_registry *reg, const char *name)
{
	if (!reg || !name)
		return NULL;
	int idx = find_tool(reg, name);
	if (idx < 0)
		return NULL;
	return &reg->entries[idx];
}

int tool_registry_acquire(struct tool_registry *reg, const char *name, struct tool_handle *out)
{
	struct tool_entry *entry;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!reg || !name)
		return -EINVAL;
	if (reg->init_error != 0)
		return reg->init_error;
	if (!reg->mutex_ready)
		return MORPH_ERR_NOT_INITIALIZED;
	tool_registry_lock(reg);
	entry = tool_lookup(reg, name);
	if (!entry) {
		tool_registry_unlock(reg);
		return -ENOENT;
	}
	out->binding = entry->binding;
	if (out->binding)
		__atomic_add_fetch(&out->binding->refs, 1, __ATOMIC_RELAXED);
	out->permission_context = reg->permission_context;
	out->exec = entry->exec;
	out->get_environment = entry->get_environment;
	out->background_list = entry->background_list;
	out->background_wait = entry->background_wait;
	out->background_cancel = entry->background_cancel;
	out->user_data = entry->user_data;
	out->origin = entry->origin;
	out->flags = entry->flags;
	out->timeout_seconds = entry->timeout_seconds;
	out->disabled = tool_is_disabled_locked(reg, name);
	tool_registry_unlock(reg);
	return 0;
}

void tool_handle_release(struct tool_handle *handle)
{
	if (!handle)
		return;
	tool_binding_release(handle->binding);
	memset(handle, 0, sizeof(*handle));
}

static int tool_set_timeout_locked(struct tool_registry *reg, const char *name, int timeout_seconds)
{
	struct tool_entry *e;

	if (!reg || !name)
		return -EINVAL;
	e = tool_lookup(reg, name);
	if (!e)
		return -ENOENT;
	e->timeout_seconds = timeout_seconds > 0 ? timeout_seconds : 0;
	return 0;
}

static int tool_timeout_seconds_locked(struct tool_registry *reg, const char *name)
{
	struct tool_entry *e;

	if (!reg || !name)
		return 0;
	e = tool_lookup(reg, name);
	if (!e)
		return 0;
	return e->timeout_seconds;
}

static int tool_disable_locked(struct tool_registry *reg, const char *name)
{
	if (!reg || !name)
		return -EINVAL;
	if (reg->disabled_count >= TOOL_DISABLED_MAX)
		return -ENOSPC;
	if (tool_is_disabled(reg, name))
		return 0;
	strncpy(reg->disabled[reg->disabled_count], name, TOOL_NAME_MAX - 1);
	(void)morph_strmap_set(&reg->disabled_by_name, reg->disabled[reg->disabled_count],
			       reg->disabled[reg->disabled_count]);
	reg->disabled_count++;
	log_info(reg ? reg->logger : NULL, "tool disabled: %s", name);
	return 0;
}

static int tool_is_disabled_locked(const struct tool_registry *reg, const char *name)
{
	if (!reg || !name)
		return 0;
	if (morph_strmap_contains(&reg->disabled_by_name, name))
		return 1;
	for (int i = 0; i < reg->disabled_count; i++) {
		if (strcmp(reg->disabled[i], name) == 0)
			return 1;
	}
	return 0;
}

int tool_is_readonly(struct tool_registry *reg, const char *name)
{
	return tool_has_flag(reg, name, TOOL_FLAG_READONLY);
}

static int tool_has_flag_locked(struct tool_registry *reg, const char *name, unsigned int flag)
{
	if (!reg || !name)
		return 0;
	struct tool_entry *e = tool_lookup(reg, name);
	if (!e)
		return 0;
	return (e->flags & flag) != 0;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_set_timeout(struct tool_registry *reg, const char *name, int timeout_seconds)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_set_timeout_locked(reg, name, timeout_seconds);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_timeout_seconds(struct tool_registry *reg, const char *name)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_timeout_seconds_locked(reg, name);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_disable(struct tool_registry *reg, const char *name)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_disable_locked(reg, name);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_is_disabled(const struct tool_registry *reg, const char *name)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_is_disabled_locked(reg, name);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}

/** \brief Read or update registry metadata under its short data lock. */
int tool_has_flag(struct tool_registry *reg, const char *name, unsigned int flag)
{
	tool_registry_lock((struct tool_registry *)reg);
	int value = tool_has_flag_locked(reg, name, flag);
	tool_registry_unlock((struct tool_registry *)reg);
	return value;
}
