#ifndef MORPH_TOOL_REGISTRY_H
#define MORPH_TOOL_REGISTRY_H

#include "agent/tool_result.h"
#include "util/strmap.h"
#include "util/array.h"
#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOOL_NAME_MAX 512
#define TOOL_DESC_MAX 8192
#define TOOL_SCHEMA_MAX 8192
#define TOOL_FORMAT_MAX 16384
#define TOOL_REGISTRY_INITIAL_CAP 128
#define TOOL_DISABLED_MAX 32

#define TOOL_FLAG_READONLY 0x01
#define TOOL_FLAG_INTERNAL_APPROVAL 0x02
#define TOOL_FLAG_DYNAMIC 0x04
#define TOOL_FLAG_MEDIA 0x08
#define TOOL_FLAG_READONLY_CONFIGURED 0x10
#define TOOL_EMPTY_INPUT_SCHEMA                                                                    \
	"{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"
#define TOOL_OBJECT_OUTPUT_SCHEMA "{\"type\":\"object\",\"properties\":{}}"

enum tool_origin {
	TOOL_ORIGIN_BUILTIN,
	TOOL_ORIGIN_DYNAMIC_SESSION,
	TOOL_ORIGIN_DYNAMIC_PERSISTENT,
	TOOL_ORIGIN_MCP,
	TOOL_ORIGIN_EXT,
};

enum tool_input_kind {
	TOOL_INPUT_JSON = 0,
	TOOL_INPUT_TEXT,
};

struct morph_log;

/* Descriptor strings are borrowed views unless created by tool_desc_clone(). */
struct tool_desc {
	const char *name;
	const char *title;
	const char *description;
	const char *input_schema;
	const char *output_schema;
	enum tool_input_kind input_kind;
	const char *input_format;
};

/* Clone into an empty owner; failure leaves the output empty. */
int tool_desc_clone(const struct tool_desc *source, struct tool_desc *out);
void tool_desc_cleanup(struct tool_desc *owned);
/* Release the owned destination, move source into it, and clear source.
 * Moving an object to itself leaves it unchanged. */
void tool_desc_move(struct tool_desc *out, struct tool_desc *source);

typedef int (*tool_exec_fn)(const char *args_json, struct tool_result *result, void *user_data);
typedef void (*tool_user_data_destroy_fn)(void *user_data);

struct prompt_context_input;
typedef void (*tool_environment_fn)(void *user_data, struct prompt_context_input *out);

typedef int (*tool_background_list_fn)(void *user_data, int64_t owner, cJSON **out);
typedef int (*tool_background_wait_fn)(void *user_data, int64_t owner, const char *turn_id,
				       int (*cancelled)(void *), void *cancel_data, cJSON **out);
typedef void (*tool_background_cancel_fn)(void *user_data, int64_t owner, const char *turn_id);

struct tool_spec {
	enum tool_origin origin;
	const char *name;
	const char *title;
	const char *description;
	const char *input_schema;
	const char *output_schema;
	enum tool_input_kind input_kind;
	const char *input_format;
	tool_exec_fn exec;
	tool_environment_fn get_environment;
	tool_background_list_fn background_list;
	tool_background_wait_fn background_wait;
	tool_background_cancel_fn background_cancel;
	void *user_data;
	tool_user_data_destroy_fn user_data_destroy;
	unsigned int flags;
	int timeout_seconds;
};

struct tool_binding;

struct tool_entry {
	struct tool_binding *binding;
	struct tool_desc desc;
	tool_exec_fn exec;
	tool_environment_fn get_environment;
	tool_background_list_fn background_list;
	tool_background_wait_fn background_wait;
	tool_background_cancel_fn background_cancel;
	void *user_data;
	tool_user_data_destroy_fn user_data_destroy;
	enum tool_origin origin;
	unsigned int flags;
	int timeout_seconds;
};

struct tool_context;

/* Retained execution capabilities, independent of movable registry entries.
 * Descriptors remain registry-owned and are deliberately absent from this view.
 * Acquire into an unowned handle; release after callbacks and user data are done. */
