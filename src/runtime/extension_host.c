#include "extension_host.h"

#include "agent/tool_runtime.h"
#include "config/config.h"
#include "errors.h"
#include "ext/ext.h"
#include "models/image_gen.h"
#include "models/llm.h"
#include "models/video_gen.h"
#include "permission/context.h"
#include "sandbox/permission.h"
#include <nb/cleanup.h>
#include <nb/file.h>
#include <nb/hash.h>
#include <nb/time.h>
#include <nb/toml.h>
#include <nb/vec.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct runtime_extension_call {
	struct ext *extension;
	const struct tool_runtime_context *runtime;
	struct tool_result *result;
	struct ext_context context;
	struct config *config;
	nb_buf extension_dir;
	nb_buf data_dir;
	nb_buf temp_dir;
};

static void runtime_extension_call_cleanup(struct runtime_extension_call *call)
{
	if (call->temp_dir.len)
		(void)nb_file_remove_all(nb_buf_cstr(&call->temp_dir), 64, NULL);

	nb_buf_destroy(&call->temp_dir);
	nb_buf_destroy(&call->data_dir);
	nb_buf_destroy(&call->extension_dir);
	free(call->config);
}

static int runtime_extension_check_cancelled(void *host_data)
{
	const struct runtime_extension_call *call = host_data;
	int rc = http_wait_cancelable(&call->runtime->http, 0);

	if (call->context.call.deadline_ms > 0 &&
	    nb_time_monotonic_ms() >= call->context.call.deadline_ms)
		return NB_ERR_TIMEOUT;

	return rc;
}

static int runtime_extension_cancelled(void *host_data)
{
	return runtime_extension_check_cancelled(host_data) != NB_OK;
}

static void runtime_extension_log(void *host_data, enum nb_log_level level,
				   const char *message)
{
	const struct runtime_extension_call *call = host_data;

	nb_log_write(call->context.logger, level, "ext %s [%s/%s]: %s",
		     call->extension->manifest.name, call->context.call.turn_id,
		     call->context.call.tool_call_id, message ? message : "");
}

static int runtime_extension_emit_stream(void *host_data, const char *kind, const char *text)
{
	const struct runtime_extension_call *call = host_data;
	int rc = runtime_extension_check_cancelled(host_data);

	if (rc != NB_OK)
		return rc;

	return tool_runtime_emit_stream(call->runtime, call->extension->manifest.name, kind, text);
}

static int runtime_extension_check_operation(void *host_data,
					    const struct tool_operation *operation)
{
	const struct runtime_extension_call *call = host_data;
	int rc = runtime_extension_check_cancelled(host_data);

	if (!operation)
		return NB_ERR_INVAL;

	if (rc != NB_OK)
		return rc;

	struct tool_operation bound = *operation;
	bound.tool_name = call->extension->manifest.name;
	bound.principal = call->extension->manifest.name;
	return tool_context_check_operation(call->runtime->permission_context, &bound);
}

static int runtime_extension_authorize_path(void *host_data, enum tool_path_op operation,
					   const char *path, char *resolved, size_t capacity)
{
	const struct runtime_extension_call *call = host_data;
	struct tool_operation request = {
		.kind = operation == TOOL_PATH_READ ? TOOL_OP_PATH_READ :
			operation == TOOL_PATH_LIST ? TOOL_OP_PATH_LIST :
			operation == TOOL_PATH_WRITE ? TOOL_OP_PATH_WRITE : TOOL_OP_PATH_DELETE,
		.action = "Access an extension path",
		.target = path,
	};

	if (operation < TOOL_PATH_READ || operation > TOOL_PATH_DELETE || !path || !*path)
		return NB_ERR_INVAL;

	int rc = runtime_extension_check_operation(host_data, &request);
	if (rc != NB_OK)
		return rc;

	return tool_context_authorize_path(call->runtime->permission_context, operation,
					    path, resolved, capacity);
}

