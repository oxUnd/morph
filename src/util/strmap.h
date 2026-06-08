#ifndef MORPH_STRMAP_H
#define MORPH_STRMAP_H

#include "error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MORPH_STRMAP_INIT_CAP 16

struct morph_strmap_entry {
	char *key;
	size_t key_len;
	uint32_t hash;
	void *value;
	unsigned char state;
};

typedef struct {
	struct morph_strmap_entry *entries;
	size_t count;
	size_t cap;
	size_t deleted;
} morph_strmap_t;

int morph_strmap_init(morph_strmap_t *m, size_t init_cap);
void morph_strmap_cleanup(morph_strmap_t *m);
void morph_strmap_clear(morph_strmap_t *m);

size_t morph_strmap_len(const morph_strmap_t *m);
int morph_strmap_set(morph_strmap_t *m, const char *key, void *value);
void *morph_strmap_get(const morph_strmap_t *m, const char *key);
int morph_strmap_contains(const morph_strmap_t *m, const char *key);
int morph_strmap_remove(morph_strmap_t *m, const char *key);

#ifdef __cplusplus
}
#endif

#endif
