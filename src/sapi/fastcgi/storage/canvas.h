#ifndef MORPH_FCGI_STORAGE_CANVAS_H
#define MORPH_FCGI_STORAGE_CANVAS_H

#include <stdint.h>

struct session_store;

/* canvas */
int canvas_list_json(struct session_store *s, const char *session_id, char **out_json);
int canvas_add_node(struct session_store *s, const char *session_id, const char *node_json,
		    char out_node_id[40]);
int canvas_patch_node(struct session_store *s, const char *session_id, const char *node_id,
		      const char *patch_json);

#endif