static int runtime_extension_model_prepare(struct runtime_extension_call *call,
					   const struct model *source, struct model *model)
{
	if (!source)
		return MORPH_ERR_NOT_CONFIGURED;

	struct tool_operation operation = {
		.kind = TOOL_OP_TOOL_CALL,
		.action = "Call an extension model service",
		.target = source->api_base,
		.network = 1,
	};
	int rc = runtime_extension_check_operation(call, &operation);
	if (rc != NB_OK)
		return rc;

	struct http_control control = call->runtime->http;
	control.interrupt_check = runtime_extension_cancelled;
	control.interrupt_user_data = call;
	(void)model_request_view_init(source, &control, model);
	if (call->runtime->models.text) {
		model->usage_sink = call->runtime->models.text->usage_sink;
		model->budget_sink = call->runtime->models.text->budget_sink;
	}

	if (call->context.call.deadline_ms > 0) {
		int64_t left = call->context.call.deadline_ms - nb_time_monotonic_ms();
		if (left <= 0)
			return NB_ERR_TIMEOUT;

		long seconds = (long)((left + 999) / 1000);
		if (model->timeout_seconds <= 0 || model->timeout_seconds > seconds)
			model->timeout_seconds = seconds;
	}

	return NB_OK;
}

struct runtime_extension_stream {
	struct runtime_extension_call *call;
	ext_stream_fn callback;
	void *user_data;
};

static int runtime_extension_stream_token(const char *text, void *user_data)
{
	struct runtime_extension_stream *stream = user_data;
	int rc = runtime_extension_check_cancelled(stream->call);

	if (rc != NB_OK)
		return rc;

	if (stream->callback)
		return stream->callback(text, stream->user_data);

	return runtime_extension_emit_stream(stream->call, "text", text);
}

static int runtime_extension_model_status(struct runtime_extension_call *call, int status)
{
	int rc = runtime_extension_check_cancelled(call);

	if (rc != NB_OK)
		return rc;

	if (status >= 400)
		return MORPH_ERR_API;

	return status < 0 ? status : NB_OK;
}

static int runtime_extension_chat(void *host_data, const char *system_prompt,
				   const char **messages, int count,
				   const struct model_chat_options *options,
				   ext_stream_fn callback, void *user_data)
{
	struct runtime_extension_call *call = host_data;
	struct model model = {0};
	struct model_chat_options opts = options ? *options : (struct model_chat_options){0};
	struct runtime_extension_stream stream = {call, callback, user_data};

	if (count < 0 || (count && !messages))
		return NB_ERR_INVAL;

	for (int i = 0; i < count; i++) {
		if (!messages[i])
			return NB_ERR_INVAL;
	}

	int rc = runtime_extension_model_prepare(call, call->runtime->models.text, &model);
	if (rc != NB_OK)
		return rc;

	if (!model.chat)
		return NB_ERR_UNSUPPORTED;

	if (model.timeout_seconds > 0 &&
	    (opts.timeout_seconds <= 0 || opts.timeout_seconds > model.timeout_seconds))
		opts.timeout_seconds = model.timeout_seconds;

	opts.usage_phase = "extension";
	rc = model.chat(&model, system_prompt, messages, count, &opts,
			 runtime_extension_stream_token, &stream);
	return runtime_extension_model_status(call, rc);
}

static int runtime_extension_vision(void *host_data, const char *system_prompt,
				     const char *prompt, const char *image_path,
				     const struct model_image_chat_options *options,
				     ext_stream_fn callback, void *user_data)
{
	struct runtime_extension_call *call = host_data;
	struct model model = {0};
	struct model_image_chat_options opts = options ? *options
		: (struct model_image_chat_options){0};
	struct runtime_extension_stream stream = {call, callback, user_data};
	char resolved[PATH_MAX];

	if (!prompt)
		return NB_ERR_INVAL;

	int rc = runtime_extension_authorize_path(call, TOOL_PATH_READ, image_path,
						 resolved, sizeof(resolved));
	if (rc == NB_OK)
		rc = runtime_extension_model_prepare(call, call->runtime->models.vision, &model);

	if (rc != NB_OK)
		return rc;

	if (!model.chat_with_image)
		return NB_ERR_UNSUPPORTED;

	if (model.timeout_seconds > 0 &&
	    (opts.timeout_seconds <= 0 || opts.timeout_seconds > model.timeout_seconds))
		opts.timeout_seconds = model.timeout_seconds;

	rc = model.chat_with_image(&model, system_prompt, prompt, resolved, &opts,
				   runtime_extension_stream_token, &stream);
	return runtime_extension_model_status(call, rc);
}

