#ifndef MORPH_FCGI_STORAGE_ARTIFACTS_H
#define MORPH_FCGI_STORAGE_ARTIFACTS_H

#include <stdint.h>

struct session_store;

struct artifact_record {
	char id[64];
	char user_id[64];
	char session_id[64];
	char kind[16];
	char mime[64];
	char filename[128];
	char relative_path[512];
	int64_t size_bytes;
	char status[16];
	int64_t created_at;
};

/* artifacts */
int store_artifact_register(struct session_store *s, const char *user_id, const char *session_id,
			    const char *kind, const char *mime, const char *filename,
			    const char *relative_path, int64_t size_bytes,
			    char out_artifact_id[64]);
int store_artifact_get(struct session_store *s, const char *artifact_id, const char *user_id,
		       struct artifact_record *out);
int store_artifact_list_json(struct session_store *s, const char *user_id, const char *session_id,
			     char **out_json);

#endif