struct tool_handle {
	struct tool_binding *binding;
	struct tool_context *permission_context;
	tool_exec_fn exec;
	tool_environment_fn get_environment;
	tool_background_list_fn background_list;
	tool_background_wait_fn background_wait;
	tool_background_cancel_fn background_cancel;
	void *user_data;
	enum tool_origin origin;
	unsigned int flags;
	int timeout_seconds;
	int disabled;
};

struct tool_registry {
	struct tool_context *permission_context;
	pthread_mutex_t mutex;
	pthread_mutex_t mutation_mutex;
	int mutex_ready;
	int init_error;
	struct morph_log *logger;
	struct tool_entry *entries;
	morph_array_t storage;
	int count;
	char disabled[TOOL_DISABLED_MAX][TOOL_NAME_MAX];
	int disabled_count;
	morph_strmap_t by_name;
	morph_strmap_t disabled_by_name;
};

/* Recursive data lock: raw entry views require this lock and expire on mutation.
 * Structural writes acquire mutation_mutex before this lock; do not call them
 * while holding only a raw data lock. Cleanup requires joined execution owners. */
void tool_registry_lock(struct tool_registry *reg);
void tool_registry_unlock(struct tool_registry *reg);
/* Registration copies descriptors and borrows user_data. Only _take transfers
 * destruction responsibility: success clears source user_data/destroy under the
 * registry lock; failure leaves the source unchanged. Descriptor strings remain
 * borrowed in tool_spec, and tool_spec_cleanup releases only owned user_data. */
void tool_spec_cleanup(struct tool_spec *spec);
int tool_register_take(struct tool_registry *reg, struct tool_spec *source);
/* Atomic replacement requires replace_flags. One means replacement, zero insertion. */
int tool_register_replace_take(struct tool_registry *reg, struct tool_spec *source,
			       unsigned int replace_flags);
int tool_register_replace_borrow(struct tool_registry *reg, const struct tool_spec *spec,
				 unsigned int replace_flags);
/* Source entry must remain stable during this call; shares execution ownership. */
int tool_register_copy(struct tool_registry *out, const struct tool_entry *entry);
/* Shares retained tool bindings, with owned descriptors. out must be initialized
 * empty and exclusively owned during copying. Source is locked internally.
 * Failure rolls back additions and preserves the initialized empty target. */
int tool_registry_copy(struct tool_registry *out, struct tool_registry *source);
void tool_registry_init(struct morph_log *logger, struct tool_registry *reg);
void tool_registry_cleanup(struct tool_registry *reg);
const char *tool_origin_name(enum tool_origin origin);
int tool_entry_set_descriptor_copy(struct tool_entry *entry, const struct tool_spec *spec);
int tool_register_borrow(struct tool_registry *reg, const struct tool_spec *spec);
int tool_unregister(struct tool_registry *reg, const char *name);
struct tool_entry *tool_borrow(struct tool_registry *reg, const char *name);
/* Copies capabilities under the registry lock and retains their user data.
 * A disabled tool is returned with disabled set; dispatch must enforce it.
 * out must be zero-initialized and empty; occupied handles are preserved.
 * Failure leaves an empty handle. No allocation or descriptor copy is needed. */
int tool_registry_acquire(struct tool_registry *reg, const char *name, struct tool_handle *out);
void tool_handle_release(struct tool_handle *handle);
int tool_set_timeout(struct tool_registry *reg, const char *name, int timeout_seconds);
int tool_timeout_seconds(struct tool_registry *reg, const char *name);
void tool_entry_cleanup_user_data(struct tool_registry *reg);
int tool_disable(struct tool_registry *reg, const char *name);
int tool_is_disabled(const struct tool_registry *reg, const char *name);
int tool_is_readonly(struct tool_registry *reg, const char *name);
int tool_has_flag(struct tool_registry *reg, const char *name, unsigned int flag);

#ifdef __cplusplus
}
#endif

#endif
