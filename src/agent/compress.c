#include "compress.h"
#include "agent/tokenizer.h"
#include "util/arena.h"
#include "util/buf.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *key_patterns[] = {
	"file_path", "output", "result", "path",
	"error", "generated", "downloaded", "saved",
	"created", "wrote",
	NULL
};

static void msg_free(struct message_list *msg)
{
	(void)msg;
}

static const char *is_system_role(struct message_list *msg)
{
	if (msg && msg->role && strcmp(msg->role, "system") == 0)
		return msg->content;
	return NULL;
}

static struct key_info *key_info_add(struct key_info *head, const char *key,
				      const char *content)
{
	if (!key || !content) return head;
	struct key_info *cur = head;
	while (cur) {
		if (strcmp(cur->key, key) == 0) return head;
		cur = cur->next;
	}
	const char *match = strstr(content, key);
	if (!match) return head;
	const char *ls = match;
	while (ls > content && *(ls - 1) != '\n') ls--;
	const char *le = match;
	while (*le && *le != '\n') le++;
	struct key_info *n = calloc(1, sizeof(*n));
	if (!n) return head;
	n->key = strdup(key);
	n->value = strndup(ls, (size_t)(le - ls));
	n->next = head;
	return n;
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
		if (prev)
			prev->next = next;
		else
			*head = next;
		msg_free(cur);
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
			msg_free(cur);
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

int compress_detect_react_cycles(struct message_list *head)
{
	if (!head) return 0;
	int marked = 0;
	struct message_list *cur = head;
	while (cur && cur->next && cur->next->next) {
		if (cur->compressed || cur->next->compressed ||
		    cur->next->next->compressed) {
			cur = cur->next;
			continue;
		}
		const char *r1 = cur->role;
		const char *r2 = cur->next->role;
		const char *r3 = cur->next->next->role;
		int is_react = 0;
		if (r1 && r2 && r3 &&
		    strcmp(r1, "assistant") == 0 &&
		    strcmp(r2, "assistant") == 0 &&
		    strcmp(r3, "user") == 0)
			is_react = 1;
		if (is_react) {
			cur->compressed = 1;
			cur->next->compressed = 1;
			cur->next->next->compressed = 1;
			marked += 3;
			cur = cur->next->next->next;
		} else {
			cur = cur->next;
		}
	}
	return marked;
}

struct key_info *extract_key_info(struct message_list *head)
{
	struct key_info *info = NULL;
	struct message_list *cur = head;
	while (cur) {
		if (!cur->content) { cur = cur->next; continue; }
		for (const char **p = key_patterns; *p; p++)
			info = key_info_add(info, *p, cur->content);
		cur = cur->next;
	}
	return info;
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

int compress_summarize(struct message_list **head, int keep_rounds,
		       summarize_fn fn, void *fn_user,
		       struct arena *session,
		       struct compress_result *result)
{
	if (!head || !*head || keep_rounds < 0 || !fn)
		return -EINVAL;
	if (!result)
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	result->original_tokens = context_token_count(*head, NULL);

	int total = msg_list_count(*head);
	int to_keep = keep_rounds * 2;
	int to_summarize = total - to_keep;
	if (to_summarize <= 0)
		return 0;

	struct message_list *cur = *head;
	struct message_list *split = NULL;
	int idx = 0;
	morph_buf_t text_buf;
	int text_ok = morph_buf_init(&text_buf, 8192);
	if (text_ok != 0)
		return text_ok;

	while (cur && idx < to_summarize) {
		if (is_system_role(cur) || cur->compressed) {
			split = cur;
			idx++;
			cur = cur->next;
			continue;
		}
		if (morph_buf_printf(&text_buf, "[%s]: %s\n",
				     cur->role, cur->content) != 0) {
			morph_buf_cleanup(&text_buf);
			MORPH_RETURN(-ENOMEM);
		}
		split = cur;
		idx++;
		cur = cur->next;
	}

	if (text_buf.len == 0) {
		morph_buf_cleanup(&text_buf);
		return 0;
	}

	char *summary = NULL;
	int rc = fn(morph_buf_cstr(&text_buf), fn_user, &summary);
	morph_buf_cleanup(&text_buf);
	if (rc < 0 || !summary)
		return rc;

	struct message_list *keep_head = split ? split->next : NULL;
	if (split) split->next = NULL;

	cur = *head;
	struct message_list *rechain = NULL;
	struct message_list **rechain_tail = &rechain;
	while (cur && cur != keep_head) {
		struct message_list *next = cur->next;
		if (is_system_role(cur)) {
			*rechain_tail = cur;
			cur->next = NULL;
			rechain_tail = &cur->next;
		} else if (cur->compressed) {
			result->messages_summarized++;
			msg_free(cur);
		} else {
			result->messages_summarized++;
			if (cur->content)
				result->preserved = key_info_add(result->preserved,
					"summarized", cur->content);
			msg_free(cur);
		}
		cur = next;
	}

	int stok = tokenizer_estimate_tokens(summary);
	if (stok < 1) stok = 1;

	struct message_list *summary_msg = arena_alloc(session, sizeof(*summary_msg));
	if (!summary_msg) { free(summary); return -ENOMEM; }
	summary_msg->role = arena_strdup(session, "system");
	summary_msg->content = arena_strdup(session, summary);
	free(summary);
	summary_msg->token_count = stok;
	summary_msg->file_paths = NULL;
	summary_msg->file_count = 0;
	summary_msg->compressed = 0;

	*head = summary_msg;
	if (rechain) {
		summary_msg->next = rechain;
		struct message_list *r = rechain;
		while (r->next) r = r->next;
		r->next = keep_head;
	} else {
		summary_msg->next = keep_head;
	}

	result->compressed_tokens = context_token_count(*head, NULL);
	return 1;
}
