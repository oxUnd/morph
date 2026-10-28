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
#include "util/name.h"
#include "util/signals.h"
#include <sys/socket.h>

#define FCGI_WORKERS_DEFAULT "128"
#define FCGI_WORKERS_MAX 512
#define FCGI_WORKER_STACK_SIZE (512U * 1024U)

struct fcgi_server {
	int listen_fd;
	volatile sig_atomic_t shutdown;
	volatile sig_atomic_t listen_closed;
	struct session_store *store;
	struct fcgi_bridge *bridge;
	struct fcgi_auth *auth;
	char trust_param[96];
	struct morph_signal_listener signals;
};

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

static int configure_trust_param(struct fcgi_server *server, const char *header)
{
	size_t off;

	server->trust_param[0] = '\0';
	if (!header || !*header)
		return 0;
	snprintf(server->trust_param, sizeof(server->trust_param), "HTTP_");
	off = strlen(server->trust_param);
	for (const unsigned char *p = (const unsigned char *)header; *p; p++) {
		if (off + 1 >= sizeof(server->trust_param))
			MORPH_RETURN(-ENAMETOOLONG);
		if (morph_name_char_valid((char)*p, NULL))
			server->trust_param[off++] = (char)toupper(*p);
		else if (*p == '-')
			server->trust_param[off++] = '_';
		else
			MORPH_RETURN(-EINVAL);
	}
	server->trust_param[off] = '\0';
	return 0;
}

static void server_stop(struct fcgi_server *server)
{
	morph_signal_flag_set(&server->shutdown, 1);
	FCGX_ShutdownPending();
	if (!__atomic_exchange_n(&server->listen_closed, 1, __ATOMIC_RELAXED)) {
		(void)shutdown(server->listen_fd, SHUT_RDWR);
		close(server->listen_fd);
	}
}

static void on_signal(int number, void *opaque)
{
	if (number != SIGWINCH)
		server_stop(opaque);
}

/* ---- per-thread accept loop ---- */
static void populate_request(struct fcgi_server *server, request_t *r, FCGX_Request *req)
{
	memset(r, 0, sizeof(*r));
	r->fcgx       = req;
	r->method     = FCGX_GetParam("REQUEST_METHOD",   req->envp);
	r->path       = FCGX_GetParam("DOCUMENT_URI",     req->envp);
	if (!r->path) r->path = FCGX_GetParam("REQUEST_URI", req->envp);
	r->query      = FCGX_GetParam("QUERY_STRING",     req->envp);
	r->auth_hdr   = FCGX_GetParam("HTTP_AUTHORIZATION", req->envp);
	r->trust_user =
		server->trust_param[0] ? FCGX_GetParam(server->trust_param, req->envp) : NULL;
	r->last_event_id = FCGX_GetParam("HTTP_LAST_EVENT_ID", req->envp);
	r->content_type  = FCGX_GetParam("CONTENT_TYPE",  req->envp);
	const char *clen = FCGX_GetParam("CONTENT_LENGTH", req->envp);
	r->content_length = clen ? atoi(clen) : 0;
	r->store = server->store;
	r->bridge = server->bridge;
	r->auth = server->auth;
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
	struct fcgi_server *server = arg;
	FCGX_Request req;
	if (FCGX_InitRequest(&req, server->listen_fd, 0) != 0)
		return NULL;

	while (!morph_signal_flag_get(&server->shutdown)) {
		int rc = FCGX_Accept_r(&req);
		if (rc < 0) break;

		request_t r;
		populate_request(server, &r, &req);
		router_dispatch(&r);

		FCGX_Finish_r(&req);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct fcgi_server state = {.listen_fd = -1};
	struct fcgi_server *server = &state;
	pthread_t *threads = NULL;
	pthread_attr_t attributes;
	int attributes_ready = 0;
	int started = 0;
	int result = 0;
	const char *listen_spec = getenv_or("MORPH_FCGI_LISTEN", "unix:/run/morph-fastcgi.sock");
	const char *database = getenv_or("MORPH_FCGI_DB", "/var/lib/morph/morph.db");
	const char *trust_header = getenv("MORPH_FCGI_TRUST_HDR");
	int workers = atoi(getenv_or("MORPH_FCGI_WORKERS", FCGI_WORKERS_DEFAULT));

	(void)argc;
	(void)argv;
	if (workers < 1)
		workers = 1;
	if (workers > FCGI_WORKERS_MAX)
		workers = FCGI_WORKERS_MAX;
	if (configure_trust_param(server, trust_header) != 0)
		return 4;
	if (morph_signals_block() != 0 || FCGX_Init() != 0)
		return 1;
	signal(SIGPIPE, SIG_IGN);
	server->listen_fd = open_listen(listen_spec);
	if (server->listen_fd < 0)
		return 2;
	result = morph_signal_listener_start(&server->signals, on_signal, server);
	if (result != 0) {
		close(server->listen_fd);
		return 1;
	}
	server->store = session_store_open(NULL, database);
	if (!server->store) {
		result = 3;
		goto out_stop_server;
	}
	if (fcgi_bridge_init(&server->bridge, database) != 0) {
		result = 5;
		goto out_stop_server;
	}
	server->auth = auth_create(trust_header);
	threads = calloc((size_t)workers, sizeof(*threads));
	if (!threads || !server->auth) {
		result = 6;
		goto out_stop_server;
	}
	if (pthread_attr_init(&attributes) == 0) {
		attributes_ready = 1;
		(void)pthread_attr_setstacksize(&attributes, FCGI_WORKER_STACK_SIZE);
	}
	fprintf(stderr, "morph-fastcgi: listening on %s, %d workers, db=%s\n", listen_spec, workers,
		database);
	for (; started < workers && !morph_signal_flag_get(&server->shutdown); started++) {
		int error = pthread_create(&threads[started], attributes_ready ? &attributes : NULL,
					   worker_main, server);

		if (error) {
			fprintf(stderr, "morph-fastcgi: worker creation failed: %s\n",
				morph_strerror(-error));
			result = 6;
			server_stop(server);
			break;
		}
	}
	if (attributes_ready)
		pthread_attr_destroy(&attributes);
	for (int i = 0; i < started; i++)
		pthread_join(threads[i], NULL);

out_stop_server:
	if (!morph_signal_flag_get(&server->listen_closed))
		server_stop(server);
	morph_signal_listener_stop(&server->signals);
	free(threads);
	auth_destroy(server->auth);
	fcgi_bridge_shutdown(server->bridge);
	session_store_close(server->store);
	return result;
}
