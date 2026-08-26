/* main.c — morph-fastcgi process entry
 *
 * Spawns N worker threads that each call FCGX_Accept_r in a loop, builds
 * a request_t, and dispatches via the router.  Listens on a Unix socket
 * (or TCP if requested) configured via env vars or argv:
 *
 *   MORPH_FCGI_LISTEN     unix:/run/morph-fastcgi.sock | :9000 | 127.0.0.1:9000
 *   MORPH_FCGI_WORKERS    integer (default 128)
 *   MORPH_FCGI_DB         path to morph SQLite DB
 *   MORPH_FCGI_TRUST_HDR  trusted proxy identity header, e.g. "X-Remote-User"
 */
#define _GNU_SOURCE
#include <fcgiapp.h>
#include <ctype.h>
#include <errno.h>
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
#include "agent_bridge.h"
#include "util/error.h"

#define FCGI_WORKERS_DEFAULT "128"
#define FCGI_WORKERS_MAX 512
#define FCGI_WORKER_STACK_SIZE (512U * 1024U)

static int g_listen_fd = -1;
static struct session_store *g_store = NULL;
static volatile sig_atomic_t g_shutdown = 0;
static char g_trust_param[96] = {0};

static const char *getenv_or(const char *k, const char *fallback)
{
	const char *v = getenv(k);
	return (v && *v) ? v : fallback;
}

static int open_listen(const char *spec)
{
	/* libfcgi accepts ":port" / "host:port" / "/path/to/sock" */
	const char *socket_spec = strncmp(spec, "unix:", 5) == 0
		? spec + 5 : spec;
	int backlog = 128;
	int fd = FCGX_OpenSocket(socket_spec, backlog);
	if (fd < 0) {
		fprintf(stderr, "FCGX_OpenSocket(%s) failed\n", spec);
		MORPH_RETURN(-EIO);
	}
	if (socket_spec[0] == '/')
		chmod(socket_spec, 0660);
	return fd;
}

static int configure_trust_param(const char *header)
{
	size_t off;

	g_trust_param[0] = '\0';
	if (!header || !*header)
		return 0;
	snprintf(g_trust_param, sizeof(g_trust_param), "HTTP_");
	off = strlen(g_trust_param);
	for (const unsigned char *p = (const unsigned char *)header; *p; p++) {
		if (off + 1 >= sizeof(g_trust_param))
			MORPH_RETURN(-ENAMETOOLONG);
		if (isalnum(*p))
			g_trust_param[off++] = (char)toupper(*p);
		else if (*p == '-')
			g_trust_param[off++] = '_';
		else
			MORPH_RETURN(-EINVAL);
	}
	g_trust_param[off] = '\0';
	return 0;
}

static void on_signal(int sig)
{
	int listen_fd;

	(void)sig;
	g_shutdown = 1;
	FCGX_ShutdownPending();
	listen_fd = g_listen_fd;
	g_listen_fd = -1;
	if (listen_fd >= 0)
		close(listen_fd);
}

