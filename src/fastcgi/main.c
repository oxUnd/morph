/* main.c — morph-fastcgi process entry
 *
 * Spawns N worker threads that each call FCGX_Accept_r in a loop, builds
 * a request_t, and dispatches via the router.  Listens on a Unix socket
 * (or TCP if requested) configured via env vars or argv:
 *
 *   MORPH_FCGI_LISTEN     unix:/run/morph-fastcgi.sock | :9000 | 127.0.0.1:9000
 *   MORPH_FCGI_WORKERS    integer (default 8)
 *   MORPH_FCGI_DB         path to morph SQLite DB
 *   MORPH_FCGI_SECRET     bearer token (optional)
 *   MORPH_FCGI_TRUST_HDR  e.g. "X-Remote-User" (optional)
 */
#define _GNU_SOURCE
#include <fcgiapp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fcgi_io.h"
#include "session_store.h"
#include "router.h"
#include "auth.h"
#include "util/error.h"

static int g_listen_fd = -1;
static struct session_store *g_store = NULL;
static volatile sig_atomic_t g_shutdown = 0;

static const char *getenv_or(const char *k, const char *fallback) {
	const char *v = getenv(k);
	return (v && *v) ? v : fallback;
}

static int open_listen(const char *spec) {
	/* libfcgi accepts ":port" / "host:port" / "/path/to/sock" */
	int backlog = 128;
	int fd = FCGX_OpenSocket(spec, backlog);
	if (fd < 0) {
		fprintf(stderr, "FCGX_OpenSocket(%s) failed\n", spec);
		MORPH_RETURN(-EIO);
	}
	if (spec[0] == '/' || strncmp(spec, "unix:", 5) == 0) {
		const char *path = spec[0] == '/' ? spec : spec + 5;
		chmod(path, 0660);
	}
	return fd;
}

static void on_signal(int sig) {
	(void)sig;
	g_shutdown = 1;
	FCGX_ShutdownPending();
}

static void install_signals(void) {
	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* ---- per-thread accept loop ---- */
static void populate_request(request_t *r, FCGX_Request *req) {
	memset(r, 0, sizeof(*r));
	r->fcgx       = req;
	r->method     = FCGX_GetParam("REQUEST_METHOD",   req->envp);
	r->path       = FCGX_GetParam("DOCUMENT_URI",     req->envp);
	if (!r->path) r->path = FCGX_GetParam("REQUEST_URI", req->envp);
	r->query      = FCGX_GetParam("QUERY_STRING",     req->envp);
	r->auth_hdr   = FCGX_GetParam("HTTP_AUTHORIZATION", req->envp);
	r->trust_user = FCGX_GetParam("HTTP_X_REMOTE_USER", req->envp);
	r->last_event_id = FCGX_GetParam("HTTP_LAST_EVENT_ID", req->envp);
	r->content_type  = FCGX_GetParam("CONTENT_TYPE",  req->envp);
	const char *clen = FCGX_GetParam("CONTENT_LENGTH", req->envp);
	r->content_length = clen ? atoi(clen) : 0;
	r->store = g_store;
	snprintf(r->user_id, sizeof(r->user_id), "anonymous");
	/* strip query from path */
	if (r->path) {
		char *q = strchr((char *)r->path, '?');
		if (q) *q = '\0';
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	FCGX_Request req;
	if (FCGX_InitRequest(&req, g_listen_fd, 0) != 0) return NULL;

	while (!g_shutdown) {
		int rc = FCGX_Accept_r(&req);
		if (rc < 0) break;

		request_t r;
		populate_request(&r, &req);
		router_dispatch(&r);

		FCGX_Finish_r(&req);
	}
	return NULL;
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;

	const char *listen_spec = getenv_or("MORPH_FCGI_LISTEN",
					    "unix:/run/morph-fastcgi.sock");
	const char *db_path     = getenv_or("MORPH_FCGI_DB",
					    "/var/lib/morph/morph.db");
	int n_workers = atoi(getenv_or("MORPH_FCGI_WORKERS", "8"));
	if (n_workers < 1) n_workers = 1;
	if (n_workers > 64) n_workers = 64;

	const char *secret    = getenv("MORPH_FCGI_SECRET");
	const char *trust_hdr = getenv("MORPH_FCGI_TRUST_HDR");

	if (FCGX_Init() != 0) { fprintf(stderr, "FCGX_Init failed\n"); return 1; }

	g_listen_fd = open_listen(listen_spec);
	if (g_listen_fd < 0) return 2;

	g_store = session_store_open(db_path);
	if (!g_store) {
		fprintf(stderr, "session_store_open(%s) failed\n", db_path);
		return 3;
	}
	auth_init(secret, trust_hdr);
	install_signals();

	fprintf(stderr, "morph-fastcgi: listening on %s, %d workers, db=%s\n",
		listen_spec, n_workers, db_path);

	pthread_t *threads = calloc((size_t)n_workers, sizeof(pthread_t));
	for (int i = 0; i < n_workers; i++) {
		if (pthread_create(&threads[i], NULL, worker_main, NULL) != 0) {
			fprintf(stderr, "pthread_create #%d failed\n", i);
			n_workers = i;
			break;
		}
	}
	for (int i = 0; i < n_workers; i++) pthread_join(threads[i], NULL);
	free(threads);

	session_store_close(g_store);
	fprintf(stderr, "morph-fastcgi: shutdown clean\n");
	return 0;
}
