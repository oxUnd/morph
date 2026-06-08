#include "strmap.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define STRMAP_EMPTY   0
#define STRMAP_USED    1
#define STRMAP_DELETED 2
#define STRMAP_MAX_LOAD 70

static uint32_t strmap_hash(const char *s, size_t len)
{
	uint32_t h = 2166136261u;

	for (size_t i = 0; i < len; i++) {
		h ^= (unsigned char)s[i];
		h *= 16777619u;
	}
	return h ? h : 1u;
}

static size_t strmap_next_cap(size_t min_cap)
{
	size_t cap = MORPH_STRMAP_INIT_CAP;

	while (cap < min_cap) {
		if (cap > SIZE_MAX / 2)
			return 0;
		cap *= 2;
	}
	return cap;
}

static char *strmap_key_dup(const char *key, size_t len)
{
	char *copy = malloc(len + 1);

	if (!copy)
		return NULL;
	memcpy(copy, key, len);
	copy[len] = '\0';
	return copy;
}

static int strmap_find_slot(const morph_strmap_t *m, const char *key,
			    size_t key_len, uint32_t hash, size_t *slot,
			    int *found)
{
	size_t first_deleted = SIZE_MAX;
	size_t mask;
	size_t idx;

	if (!m || !m->entries || m->cap == 0 || !slot || !found)
		MORPH_RETURN(-EINVAL);

	mask = m->cap - 1;
	idx = hash & mask;
	for (;;) {
		const struct morph_strmap_entry *e = &m->entries[idx];

		if (e->state == STRMAP_EMPTY) {
			*slot = first_deleted != SIZE_MAX ? first_deleted : idx;
			*found = 0;
			return 0;
		}
		if (e->state == STRMAP_DELETED) {
			if (first_deleted == SIZE_MAX)
				first_deleted = idx;
		} else if (e->hash == hash && e->key_len == key_len &&
			   memcmp(e->key, key, key_len) == 0) {
			*slot = idx;
			*found = 1;
			return 0;
		}
		idx = (idx + 1) & mask;
	}
}

static int strmap_insert_owned(morph_strmap_t *m, char *key, size_t key_len,
			       uint32_t hash, void *value)
{
	size_t slot;
	int found;
	int rc;

	rc = strmap_find_slot(m, key, key_len, hash, &slot, &found);
	if (rc != 0)
		return rc;
	if (found) {
		free(m->entries[slot].key);
		m->entries[slot].key = key;
		m->entries[slot].value = value;
		return 0;
	}
	if (m->entries[slot].state == STRMAP_DELETED)
		m->deleted--;
	m->entries[slot].key = key;
	m->entries[slot].key_len = key_len;
	m->entries[slot].hash = hash;
	m->entries[slot].value = value;
	m->entries[slot].state = STRMAP_USED;
	m->count++;
	return 0;
}

static int strmap_rehash(morph_strmap_t *m, size_t min_cap)
{
	morph_strmap_t next;
	size_t new_cap;
	int rc;

	if (!m)
		MORPH_RETURN(-EINVAL);
	new_cap = strmap_next_cap(min_cap);
	if (new_cap == 0)
		MORPH_RETURN(-EOVERFLOW);
	rc = morph_strmap_init(&next, new_cap);
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < m->cap; i++) {
		struct morph_strmap_entry *e = &m->entries[i];

		if (e->state != STRMAP_USED)
			continue;
		rc = strmap_insert_owned(&next, e->key, e->key_len,
					 e->hash, e->value);
		if (rc != 0) {
			next.entries = NULL;
			morph_strmap_cleanup(&next);
			return rc;
		}
		e->key = NULL;
	}
	free(m->entries);
	*m = next;
	return 0;
}

static int strmap_maybe_grow(morph_strmap_t *m)
{
	size_t used;

	if (!m || m->cap == 0)
		MORPH_RETURN(-EINVAL);
	used = m->count + m->deleted + 1;
	if (used * 100 <= m->cap * STRMAP_MAX_LOAD)
		return 0;
	return strmap_rehash(m, m->count * 2 + 1);
}

