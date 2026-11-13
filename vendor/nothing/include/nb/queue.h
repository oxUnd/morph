#ifndef NB_QUEUE_H
#define NB_QUEUE_H

#include "nb/base.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Intrusive doubly-linked list. The head is a sentinel that is not part of
 * the payload; nodes keep stable addresses while linked. */
typedef struct nb_queue {
    struct nb_queue *prev;
    struct nb_queue *next;
} nb_queue;

#define NB_QUEUE_INIT(q)                                                                           \
    do {                                                                                           \
        (q)->prev = (q);                                                                           \
        (q)->next = (q);                                                                           \
    } while (0)

#define NB_QUEUE_DATA(ptr, type, field) ((type *)((char *)(ptr) - offsetof(type, field)))

#define NB_QUEUE_FOREACH(q, h) for ((q) = (h)->next; (q) != (h); (q) = (q)->next)

#define NB_QUEUE_FOREACH_SAFE(q, tmp, h)                                                           \
    for ((q) = (h)->next, (tmp) = (q)->next; (q) != (h); (q) = (tmp), (tmp) = (q)->next)

static inline void nb_queue_init(nb_queue *head) { NB_QUEUE_INIT(head); }

static inline bool nb_queue_empty(const nb_queue *head) { return head->next == head; }

static inline void nb_queue_insert_head(nb_queue *head, nb_queue *node)
{
    node->next = head->next;
    node->next->prev = node;
    node->prev = head;
    head->next = node;
}

static inline void nb_queue_insert_tail(nb_queue *head, nb_queue *node)
{
    node->prev = head->prev;
    node->prev->next = node;
    node->next = head;
    head->prev = node;
}

static inline void nb_queue_remove(nb_queue *node)
{
    node->next->prev = node->prev;
    node->prev->next = node->next;
    node->prev = NULL;
    node->next = NULL;
}

static inline nb_queue *nb_queue_head(const nb_queue *head) { return head->next; }

static inline nb_queue *nb_queue_last(const nb_queue *head) { return head->prev; }

static inline nb_queue *nb_queue_next(const nb_queue *node) { return node->next; }

static inline nb_queue *nb_queue_prev(const nb_queue *node) { return node->prev; }

/* Moves every node from the list headed by node to the tail of head and
 * reinitializes node as an empty sentinel. */
static inline void nb_queue_add(nb_queue *head, nb_queue *list)
{
    if (!head || !list || nb_queue_empty(list)) {
        return;
    }
    head->prev->next = list->next;
    list->next->prev = head->prev;
    list->prev->next = head;
    head->prev = list->prev;
    nb_queue_init(list);
}

/* Splits head before q into n. q == head leaves n empty. */
static inline void nb_queue_split(nb_queue *head, nb_queue *q, nb_queue *n)
{
    if (!head || !q || !n) {
        return;
    }
    if (q == head) {
        nb_queue_init(n);
        return;
    }
    n->prev = head->prev;
    n->prev->next = n;
    n->next = q;
    head->prev = q->prev;
    head->prev->next = head;
    q->prev = n;
}

static inline nb_queue *nb_queue_middle(nb_queue *head)
{
    if (!head || nb_queue_empty(head)) {
        return NULL;
    }
    nb_queue *slow = head->next;
    nb_queue *fast = head->next->next;
    while (fast != head && fast->next != head) {
        slow = slow->next;
        fast = fast->next->next;
    }
    return slow;
}

/* Stable insertion sort. cmp compares two nodes in ascending order. */
static inline void nb_queue_sort(nb_queue *head, int (*cmp)(const nb_queue *, const nb_queue *))
{
    if (!head || !cmp || nb_queue_empty(head) || head->next->next == head) {
        return;
    }

    nb_queue *current = head->next->next;
    while (current != head) {
        nb_queue *next = current->next;
        nb_queue *previous = current->prev;
        while (previous != head && cmp(previous, current) > 0) {
            previous = previous->prev;
        }
        if (previous != current->prev) {
            nb_queue_remove(current);
            current->next = previous->next;
            current->next->prev = current;
            current->prev = previous;
            previous->next = current;
        }
        current = next;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* NB_QUEUE_H */