static void runtime_extension_references_cleanup(nb_vec *paths)
{
	for (size_t i = 0; i < paths->len; i++) {
		char **path = nb_vec_at(paths, i);
		free(*path);
	}

	nb_vec_destroy(paths);
}

static int runtime_extension_references(struct runtime_extension_call *call,
				       const char **paths, int count, int maximum,
				       nb_vec *resolved)
{
	if (count < 0 || count > maximum || (count && !paths))
		return NB_ERR_INVAL;

	char *item = NULL;
	nb_vec_init(resolved, sizeof(item));
	for (int i = 0; i < count; i++) {
		char path[PATH_MAX];
		int rc = runtime_extension_authorize_path(call, TOOL_PATH_READ, paths[i],
							 path, sizeof(path));
		if (rc != NB_OK)
			return rc;

		char *owned NB_CLEANUP(nb_charp_cleanup) = strdup(path);
		if (!owned)
			return NB_ERR_NOMEM;

		char **slot = nb_vec_push(resolved);
		if (!slot)
			return NB_ERR_NOMEM;

		*slot = nb_charp_steal(&owned);
	}

	return NB_OK;
}

static int runtime_extension_image(void *host_data, const struct ext_image_request *request,
				    struct image_result *result)
{
	struct runtime_extension_call *call = host_data;
	struct model model = {0};
	nb_vec references NB_CLEANUP(runtime_extension_references_cleanup) = {0};
	char output[PATH_MAX];

	if (!request || !request->prompt || !result)
		return NB_ERR_INVAL;

	int rc = runtime_extension_references(call, request->reference_paths,
		request->reference_count, IMAGE_GEN_MAX_REFERENCE_IMAGES, &references);
	if (rc == NB_OK)
		rc = runtime_extension_authorize_path(call, TOOL_PATH_WRITE,
			call->context.paths.output_dir, output, sizeof(output));

	if (rc == NB_OK)
		rc = runtime_extension_model_prepare(call, call->runtime->models.image, &model);

	if (rc != NB_OK)
		return rc;

	rc = image_gen_create(&model, request->prompt, request->style,
		request->size ? request->size : "2k", (const char **)references.data,
		request->reference_count, output, result);
	return runtime_extension_model_status(call, rc);
}

static int runtime_extension_video(void *host_data, const struct ext_video_request *request,
				    struct video_result *result)
{
	struct runtime_extension_call *call = host_data;
	struct model model = {0};
	nb_vec images NB_CLEANUP(runtime_extension_references_cleanup) = {0};
	nb_vec videos NB_CLEANUP(runtime_extension_references_cleanup) = {0};
	nb_vec audios NB_CLEANUP(runtime_extension_references_cleanup) = {0};
	char output[PATH_MAX];

	if (!request || !request->prompt || !result || request->generate_audio < -1 ||
	    request->generate_audio > 1 || request->duration_seconds < 0)
		return NB_ERR_INVAL;

	int rc = runtime_extension_references(call, request->image_paths,
		request->image_count, VIDEO_GEN_MAX_REFERENCE_IMAGES, &images);
	if (rc == NB_OK)
		rc = runtime_extension_references(call, request->video_paths,
			request->video_count, VIDEO_GEN_MAX_REFERENCE_VIDEOS, &videos);

	if (rc == NB_OK)
		rc = runtime_extension_references(call, request->audio_paths,
			request->audio_count, VIDEO_GEN_MAX_REFERENCE_AUDIOS, &audios);