int morph_strmap_init(morph_strmap_t *m, size_t init_cap)
{
	size_t cap;

	if (!m)
		MORPH_RETURN(-EINVAL);
	if (init_cap == 0)
		init_cap = MORPH_STRMAP_INIT_CAP;
	cap = strmap_next_cap(init_cap);
	if (cap == 0)
		MORPH_RETURN(-EOVERFLOW);
	m->entries = calloc(cap, sizeof(m->entries[0]));
	if (!m->entries)
		MORPH_RETURN(-ENOMEM);
	m->count = 0;
	m->cap = cap;
	m->deleted = 0;
	return 0;
}

void morph_strmap_cleanup(morph_strmap_t *m)
{
	if (!m)
		return;
	if (m->entries) {
		for (size_t i = 0; i < m->cap; i++) {
			if (m->entries[i].state == STRMAP_USED)
				free(m->entries[i].key);
		}
	}
	free(m->entries);
	m->entries = NULL;
	m->count = 0;
	m->cap = 0;
	m->deleted = 0;
}

void morph_strmap_clear(morph_strmap_t *m)
{
	if (!m || !m->entries)
		return;
	for (size_t i = 0; i < m->cap; i++) {
		if (m->entries[i].state == STRMAP_USED)
			free(m->entries[i].key);
		memset(&m->entries[i], 0, sizeof(m->entries[i]));
	}
	m->count = 0;
	m->deleted = 0;
}

size_t morph_strmap_len(const morph_strmap_t *m)
{
	return m ? m->count : 0;
}

int morph_strmap_set(morph_strmap_t *m, const char *key, void *value)
{
	size_t key_len;
	uint32_t hash;
	char *copy;
	int rc;

	if (!m || !m->entries || !key)
		MORPH_RETURN(-EINVAL);
	rc = strmap_maybe_grow(m);
	if (rc != 0)
		return rc;
	key_len = strlen(key);
	hash = strmap_hash(key, key_len);
	copy = strmap_key_dup(key, key_len);
	if (!copy)
		MORPH_RETURN(-ENOMEM);
	rc = strmap_insert_owned(m, copy, key_len, hash, value);
	if (rc != 0)
		free(copy);
	return rc;
}

void *morph_strmap_get(const morph_strmap_t *m, const char *key)
{
	size_t key_len;
	uint32_t hash;
	size_t slot;
	int found;

	if (!m || !m->entries || !key)
		return NULL;
	key_len = strlen(key);
	hash = strmap_hash(key, key_len);
	if (strmap_find_slot(m, key, key_len, hash, &slot, &found) != 0)
		return NULL;
	return found ? m->entries[slot].value : NULL;
}

int morph_strmap_contains(const morph_strmap_t *m, const char *key)
{
	size_t key_len;
	uint32_t hash;
	size_t slot;
	int found;

	if (!m || !m->entries || !key)
		return 0;
	key_len = strlen(key);
	hash = strmap_hash(key, key_len);
	if (strmap_find_slot(m, key, key_len, hash, &slot, &found) != 0)
		return 0;
	return found;
}

int morph_strmap_remove(morph_strmap_t *m, const char *key)
{
	size_t key_len;
	uint32_t hash;
	size_t slot;
	int found;

	if (!m || !m->entries || !key)
		return 0;
	key_len = strlen(key);
	hash = strmap_hash(key, key_len);
	if (strmap_find_slot(m, key, key_len, hash, &slot, &found) != 0)
		return 0;
	if (!found)
		return 0;
	free(m->entries[slot].key);
	memset(&m->entries[slot], 0, sizeof(m->entries[slot]));
	m->entries[slot].state = STRMAP_DELETED;
	m->count--;
	m->deleted++;
	return 1;
}
