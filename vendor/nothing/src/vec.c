#include "nb/vec.h"

#include "nb/mem.h"
#include "container_internal.h"

#include <string.h>

void nb_vec_init(nb_vec *vec, size_t elem_size)
{
    NB_ASSERT(vec != NULL);
    NB_ASSERT(elem_size > 0);
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
    vec->elem_size = elem_size;
}

void nb_vec_destroy(nb_vec *vec)
{
    if (!vec) {
        return;
    }
    if (vec->data) {
        nb_free(vec->data, vec->cap * vec->elem_size);
    }
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
}

void nb_vec_move(nb_vec *dst, nb_vec *src)
{
    if (!dst || !src || dst == src) {
        return;
    }
    nb_vec previous = *dst;
    *dst = *src;
    *src = previous;
}

void *nb_vec_push_n(nb_vec *vec, size_t count)
{
    if (!vec || count == 0 || count > SIZE_MAX - vec->len ||
        !nb_vec_reserve(vec, vec->len + count)) {
        return NULL;
    }
    void *slots = vec->data + vec->len * vec->elem_size;
    vec->len += count;
    return slots;
}

bool nb_vec_reserve(nb_vec *vec, size_t count)
{
    if (!vec || vec->elem_size == 0) {
        return false;
    }
    if (count <= vec->cap) {
        return true;
    }
    if (count > SIZE_MAX / vec->elem_size) {
        return false;
    }
    size_t new_cap = vec->cap ? vec->cap : 8u;
    while (new_cap < count) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = count;
            break;
        }
        new_cap *= 2u;
    }
    if (new_cap > SIZE_MAX / vec->elem_size) {
        return false;
    }
    unsigned char *data = nb_try_realloc(vec->data,
                                         vec->cap * vec->elem_size,
                                         new_cap * vec->elem_size);
    if (!data) {
        return false;
    }
    vec->data = data;
    vec->cap = new_cap;
    return true;
}

bool nb_vec_shrink(nb_vec *vec)
{
    if (!vec || vec->elem_size == 0 || vec->len > vec->cap) {
        return false;
    }
    if (vec->len == vec->cap) {
        return true;
    }
    if (vec->cap > SIZE_MAX / vec->elem_size) {
        return false;
    }
    size_t old_bytes = vec->cap * vec->elem_size;
    if (vec->len == 0) {
        if (vec->data) {
            nb_free(vec->data, old_bytes);
        }
        vec->data = NULL;
        vec->cap = 0;
        return true;
    }
    if (vec->len > SIZE_MAX / vec->elem_size) {
        return false;
    }
    size_t new_bytes = vec->len * vec->elem_size;
    unsigned char *data = nb_try_realloc(vec->data, old_bytes, new_bytes);
    if (!data) {
        return false;
    }
    vec->data = data;
    vec->cap = vec->len;
    return true;
}

void *nb_vec_push(nb_vec *vec)
{
    if (!vec || vec->len == SIZE_MAX ||
        !nb_vec_reserve(vec, vec->len + 1u)) {
        return NULL;
    }
    void *ptr = vec->data + vec->len * vec->elem_size;
    vec->len++;
    return ptr;
}

bool nb_vec_push_copy(nb_vec *vec, const void *elem)
{
    if (!vec || !elem) {
        return false;
    }
    size_t offset = 0;
    bool internal = nb_span_offset(vec->data, vec->len * vec->elem_size,
                                   elem, vec->elem_size, &offset);
    void *dst = nb_vec_push(vec);
    if (!dst) {
        return false;
    }
    if (internal) {
        elem = vec->data + offset;
    }
    memcpy(dst, elem, vec->elem_size);
    return true;
}

bool nb_vec_insert(nb_vec *vec, size_t index, const void *elem)
{
    if (!vec || !elem || vec->elem_size == 0 || index > vec->len) {
        return false;
    }
    size_t offset = 0;
    bool internal = nb_span_offset(vec->data, vec->len * vec->elem_size,
                                   elem, vec->elem_size, &offset);
    if (vec->len == SIZE_MAX || !nb_vec_reserve(vec, vec->len + 1u)) {
        return false;
    }
    unsigned char *dst = vec->data + index * vec->elem_size;
    size_t tail = vec->len - index;
    if (tail != 0) {
        memmove(dst + vec->elem_size, dst, tail * vec->elem_size);
    }
    if (internal) {
        if (offset >= index * vec->elem_size) {
            offset += vec->elem_size;
        }
        elem = vec->data + offset;
    }
    memmove(dst, elem, vec->elem_size);
    vec->len++;
    return true;
}

void nb_vec_remove_range(nb_vec *vec, size_t index, size_t count)
{
    if (!vec || index >= vec->len || count == 0) {
        return;
    }
    size_t available = vec->len - index;
    if (count > available) {
        count = available;
    }
    size_t tail = available - count;
    if (tail) {
        unsigned char *dst = vec->data + index * vec->elem_size;
        memmove(dst, dst + count * vec->elem_size, tail * vec->elem_size);
    }
    vec->len -= count;
}

void nb_vec_remove(nb_vec *vec, size_t index) { nb_vec_remove_range(vec, index, 1); }

void *nb_vec_at(const nb_vec *vec, size_t index)
{
    if (!vec || index >= vec->len) {
        return NULL;
    }
    return vec->data + index * vec->elem_size;
}

void *nb_vec_at_mut(nb_vec *vec, size_t index)
{
    return nb_vec_at(vec, index);
}

void nb_vec_pop(nb_vec *vec)
{
    if (vec && vec->len > 0) {
        vec->len--;
    }
}

void nb_vec_clear(nb_vec *vec)
{
    if (vec) {
        vec->len = 0;
    }
}

size_t nb_vec_len(const nb_vec *vec)
{
    return vec ? vec->len : 0u;
}

bool nb_vec_empty(const nb_vec *vec)
{
    return !vec || vec->len == 0;
}

size_t nb_vec_elem_size(const nb_vec *vec)
{
    return vec ? vec->elem_size : 0u;
}