	if (rc == NB_OK)
		rc = runtime_extension_authorize_path(call, TOOL_PATH_WRITE,
			call->context.paths.output_dir, output, sizeof(output));

	if (rc == NB_OK)
		rc = runtime_extension_model_prepare(call, call->runtime->models.video, &model);

	if (rc != NB_OK)
		return rc;

	rc = video_gen_create(&model, request->prompt, (const char **)images.data,
		request->image_count, (const char **)videos.data, request->video_count,
		(const char **)audios.data, request->audio_count, request->generate_audio,
		request->duration_seconds, output, result);
	return runtime_extension_model_status(call, rc);
}

static int runtime_extension_ask_user(void *host_data, const char *question,
	const char *const *choices, int count, const char *selection_mode,
	int min_choices, int max_choices, const struct ask_user_options *options,
	char ***answers, int *answer_count)
{
	struct runtime_extension_call *call = host_data;
	int rc = runtime_extension_check_cancelled(call);

	if (!question || !selection_mode || count < 0 || count > ASK_USER_CHOICES_MAX ||
	    (count && !choices) || min_choices < 0 || max_choices < min_choices ||
	    !answers || !answer_count ||
	    (strcmp(selection_mode, "single") && strcmp(selection_mode, "multiple")))
		return NB_ERR_INVAL;

	for (int i = 0; i < count; i++) {
		if (!choices[i])
			return NB_ERR_INVAL;
	}

	if (options) {
		if (options->recommended_count < 0 ||
		    options->recommended_count > ASK_USER_CHOICES_MAX)
			return NB_ERR_INVAL;

		for (int i = 0; i < options->recommended_count; i++) {
			if (options->recommended_indices[i] < 0 ||
			    options->recommended_indices[i] >= count)
				return NB_ERR_INVAL;
		}
	}

	if (*answers)
		return NB_ERR_EXISTS;

	if (rc != NB_OK)
		return rc;

	if (!call->runtime->ask_user_fn)
		return NB_ERR_UNSUPPORTED;

	struct ask_user_options opts = options ? *options : (struct ask_user_options){0};
	opts.cancel_token = call->runtime->http.cancel_token;
	return call->runtime->ask_user_fn(question, choices, count, selection_mode,
		min_choices, max_choices, &opts, answers, answer_count,
		call->runtime->ask_user_user_data);
}

static int runtime_extension_add_artifact(void *host_data, enum tool_artifact_kind kind,
	const char *path, int width, int height, int duration_seconds)
{
	struct runtime_extension_call *call = host_data;
	char resolved[PATH_MAX];

	if (kind < TOOL_ARTIFACT_FILE || kind > TOOL_ARTIFACT_VIDEO ||
	    width < 0 || height < 0 || duration_seconds < 0)
		return NB_ERR_INVAL;

	int rc = runtime_extension_authorize_path(call, TOOL_PATH_READ, path,
						 resolved, sizeof(resolved));
	if (rc != NB_OK)
		return rc;

	if (nb_path_is_within(resolved, call->context.paths.temp_dir, true))
		return NB_ERR_INVAL;

	if (kind == TOOL_ARTIFACT_IMAGE)
		return tool_result_add_image_copy(call->result, resolved, width, height);

	if (kind == TOOL_ARTIFACT_VIDEO)
		return tool_result_add_video_copy(call->result, resolved, duration_seconds);

	return tool_result_add_artifact_copy(call->result, kind, resolved);
}

static const char *runtime_extension_string(const char *value)
{
	return value ? value : "";
}

