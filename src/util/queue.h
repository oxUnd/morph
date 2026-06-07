#ifndef MORPH_QUEUE_H
#define MORPH_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

struct morph_queue {
	struct morph_queue *prev;
	struct morph_queue *next;
};

#define morph_queue_init(q)             \
	do {                                \
		(q)->prev = (q);                \
		(q)->next = (q);                \
	} while (0)

#define morph_queue_empty(h)            ((h)->next == (h))
#define morph_queue_sentinel(h)         (h)

#define morph_queue_insert_head(h, n)   \
	do {                                \
		(n)->next = (h)->next;          \
		(n)->next->prev = (n);          \
		(n)->prev = (h);                \
		(h)->next = (n);                \
	} while (0)

#define morph_queue_insert_tail(h, n)   \
	do {                                \
		(n)->prev = (h)->prev;          \
		(n)->prev->next = (n);          \
		(n)->next = (h);                \
		(h)->prev = (n);                \
	} while (0)

#define morph_queue_remove(n)           \
	do {                                \
		(n)->next->prev = (n)->prev;    \
		(n)->prev->next = (n)->next;    \
		(n)->prev = NULL;               \
		(n)->next = NULL;               \
	} while (0)

#define morph_queue_head(h)             ((h)->next)
#define morph_queue_last(h)             ((h)->prev)
#define morph_queue_next(q)             ((q)->next)
#define morph_queue_prev(q)             ((q)->prev)

#define morph_queue_data(ptr, type, field)  \
	((type *)((char *)(ptr) - offsetof(type, field)))

#define morph_queue_foreach(q, h)       \
	for ((q) = (h)->next; (q) != (h); (q) = (q)->next)

#define morph_queue_foreach_safe(q, tmp, h)     \
	for ((q) = (h)->next, (tmp) = (q)->next;   \
	     (q) != (h);                            \
	     (q) = (tmp), (tmp) = (q)->next)

static inline void morph_queue_add(struct morph_queue *h, struct morph_queue *n)
{
	if (!h || !n || morph_queue_empty(n))
		return;
	h->prev->next = n->next;
	n->next->prev = h->prev;
	n->prev->next = h;
	h->prev = n->prev;
	morph_queue_init(n);
}

static inline void morph_queue_split(struct morph_queue *h, struct morph_queue *q,
                                     struct morph_queue *n)
{
	if (!h || !q || !n)
		return;
	if (q == h) {
		morph_queue_init(n);
		return;
	}
	n->prev = h->prev;
	n->prev->next = n;
	n->next = q;
	h->prev = q->prev;
	h->prev->next = h;
	q->prev = n;
}

static inline struct morph_queue *morph_queue_middle(struct morph_queue *q)
{
	struct morph_queue *slow;
	struct morph_queue *fast;

	if (!q)
		return NULL;
	if (morph_queue_empty(q))
		return NULL;
	slow = q->next;
	fast = q->next->next;
	while (fast != q && fast->next != q) {
		slow = slow->next;
		fast = fast->next->next;
	}
	return slow;
}

static inline void morph_queue_sort(struct morph_queue *q,
                                    int (*cmp)(const struct morph_queue *,
                                               const struct morph_queue *))
{
	struct morph_queue *cur;

	if (!q || !cmp || morph_queue_empty(q) || q->next->next == q)
		return;

	cur = q->next->next;
	while (cur != q) {
		struct morph_queue *next = cur->next;
		struct morph_queue *prev = cur->prev;

		while (prev != q && cmp(prev, cur) > 0)
			prev = prev->prev;
		if (prev != cur->prev) {
			morph_queue_remove(cur);
			cur->next = prev->next;
			cur->next->prev = cur;
			cur->prev = prev;
			prev->next = cur;
		}
		cur = next;
	}
}

#ifdef __cplusplus
}
#endif

#endif
