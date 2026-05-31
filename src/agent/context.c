#include "context.h"
#include "util/arena.h"
#include <stdlib.h>
#include <string.h>

struct message_list *msg_list_create(struct arena *session, const char *role, const char *content,
				      int token_count)
{
	struct message_list *m = arena_alloc(session, sizeof(*m));
	if (!m)
		return NULL;
	m->role = arena_strdup(session, role ? role : "");
	m->content = arena_strdup(session, content ? content : "");
	m->token_count = token_count;
	m->file_paths = NULL;
	m->file_count = 0;
	m->compressed = 0;
	m->next = NULL;
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
	(void)head;
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
