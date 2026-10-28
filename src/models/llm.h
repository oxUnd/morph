#ifndef LLM_H
#define LLM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "http/client.h"
#include "agent/tool.h"
#include <stdint.h>

#ifndef MORPH_MODEL_EXTRA_BODY_MAX
#define MORPH_MODEL_EXTRA_BODY_MAX 8192
#endif

struct arena;

typedef int (*sse_callback)(const char *token, void *user_data);

enum llm_stream_kind {
	LLM_STREAM_CONTENT,
	LLM_STREAM_REASONING,
};

typedef int (*llm_stream_callback)(enum llm_stream_kind kind,
				   const char *token, void *user_data);

struct tool_call {
	char id[128];
	char tool_call_id[128];
	char name[64];
	char *arguments;
	enum tool_input_kind input_kind;
};

struct model_usage {
	char provider[32];
	char model[128];
	char kind[32];
	char phase[32];
	char response_id[128];
	char system_fingerprint[128];
	char finish_reason[64];
	char usage_source[32];
	int64_t created;
	int64_t input_tokens;
	int64_t output_tokens;
	int64_t total_tokens;
	int64_t cached_tokens;
	int64_t reasoning_tokens;
	int64_t audio_tokens;
	int64_t image_tokens;
	int64_t image_units;
	int64_t video_seconds;
};

typedef void (*model_usage_callback)(const struct model_usage *usage,
				     void *user_data);

struct model_usage_sink {
	model_usage_callback callback;
	void *user_data;
};

struct chat_message {
	char *role;
	char *content;
	char *tool_call_id;
	struct tool_call *tool_calls;
	int tool_call_count;
	char *reasoning_content;
	/* Synthetic context references may need a different wire role per adapter. */
	int is_context_summary;
};

struct chat_response {
	char *content;
	struct tool_call *tool_calls;
	int tool_call_count;
	struct model_usage usage;
	struct arena *arena;
	char *reasoning_content;
};

struct model_chat_options {
	int max_tokens;
	long timeout_seconds;
	const char *usage_phase;
};

struct model_image_chat_options {
	int max_tokens;
	long timeout_seconds;
	int max_dim;
};

enum model_error_kind {
	MODEL_ERROR_NONE,
	MODEL_ERROR_NETWORK,
	MODEL_ERROR_CONTEXT,
	MODEL_ERROR_RATE_LIMIT,
	MODEL_ERROR_AUTH,
	MODEL_ERROR_CONFIG,
	MODEL_ERROR_SERVER,
	MODEL_ERROR_INCOMPLETE,
	MODEL_ERROR_API,
};

struct model_error_info {
	enum model_error_kind kind;
	int http_status;
	char provider_code[96];
	long retry_after_seconds;
	int output_started;
	int retryable;
};

struct model {
	/* Borrowed for the model lifetime; shared only within its owning runtime. */
	struct model_usage_sink *usage_sink;
	char name[64];
	char provider[32];
	char adapter[64];
	char api_base[256];
	char api_key[256];
	char model_id[128];
	char extra_body_json[MORPH_MODEL_EXTRA_BODY_MAX];
	char last_error[512];
	struct model_error_info error_info;
	int context_limit;
	int max_tokens;
	long timeout_seconds;
	int retry_count;
	int poll_interval_seconds;
	int poll_timeout_seconds;
	void *handle;
	int (*chat)(struct model *self, struct arena *arena,
		    const char *system_prompt,
		    const char **messages, int n,
		    const struct model_chat_options *opts,
		    sse_callback cb, void *user_data);
	int (*chat_with_tools)(struct model *self, struct arena *arena,
			       const char *system_prompt,
			       struct chat_message *messages, int msg_count,
			       struct tool_desc *tools, int tool_count,
			       struct chat_response *response,
			       sse_callback thought_cb, void *thought_ud);
	int (*chat_with_tools_stream)(struct model *self, struct arena *arena,
				      const char *system_prompt,
				      struct chat_message *messages, int msg_count,
				      struct tool_desc *tools, int tool_count,
				      struct chat_response *response,
				      llm_stream_callback stream_cb,
				      void *stream_ud);
	int (*chat_with_image)(struct model *self, struct arena *arena,
			       const char *system_prompt,
			       const char *prompt,
			       const char *image_path,
			       const struct model_image_chat_options *opts,
			       sse_callback cb, void *user_data);
	int (*generate)(struct model *self, const char *prompt, const char *out_path);
	void (*destroy)(struct model *self);
};

/* Local estimate over the selected adapter's serialized input. This is not
 * provider usage: tokenizer differences and framing require a safety margin. */
typedef int (*model_token_counter)(const char *text, void *user_data);
int model_estimate_input_tokens(const struct model *model,
	const char *system_prompt, const struct chat_message *messages, int msg_count,
	const struct tool_desc *tools, int tool_count, model_token_counter count,
	void *user_data, int *tokens);
/* Adapter-normalized model input, without transport headers or generation options.
 * Caller owns the JSON. Used by local request inspection, not model tools. */
int model_input_snapshot(const struct model *model, const char *system_prompt,
	const struct chat_message *messages, int msg_count,
	const struct tool_desc *tools, int tool_count, cJSON **out);

struct model *model_llm_create(const char *provider, const char *model_id,
			       const char *api_base, const char *api_key);
void model_destroy(struct model *m);
void model_report_usage(const struct model *model, const struct model_usage *usage);


void chat_response_free(struct chat_response *resp);
void chat_message_cleanup(struct chat_message *msg, struct arena *arena);
void tool_call_cleanup(struct tool_call *tc, struct arena *arena);

#ifdef __cplusplus
}
#endif

#endif
