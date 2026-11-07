#ifndef MORPH_OUTPUT_STORE_H
#define MORPH_OUTPUT_STORE_H

#include <stdint.h>

struct arena;
struct db;

#ifdef __cplusplus
extern "C" {
#endif

struct output_record {
	int64_t id;
	int64_t session_id;
	const char *kind;
	const char *path;
	const char *prompt;
	const char *model;
	const char *request_prompt;
	const char *provider;
	const char *tool_name;
	const char *tool_call_id;
	const char *turn_id;
	const char *recipe_json;
	const char *mime;
	int width;
	int height;
	int duration_seconds;
	int64_t size_bytes;
	int64_t created_at;
};

int output_store_find_id(struct db *db, const char *path, int64_t *id);
/* A previously recorded path succeeds with *id == 0. Input strings are borrowed. */
int output_store_insert(struct db *db, const struct output_record *record, int64_t *id);
/* Returned strings belong to arena. Missing paths return -ENODATA. */
int output_store_get(struct db *db, const char *path, struct arena *arena,
		     struct output_record *record);

#ifdef __cplusplus
}
#endif

#endif
