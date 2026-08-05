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

struct chat_message {
	char *role;
	char *content;
	char *tool_call_id;
	struct tool_call *tool_calls;
	int tool_call_count;
};

struct chat_response {
	char *content;
	struct tool_call *tool_calls;
	int tool_call_count;
	struct model_usage usage;
	struct arena *arena;
};

struct model_chat_options {
	int max_tokens;
	long timeout_seconds;
};

struct model_image_chat_options {
	int max_tokens;
	long timeout_seconds;
	int max_dim;
};

struct model {
	char name[64];
	char provider[32];
	char adapter[64];
	char api_base[256];
	char api_key[256];
	char model_id[128];
	char extra_body_json[MORPH_MODEL_EXTRA_BODY_MAX];
	char last_error[512];
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

struct model *model_llm_create(const char *provider, const char *model_id,
			       const char *api_base, const char *api_key);
void model_destroy(struct model *m);
void model_set_usage_callback(model_usage_callback cb);
void model_set_usage_user_data(void *user_data);
void *model_get_usage_user_data(void);
void model_report_usage(const struct model_usage *usage);

void chat_response_free(struct chat_response *resp);
void chat_message_cleanup(struct chat_message *msg, struct arena *arena);
void tool_call_cleanup(struct tool_call *tc, struct arena *arena);

#ifdef __cplusplus
}
#endif

#endif
