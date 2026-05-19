/* router.c — minimal pattern router with `:id` capture
 *
 * Intentionally tiny — no regex, no dependencies.  Patterns only support
 * static segments and `:name` placeholders, which is sufficient for the
 * Morph API surface.
 */
#include "router.h"
#include "auth.h"
#include "handlers/handlers.h"

#include <string.h>

typedef void (*handler_fn)(request_t *);

typedef struct {
    const char *method;
    const char *pattern;   /* e.g. /api/sessions/:id/turns */
    handler_fn  fn;
    int         needs_auth;
} route_t;

/* ------------ routing table ------------ */
static const route_t ROUTES[] = {
    /* health */
    { "GET",  "/api/health",                       handle_health,         0 },

    /* sessions */
    { "POST", "/api/sessions",                     handle_create_session, 1 },
    { "GET",  "/api/sessions",                     handle_list_sessions,  1 },
    { "GET",  "/api/sessions/:id",                 handle_get_session,    1 },
    { "DELETE","/api/sessions/:id",                handle_delete_session, 1 },

    /* turns (one ReAct round) */
    { "POST", "/api/sessions/:id/turns",           handle_post_turn,      1 },

    /* canvas */
    { "GET",  "/api/sessions/:id/canvas",          handle_get_canvas,     1 },
    { "POST", "/api/sessions/:id/canvas/nodes",    handle_add_canvas_node,1 },
    { "PATCH","/api/sessions/:id/canvas/nodes/:node", handle_patch_canvas_node, 1 },

    /* actions (Web → Agent) */
    { "POST", "/api/sessions/:id/actions",         handle_post_action,    1 },

    /* events (Agent → Web, SSE) */
    { "GET",  "/api/sessions/:id/events",          handle_sse,            1 },
};

#define N_ROUTES (sizeof(ROUTES) / sizeof(ROUTES[0]))

/* ------------ pattern matcher ------------ */
/* Returns 1 on match (and fills r->params); 0 otherwise. */
static int match_pattern(const char *pattern, const char *path, request_t *r) {
    r->n_params = 0;
    const char *p = pattern;
    const char *u = path;

    while (*p && *u) {
        if (*p == '/') {
            if (*u != '/') return 0;
            p++; u++;
            continue;
        }
        /* segment boundaries */
        const char *p_seg_end = strchr(p, '/'); if (!p_seg_end) p_seg_end = p + strlen(p);
        const char *u_seg_end = strchr(u, '/'); if (!u_seg_end) u_seg_end = u + strlen(u);

        if (*p == ':') {
            /* capture */
            if (r->n_params >= FCGI_MAX_PATH_PARAMS) return 0;
            size_t klen = (size_t)(p_seg_end - p - 1);
            size_t vlen = (size_t)(u_seg_end - u);
            if (klen >= sizeof(r->params[0].key)) return 0;
            if (vlen >= sizeof(r->params[0].val)) return 0;
            memcpy(r->params[r->n_params].key, p + 1, klen);
            r->params[r->n_params].key[klen] = '\0';
            memcpy(r->params[r->n_params].val, u, vlen);
            r->params[r->n_params].val[vlen] = '\0';
            r->n_params++;
        } else {
            /* literal */
            size_t plen = (size_t)(p_seg_end - p);
            size_t ulen = (size_t)(u_seg_end - u);
            if (plen != ulen || strncmp(p, u, plen) != 0) return 0;
        }
        p = p_seg_end;
        u = u_seg_end;
    }
    /* both must be exhausted (allow optional trailing /) */
    while (*u == '/') u++;
    while (*p == '/') p++;
    return *p == '\0' && *u == '\0';
}

/* ------------ entry ------------ */
void router_dispatch(request_t *r) {
    if (!r->method || !r->path) { reply_400(r, "missing method/path"); return; }

    for (size_t i = 0; i < N_ROUTES; i++) {
        const route_t *rt = &ROUTES[i];
        if (strcmp(rt->method, r->method) != 0) continue;
        if (!match_pattern(rt->pattern, r->path, r)) continue;

        if (rt->needs_auth && !auth_check(r)) { reply_401(r); return; }
        rt->fn(r);
        return;
    }
    reply_404(r);
}