static const char *runtime_extension_get_config_string(void *host_data, const char *path)
{
	const struct runtime_extension_call *call = host_data;
	const struct config_model_entry *entry = NULL;
	const char *field;

	if (!path || strncmp(path, "model.", 6) != 0)
		return NULL;

	path += 6;
	field = strchr(path, '.');
	if (!field)
		return NULL;

	size_t length = (size_t)(field - path);
#define MODEL_ENTRY(name) \
	if (length == sizeof(#name) - 1 && strncmp(path, #name, length) == 0) \
		entry = &call->config->models.name
	MODEL_ENTRY(text);
	MODEL_ENTRY(vision);
	MODEL_ENTRY(image);
	MODEL_ENTRY(video);
#undef MODEL_ENTRY
	if (!entry)
		return NULL;

	field++;
#define MODEL_STRING(name) \
	if (strcmp(field, #name) == 0) \
		return entry->name
	MODEL_STRING(provider);
	MODEL_STRING(adapter);
	MODEL_STRING(model);
	MODEL_STRING(api_base);
	MODEL_STRING(api_key);
	MODEL_STRING(api_key_env);
	MODEL_STRING(extra_body_json);
#undef MODEL_STRING
	return NULL;
}

static int runtime_extension_context_prepare(struct runtime_extension_call *call,
					     const struct permission_policy *policy,
					     const struct ext_host_api *host)
{
	const struct tool_runtime_context *runtime = call->runtime;
	struct ext_context *context = &call->context;
	nb_buf parent NB_CLEANUP(nb_buf_destroy) = {0};
	unsigned char digest[NB_SHA256_DIGEST_SIZE];
	char key[NB_SHA256_DIGEST_SIZE * 2 + 1];

	if (!runtime->config)
		return MORPH_ERR_NOT_CONFIGURED;

	call->config = malloc(sizeof(*call->config));
	if (!call->config)
		return NB_ERR_NOMEM;

	*call->config = *runtime->config;
	int rc = nb_path_canonical(&call->extension_dir, call->extension->path, NULL);
	if (rc == NB_OK)
		rc = nb_path_dirname(&parent, nb_buf_cstr(&call->extension_dir));

	if (rc != NB_OK)
		return rc;

	nb_sha256_ctx hash;
	nb_sha256_init(&hash);
	nb_sha256_update(&hash, call->extension_dir.ptr, call->extension_dir.len + 1);
	const char *user_id = runtime_extension_string(runtime->user_id);
	nb_sha256_update(&hash, user_id, strlen(user_id));
	nb_sha256_final(&hash, digest);
	for (size_t i = 0; i < sizeof(digest); i++) {
		const char hex[] = "0123456789abcdef";
		key[i * 2] = hex[digest[i] >> 4];
		key[i * 2 + 1] = hex[digest[i] & 15];
	}

	key[sizeof(key) - 1] = '\0';
	if (!nb_buf_printf(&call->data_dir, "%s/.data/%s", nb_buf_cstr(&parent), key))
		return NB_ERR_NOMEM;

	rc = nb_file_mkdir(nb_buf_cstr(&call->data_dir), true, NULL);
	if (rc == NB_OK)
		rc = nb_file_temp_dir(&call->temp_dir, NULL, "morph-ext-", NULL);

	if (rc != NB_OK)
		return rc;

	context->abi_version = EXT_CONTEXT_ABI_VERSION;
	context->struct_size = sizeof(*context);
	context->config = call->config;
	context->manifest = &call->extension->manifest;
	context->settings = call->extension->manifest.settings;
	context->logger = runtime->http.logger;
	context->paths = (struct ext_paths){
		.workdir = runtime->workdir ? runtime->workdir
			: tool_context_workdir_borrow(runtime->permission_context),
		.output_dir = tool_context_output_dir_borrow(runtime->permission_context),
		.extension_dir = nb_buf_cstr(&call->extension_dir),
		.data_dir = nb_buf_cstr(&call->data_dir),
		.temp_dir = nb_buf_cstr(&call->temp_dir),
	};
	context->call = (struct ext_call_context){
		.user_id = user_id,
		.session_id = runtime_extension_string(runtime->credit_session_id),
		.agent_id = runtime->agent_id ? runtime->agent_id : "/root",
		.agent_depth = runtime->agent_depth,
		.turn_id = runtime_extension_string(runtime->turn_id),
		.tool_call_id = runtime_extension_string(runtime->tool_call_id),
		.deadline_ms = runtime->deadline_ms,
	};
	context->permissions = *policy;
	context->host = host;
	context->host_data = call;
	return NB_OK;
}

static int runtime_extension_native(struct runtime_extension_call *call,
				   const struct permission_policy *policy,
				   const char *args_json, char **output)
{
	const struct ext_host_api host = {
		.abi_version = EXT_CONTEXT_ABI_VERSION,
		.struct_size = sizeof(host),
		.check_cancelled = runtime_extension_check_cancelled,
		.log = runtime_extension_log,
		.emit_stream = runtime_extension_emit_stream,
		.check_operation = runtime_extension_check_operation,
		.authorize_path = runtime_extension_authorize_path,
		.chat = runtime_extension_chat,
		.vision = runtime_extension_vision,
		.image = runtime_extension_image,
		.video = runtime_extension_video,
		.ask_user = runtime_extension_ask_user,
		.add_artifact = runtime_extension_add_artifact,
		.get_config_string = runtime_extension_get_config_string,
	};
	int rc = runtime_extension_context_prepare(call, policy, &host);

	if (rc != NB_OK)
		return rc;

	rc = runtime_extension_check_cancelled(call);
	if (rc == NB_OK)
		rc = ext_run_with_context(call->extension, &call->context, args_json, output,
			runtime_extension_cancelled, call, NULL);

	int cancelled = runtime_extension_check_cancelled(call);
	if (cancelled != NB_OK)
		return cancelled;

	return rc > 0 ? NB_ERR_PROTOCOL : rc;
}

static int runtime_extension_legacy_cancelled(void *opaque)
{
	const struct tool_result *result = opaque;
	return http_wait_cancelable(&result->runtime->http, 0) != NB_OK;
}

int runtime_extension_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct ext *extension = user_data;
	char *output NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct sandbox_config limits NB_CLEANUP(sandbox_config_cleanup) = {0};
	struct permission_policy policy = {0};

	if (!extension || !args_json || !result)
		return NB_ERR_INVAL;

	struct tool_context *permissions = result->runtime
		? result->runtime->permission_context : NULL;
	if (!permissions)
		return NB_ERR_NOT_PERMITTED;

	int rc = tool_context_get_permission_policy(permissions, &policy);
	struct tool_operation operation = {
		.kind = TOOL_OP_TOOL_CALL,
		.tool_name = extension->manifest.name,
		.principal = extension->manifest.name,
		.action = args_json,
		.details_json = args_json,
		.scope = extension->path,
		.network = !!(extension->manifest.permissions & EXT_PERM_NETWORK),
	};
	if (rc == NB_OK)
		rc = tool_context_check_operation(permissions, &operation);

	if (rc == NB_OK)
		rc = tool_context_add_builtin_path(permissions, TOOL_PATH_READ, extension->path);

	if (rc == NB_OK)
		rc = sandbox_permission_prepare(permissions, extension->manifest.name,
						 NULL, &limits);

	if (rc != NB_OK)
		return rc;

	limits.network_access = operation.network;
	int host_access = permission_effective_access(&policy).host;
	if (extension->run_v2 && host_access) {
		struct runtime_extension_call call NB_CLEANUP(runtime_extension_call_cleanup) = {
			.extension = extension,
			.runtime = result->runtime,
			.result = result,
		};
		rc = runtime_extension_native(&call, &policy, args_json, &output);
	} else {
		rc = ext_run_bounded(extension, args_json, &output,
			runtime_extension_legacy_cancelled, result, host_access ? NULL : &limits);
	}

	struct tool_artifact_list artifacts = result->artifacts;
	if (output) {
		int copy_rc = tool_result_success_json_text_copy(result, output);
		if (copy_rc != NB_OK)
			return copy_rc;
	}

	if (rc == NB_OK)
		result->artifacts = artifacts;

	return rc;
}
