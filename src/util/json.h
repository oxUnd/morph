#ifndef MORPH_JSON_H
#define MORPH_JSON_H

#include "cJSON.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize once; the arena owns the returned heap buffer via cleanup. */
char *morph_json_print(struct arena *arena, const cJSON *value);
/* Takes ownership of a malloc-allocated string, including on failure. */
cJSON *morph_json_take_string(char *text);

#ifdef __cplusplus
}
#endif
#endif