static void install_signals(void)
{
	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* ---- per-thread accept loop ---- */
static void populate_request(request_t *r, FCGX_Request *req)
{
	memset(r, 0, sizeof(*r));
	r->fcgx       = req;
	r->method     = FCGX_GetParam("REQUEST_METHOD",   req->envp);
	r->path       = FCGX_GetParam("DOCUMENT_URI",     req->envp);
	if (!r->path) r->path = FCGX_GetParam("REQUEST_URI", req->envp);
	r->query      = FCGX_GetParam("QUERY_STRING",     req->envp);
	r->auth_hdr   = FCGX_GetParam("HTTP_AUTHORIZATION", req->envp);
	r->trust_user = g_trust_param[0]
		? FCGX_GetParam(g_trust_param, req->envp) : NULL;
	r->last_event_id = FCGX_GetParam("HTTP_LAST_EVENT_ID", req->envp);
	r->content_type  = FCGX_GetParam("CONTENT_TYPE",  req->envp);
	const char *clen = FCGX_GetParam("CONTENT_LENGTH", req->envp);
	r->content_length = clen ? atoi(clen) : 0;
	r->store = g_store;
	snprintf(r->user_id, sizeof(r->user_id), "anonymous");
	snprintf(r->username, sizeof(r->username), "anonymous");
	snprintf(r->role, sizeof(r->role), "anonymous");
	/* strip query from path */
	if (r->path) {
		char *q = strchr((char *)r->path, '?');
		if (q) *q = '\0';
	}
}

static void *worker_main(void *arg)
{
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

int main(int argc, char **argv)
{
	(void)argc; (void)argv;

	const char *listen_spec = getenv_or("MORPH_FCGI_LISTEN",
					    "unix:/run/morph-fastcgi.sock");
	const char *db_path     = getenv_or("MORPH_FCGI_DB",
					    "/var/lib/morph/morph.db");
	int n_workers = atoi(getenv_or("MORPH_FCGI_WORKERS",
				      FCGI_WORKERS_DEFAULT));
	if (n_workers < 1) n_workers = 1;
	if (n_workers > FCGI_WORKERS_MAX) n_workers = FCGI_WORKERS_MAX;

	const char *trust_hdr = getenv("MORPH_FCGI_TRUST_HDR");

	if (configure_trust_param(trust_hdr) < 0) {
		fprintf(stderr, "morph-fastcgi: invalid MORPH_FCGI_TRUST_HDR\n");
		return 4;
	}

	if (FCGX_Init() != 0) { fprintf(stderr, "FCGX_Init failed\n"); return 1; }

	g_listen_fd = open_listen(listen_spec);
	if (g_listen_fd < 0) return 2;

	g_store = session_store_open(db_path);
	if (!g_store) {
		fprintf(stderr, "session_store_open(%s) failed\n", db_path);
		return 3;
	}
	if (fcgi_bridge_init(db_path) != 0) {
		session_store_close(g_store);
		g_store = NULL;
		return 5;
	}
	auth_init(trust_hdr);
	install_signals();

	fprintf(stderr, "morph-fastcgi: listening on %s, %d workers, db=%s\n",
		listen_spec, n_workers, db_path);
	{
		struct fcgi_bridge_pool_status pool;

		fcgi_bridge_pool_status(&pool);
		fprintf(stderr, "morph-fastcgi: elastic runtime pool %d->%d, "
			"queue=%d, idle=%ds\n", pool.min_workers,
			pool.max_workers, pool.queue_max, pool.idle_seconds);
	}

	pthread_t *threads = calloc((size_t)n_workers, sizeof(pthread_t));
	pthread_attr_t worker_attr;
	pthread_attr_t *worker_attr_ptr = NULL;
	int worker_attr_ready = 0;
	if (!threads) {
		fprintf(stderr, "morph-fastcgi: worker allocation failed\n");
		fcgi_bridge_shutdown();
		session_store_close(g_store);
		return 6;
	}
	if (pthread_attr_init(&worker_attr) == 0) {
		worker_attr_ready = 1;
		if (pthread_attr_setstacksize(&worker_attr,
					      FCGI_WORKER_STACK_SIZE) == 0)
			worker_attr_ptr = &worker_attr;
	}
	for (int i = 0; i < n_workers; i++) {
		if (pthread_create(&threads[i], worker_attr_ptr,
				   worker_main, NULL) != 0) {
			fprintf(stderr, "pthread_create #%d failed\n", i);
			n_workers = i;
			break;
		}
	}
	if (worker_attr_ready)
		pthread_attr_destroy(&worker_attr);
	for (int i = 0; i < n_workers; i++) pthread_join(threads[i], NULL);
	free(threads);

	fcgi_bridge_shutdown();
	session_store_close(g_store);
	fprintf(stderr, "morph-fastcgi: shutdown clean\n");
	return 0;
}
