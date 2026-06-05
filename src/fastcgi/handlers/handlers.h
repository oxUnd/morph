/* handlers.h — handler entry points referenced by the router */
#ifndef MORPH_FCGI_HANDLERS_H
#define MORPH_FCGI_HANDLERS_H

#include "../fcgi_io.h"

/* health */
void handle_health(request_t *r);

/* users / setup */
void handle_install(request_t *r);
void handle_signup(request_t *r);
void handle_me_quota(request_t *r);

/* sessions */
void handle_create_session(request_t *r);
void handle_list_sessions(request_t *r);
void handle_get_session(request_t *r);
void handle_delete_session(request_t *r);

/* turns */
void handle_post_turn(request_t *r);

/* canvas */
void handle_get_canvas(request_t *r);
void handle_add_canvas_node(request_t *r);
void handle_patch_canvas_node(request_t *r);

/* actions (Web → Agent) */
void handle_post_action(request_t *r);

/* events (Agent → Web, SSE) */
void handle_sse(request_t *r);

/* artifacts */
void handle_list_artifacts(request_t *r);
void handle_artifact_meta(request_t *r);
void handle_get_artifact(request_t *r);

#endif
