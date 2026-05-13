#ifndef CONTEXT_H
#define CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include <stdint.h>

struct message_list {
	char *role;
	char *content;
	char **file_paths;
	int file_count;
	int token_count;
	int compressed;
	struct message_list *next;
};

struct key_info {
	char *key;
	char *value;
	struct key_info *next;
};

typedef int (*summarize_fn)(const char *text, void *user_data, char **out);

struct compress_config {
	int max_context_tokens;
	int max_history_rounds;
	double summarize_threshold_ratio;
	double compress_target_ratio;
	summarize_fn summarize;
	void *summarize_user_data;
};

struct compress_result {
	int original_tokens;
	int compressed_tokens;
	int messages_removed;
	int messages_summarized;
	char *summary;
	struct key_info *preserved;
};

struct tokenizer {
	char model_name[64];
	int context_limit;
	int (*count)(const char *text);
};

struct message_list *msg_list_create(const char *role, const char *content,
				      int token_count);
void msg_list_append(struct message_list **head, struct message_list *msg);
void msg_list_destroy(struct message_list *head);
int msg_list_count(struct message_list *head);
int context_token_count(struct message_list *head, struct tokenizer *tok);
int context_needs_compress(struct message_list *head, struct tokenizer *tok,
			   struct compress_config *cfg);

#ifdef __cplusplus
}
#endif

#endif