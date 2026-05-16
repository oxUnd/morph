#ifndef LLM_H
#define LLM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "http/client.h"

struct arena;

typedef int (*sse_callback)(const char *token, void *user_data);

struct tool_desc;

struct tool_call {
	char id[128];
	char name[64];
	char *arguments;
};

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
};

struct model {
	char name[64];
	char provider[32];
	char api_base[256];
	char api_key[256];
	char model_id[128];
	int context_limit;
	int max_tokens;
	long timeout_seconds;
	void *handle;
	int (*chat)(struct model *self, struct arena *arena,
		    const char *system_prompt,
		    const char **messages, int n,
		    sse_callback cb, void *user_data);
	int (*chat_with_tools)(struct model *self, struct arena *arena,
			       const char *system_prompt,
			       struct chat_message *messages, int msg_count,
			       struct tool_desc *tools, int tool_count,
			       struct chat_response *response,
			       sse_callback thought_cb, void *thought_ud);
	int (*generate)(struct model *self, const char *prompt, const char *out_path);
	void (*destroy)(struct model *self);
};

struct model *model_llm_create(const char *provider, const char *model_id,
			       const char *api_base, const char *api_key);
void model_destroy(struct model *m);

void chat_response_free(struct chat_response *resp);
void chat_message_cleanup(struct chat_message *msg);
void tool_call_cleanup(struct tool_call *tc);

#ifdef __cplusplus
}
#endif

#endif
