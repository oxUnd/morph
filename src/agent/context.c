#include "context.h"
#include <stdlib.h>
#include <string.h>

struct message_list *msg_list_create(const char *role, const char *content,
				      int token_count)
{
	struct message_list *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->role = strdup(role ? role : "");
	m->content = strdup(content ? content : "");
	m->token_count = token_count;
	return m;
}

void msg_list_append(struct message_list **head, struct message_list *msg)
{
	if (!head || !msg)
		return;
	if (!*head) {
		*head = msg;
		return;
	}
	struct message_list *cur = *head;
	while (cur->next)
		cur = cur->next;
	cur->next = msg;
}

void msg_list_destroy(struct message_list *head)
{
	struct message_list *cur = head;
	while (cur) {
		struct message_list *next = cur->next;
		free(cur->role);
		free(cur->content);
		for (int i = 0; i < cur->file_count; i++)
			free(cur->file_paths[i]);
		free(cur->file_paths);
		free(cur);
		cur = next;
	}
}

int msg_list_count(struct message_list *head)
{
	int count = 0;
	struct message_list *cur = head;
	while (cur) {
		count++;
		cur = cur->next;
	}
	return count;
}

int context_token_count(struct message_list *head, struct tokenizer *tok)
{
	int total = 0;
	struct message_list *cur = head;
	while (cur) {
		if (tok && tok->count)
			total += tok->count(cur->content);
		else
			total += cur->token_count;
		cur = cur->next;
	}
	return total;
}

int context_needs_compress(struct message_list *head, struct tokenizer *tok,
			   struct compress_config *cfg)
{
	if (!cfg)
		return 0;
	int total = context_token_count(head, tok);
	int threshold = (int)(cfg->max_context_tokens * cfg->summarize_threshold_ratio);
	return total >= threshold;
}