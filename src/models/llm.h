#ifndef LLM_H
#define LLM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "http/client.h"

typedef int (*sse_callback)(const char *token, void *user_data);

struct model {
	char name[64];
	char provider[32];
	char api_base[256];
	char api_key[256];
	char model_id[128];
	int context_limit;
	void *handle;
	int (*chat)(struct model *self, const char *system_prompt,
		    const char **messages, int n,
		    sse_callback cb, void *user_data);
	int (*generate)(struct model *self, const char *prompt, const char *out_path);
	void (*destroy)(struct model *self);
};

struct model *model_llm_create(const char *provider, const char *model_id,
			       const char *api_base, const char *api_key);
void model_destroy(struct model *m);

#ifdef __cplusplus
}
#endif

#endif