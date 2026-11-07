#include "context.h"
#include "agent/tokenizer.h"
#include "util/bpe.h"
#include "util/cleanup.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>

struct message_list *msg_list_create(const char *role, const char *content, int token_count)
{
	struct message_list *m MORPH_CLEANUP(msg_list_clear) = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->role = strdup(role ? role : "");
	m->content = strdup(content ? content : "");
	if (!m->role || !m->content)
		return NULL;
	m->token_count = token_count;
	m->file_paths = NULL;
	m->file_count = 0;
	m->compressed = 0;
	m->next = NULL;
	return msg_list_steal(&m);
}

int msg_list_append_take(struct message_list **head, struct message_list **msg)
{
	struct message_list **tail;

	if (!head || !msg || !*msg || head == msg)
		return -EINVAL;
	tail = head;
	while (*tail) {
		if (*tail == *msg)
			return -EINVAL;
		tail = &(*tail)->next;
	}
	*tail = *msg;
	*msg = NULL;
	return 0;
}

void msg_list_destroy(struct message_list *head)
{
	while (head) {
		struct message_list *next = head->next;

		free(head->role);
		free(head->content);
		for (int i = 0; i < head->file_count; i++)
			free(head->file_paths[i]);
		free(head->file_paths);
		free(head);
		head = next;
	}
}

void msg_list_clear(struct message_list **head)
{
	if (!head)
		return;
	msg_list_destroy(*head);
	*head = NULL;
}

struct message_list *msg_list_steal(struct message_list **source)
{
	struct message_list *value;

	if (!source)
		return NULL;
	value = *source;
	*source = NULL;
	return value;
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
	int64_t total = 0;
	struct message_list *cur = head;
	while (cur) {
		if (cur->token_count > 0)
			total += cur->token_count;
		else if (tok && tok->encoder)
			total += bpe_count_tokens(tok->encoder, cur->content);
		else if (tok && tok->count)
			total += tok->count(cur->content);
		else
			total += tokenizer_estimate_tokens(cur->content);
		cur = cur->next;
	}
	return total > INT_MAX ? INT_MAX : (int)total;
}

int context_needs_compress(struct message_list *head, struct tokenizer *tok,
			   struct compress_config *cfg)
{
	if (!cfg || cfg->max_context_tokens <= 0)
		return 0;
	int total = context_token_count(head, tok);
	int threshold = (int)(cfg->max_context_tokens * cfg->summarize_threshold_ratio);
	return total >= threshold;
}
