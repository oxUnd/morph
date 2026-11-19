#ifndef MORPH_EXT_SDK_H
#define MORPH_EXT_SDK_H

#include <stddef.h>
#include <stdint.h>
#include <nb/log.h>
#include <morph/ext_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXT_CONTEXT_ABI_VERSION 1u
#define EXT_SDK_VERSION "1.1.0"

/* Legacy opaque views; use host services instead of private host layouts. */
struct config;
struct tool_operation;
struct ext_manifest;
struct nb_toml_value;
struct model_chat_options;
struct model_image_chat_options;
struct image_result;
struct video_result;
struct ask_user_options;

typedef int (*ext_stream_fn)(const char *text, void *user_data);

struct ext_paths {
	const char *workdir;
	const char *output_dir;
	const char *extension_dir;
	const char *data_dir;
	/* Private scratch space, removed when the call returns. */
	const char *temp_dir;
};

struct ext_call_context {
	const char *user_id;
	const char *session_id;
	const char *agent_id;
	int agent_depth;
	const char *turn_id;
	const char *tool_call_id;
	/* Absolute monotonic milliseconds; zero means no deadline. */
	int64_t deadline_ms;
};

struct ext_image_request {
	const char *prompt;
	const char *style;
	const char *size;
	const char **reference_paths;
	int reference_count;
};

struct ext_video_request {
	const char *prompt;
	const char **image_paths;
	int image_count;
	const char **video_paths;
	int video_count;
	const char **audio_paths;
	int audio_count;
	int generate_audio;
	int duration_seconds;
};

/* All callbacks are synchronous and bound to the invoking user/agent. Inputs
 * are borrowed until return. Callback/user_data pairs must not outlive ext_run_v2.
 * check_cancelled returns NB_OK, NB_ERR_CANCELLED or NB_ERR_TIMEOUT. Models use
 * request-local clients with the caller's cancellation and usage/budget sinks.
 * Serialize host calls across extension workers. ask_user returns malloc-owned
 * answers/strings, including on failure; free each
 * string and then the array. Artifact paths must survive the call's temp_dir. */
struct ext_host_api {
	uint32_t abi_version;
	size_t struct_size;
	int (*check_cancelled)(void *host_data);
	void (*log)(void *host_data, enum nb_log_level level, const char *message);
	int (*emit_stream)(void *host_data, const char *kind, const char *text);
	int (*check_operation)(void *host_data, const struct tool_operation *operation);
	int (*authorize_path)(void *host_data, enum tool_path_op operation,
			      const char *path, char *resolved, size_t capacity);
	int (*chat)(void *host_data, const char *system_prompt, const char **messages,
		    int count, const struct model_chat_options *options,
		    ext_stream_fn callback, void *user_data);
	int (*vision)(void *host_data, const char *system_prompt, const char *prompt,
		      const char *image_path, const struct model_image_chat_options *options,
		      ext_stream_fn callback, void *user_data);
	int (*image)(void *host_data, const struct ext_image_request *request,
		     struct image_result *result);
	int (*video)(void *host_data, const struct ext_video_request *request,
		     struct video_result *result);
	int (*ask_user)(void *host_data, const char *question,
			const char *const *choices, int count, const char *selection_mode,
			int min_choices, int max_choices, const struct ask_user_options *options,
			char ***answers, int *answer_count);
	int (*add_artifact)(void *host_data, enum tool_artifact_kind kind,
			    const char *path, int width, int height, int duration_seconds);
	/* Borrowed string from the effective per-call config snapshot. Supports
	 * model.{text,vision,image,video}.{provider,adapter,model,api_base,
	 * api_key,api_key_env,extra_body_json}. Unknown paths return NULL;
	 * configured empty strings remain empty. Never free or retain the view. */
	const char *(*get_config_string)(void *host_data, const char *path);
};

/* Check appended callbacks before reading them, including on older ABI-1 hosts. */
#define EXT_HOST_HAS_CALLBACK(host, member) \
	((host) && (host)->struct_size >= \
	 offsetof(struct ext_host_api, member) + sizeof((host)->member) && \
	 (host)->member)

/* The host owns this object and every view it contains until ext_run_v2 returns.
 * config is a legacy opaque snapshot; read values through get_config_string.
 * settings is the optional [config] table in the manifest. No field transfers
 * ownership. Join extension workers before returning; never retain host_data.
 * Private config/manifest/model layouts are not part of this SDK. */
struct ext_context {
	uint32_t abi_version;
	size_t struct_size;
	const struct config *config;
	const struct ext_manifest *manifest;
	const struct nb_toml_value *settings;
	struct nb_log *logger;
	struct ext_paths paths;
	struct ext_call_context call;
	struct permission_policy permissions;
	const struct ext_host_api *host;
	void *host_data;
};

typedef int (*ext_run_v2_fn)(const struct ext_context *context,
			     const char *args_json, char **result_json);

#ifdef __cplusplus
}
#endif

#endif
