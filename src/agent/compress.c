#include "compress.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *is_system_role(struct message_list *msg)
{
	if (msg && msg->role && strcmp(msg->role, "system") == 0)
		return msg->content;
	return NULL;
}

int compress_sliding_window(struct message_list **head, int keep_rounds,
			   struct compress_result *result)
{
	if (!head || !*head || keep_rounds < 0)
		return -EINVAL;
	if (!result)
		return -EINVAL;

	int total = msg_list_count(*head);
	int to_preserve = keep_rounds * 2;

	struct message_list *sys_msg_save = NULL;
	int removed = 0;
	int original_tokens = context_token_count(*head, NULL);
	memset(result, 0, sizeof(*result));
	result->original_tokens = original_tokens;

	struct message_list *cur = *head;
	struct message_list *prev = NULL;

	while (cur && (total - removed) > to_preserve) {
		if (is_system_role(cur)) {
			prev = cur;
			cur = cur->next;
			continue;
		}
		struct message_list *next = cur->next;
		if (prev) {
			prev->next = next;
		} else {
			*head = next;
		}
		free(cur->role);
		free(cur->content);
		free(cur);
		removed++;
		cur = next;
	}

	result->messages_removed = removed;
	result->compressed_tokens = context_token_count(*head, NULL);
	return 0;
}

int compress_react_trace(struct message_list **head,
			 struct compress_result *result)
{
	if (!head || !result)
		return -EINVAL;
	struct message_list *cur = *head;
	struct message_list *prev = NULL;
	int removed = 0;
	while (cur) {
		if (cur->compressed) {
			struct message_list *next = cur->next;
			if (prev)
				prev->next = next;
			else
				*head = next;
			free(cur->role);
			free(cur->content);
			free(cur);
			removed++;
			cur = next;
		} else {
			prev = cur;
			cur = cur->next;
		}
	}
	result->messages_removed = removed;
	return 0;
}

static const char *key_patterns[] = {
	"file_path",
	"output",
	"result",
	"path",
	NULL
};

struct key_info *extract_key_info(struct message_list *head)
{
	struct key_info *info_head = NULL;
	struct key_info **tail = &info_head;
	struct message_list *cur = head;
	while (cur) {
		for (const char **p = key_patterns; *p; p++) {
			if (strstr(cur->content, *p)) {
				struct key_info *ki = calloc(1, sizeof(*ki));
				ki->key = strdup(*p);
				ki->value = strdup(cur->content);
				*tail = ki;
				tail = &ki->next;
			}
		}
		cur = cur->next;
	}
	return info_head;
}

void key_info_free(struct key_info *head)
{
	struct key_info *cur = head;
	while (cur) {
		struct key_info *next = cur->next;
		free(cur->key);
		free(cur->value);
		free(cur);
		cur = next;
	}
}