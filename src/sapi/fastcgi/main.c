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
#include "nb/cleanup.h"
#include <fcgiapp.h>
#include "sapi/fastcgi/storage/events.h"
#include "sapi/fastcgi/storage/tokens.h"
#include "nb/time.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include "nb/thread.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "fcgi_io.h"
#include "session_store.h"
#include "router.h"
#include "auth.h"
#include "agent_bridge.h"
#include "stream_pool.h"
#include "errors.h"
#include "nb/log.h"
#include "nb/str.h"
#include "nb/signal.h"
#include <sys/socket.h>

#define FCGI_WORKERS_DEFAULT "128"
#define FCGI_WORKERS_MAX 512
#define FCGI_WORKER_STACK_SIZE (512U * 1024U)
#define FCGI_STREAM_THREADS_DEFAULT 32
#define FCGI_STREAM_THREADS_MAX 256
#define FCGI_STREAM_QUEUE_DEFAULT 256
#define FCGI_STREAM_QUEUE_MAX 65536

struct fcgi_server {
	int listen_fd;
	nb_atomic_int shutdown;
	nb_atomic_int listen_closed;
	struct session_store *store;
	struct fcgi_bridge *bridge;
	struct fcgi_auth *auth;
	struct fcgi_stream_pool *streams;
	nb_thread *maintenance_thread;
	int maintenance_started;
	char socket_path[PATH_MAX];
	char trust_param[96];
	nb_signal_listener *signals;
	struct nb_log *logger;
	int logger_ready;
	struct fcgi_metrics metrics;
	unsigned long long request_seq;
};

static const char *getenv_or(const char *k, const char *fallback)
{
	const char *v = getenv(k);
	return (v && *v) ? v : fallback;
}

static long long now_ms(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? (long long)(now / 1000000) : 0;
}

static void remove_stale_socket(const char *path)
{
	int fd;
	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path) >=
	    (int)sizeof(addr.sun_path))
		return;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno == ECONNREFUSED)
		(void)unlink(path);
	close(fd);
}

static int open_listen(struct fcgi_server *server, const char *spec)
{
	/* libfcgi accepts ":port" / "host:port" / "/path/to/sock" */
	const char *socket_spec = strncmp(spec, "unix:", 5) == 0 ? spec + 5 : spec;
	int backlog = 128;
	if (socket_spec[0] == '/') {
		snprintf(server->socket_path, sizeof(server->socket_path), "%s", socket_spec);
		remove_stale_socket(socket_spec);
	}
	int fd = FCGX_OpenSocket(socket_spec, backlog);
	if (fd < 0) {
		fprintf(stderr, "FCGX_OpenSocket(%s) failed\n", spec);
		return NB_ERR_IO;
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
			return NB_ERR_NAME_TOO_LONG;
		if (nb_ascii_is_alnum((unsigned char)((char)*p)))
			server->trust_param[off++] = (char)toupper(*p);
		else if (*p == '-')
			server->trust_param[off++] = '_';
		else
			return NB_ERR_INVAL;
	}
	server->trust_param[off] = '\0';
	return 0;
}

static void server_stop(struct fcgi_server *server)
{
	nb_atomic_int_store(&server->shutdown, 1);
	FCGX_ShutdownPending();
	store_wake_all(server->store);
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
	r->fcgx = req;
	r->logger = server->logger_ready ? server->logger : NULL;
	r->metrics = &server->metrics;
	r->started_ms = now_ms();
	__atomic_add_fetch(&server->metrics.requests_total, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&server->metrics.active, 1, __ATOMIC_RELAXED);
	r->method = FCGX_GetParam("REQUEST_METHOD", req->envp);
	r->path = FCGX_GetParam("DOCUMENT_URI", req->envp);
	if (!r->path)
		r->path = FCGX_GetParam("REQUEST_URI", req->envp);
	r->query = FCGX_GetParam("QUERY_STRING", req->envp);
	r->auth_hdr = FCGX_GetParam("HTTP_AUTHORIZATION", req->envp);
	r->trust_user =
		server->trust_param[0] ? FCGX_GetParam(server->trust_param, req->envp) : NULL;
	r->last_event_id = FCGX_GetParam("HTTP_LAST_EVENT_ID", req->envp);
	r->content_type = FCGX_GetParam("CONTENT_TYPE", req->envp);
	const char *clen = FCGX_GetParam("CONTENT_LENGTH", req->envp);
	r->content_length = 0;
	if (clen && *clen) {
		char *end = NULL;
		long value;

		errno = 0;
		value = strtol(clen, &end, 10);
		if (errno != 0 || end == clen || *end != '\0' || value < 0)
			r->content_length = INT_MAX;
		else if (value > INT_MAX)
			r->content_length = INT_MAX;
		else
			r->content_length = (int)value;
	}
	r->store = server->store;
	r->bridge = server->bridge;
	r->auth = server->auth;
	r->streams = server->streams;
	r->shutdown = &server->shutdown;
	{
		const char *incoming = FCGX_GetParam("HTTP_X_REQUEST_ID", req->envp);
		size_t n = 0;

		if (incoming) {
			for (; *incoming && n < sizeof(r->request_id) - 1; incoming++) {
				unsigned char c = (unsigned char)*incoming;

				if (isalnum(c) || c == '-' || c == '_' || c == '.')
					r->request_id[n++] = (char)c;
			}
		}
		if (n == 0)
			snprintf(r->request_id, sizeof(r->request_id), "req-%llu-%lld",
				 (unsigned long long)__atomic_add_fetch(&server->request_seq, 1,
									__ATOMIC_RELAXED),
				 now_ms());
		else
			r->request_id[n] = '\0';
	}
	snprintf(r->user_id, sizeof(r->user_id), "anonymous");
	snprintf(r->username, sizeof(r->username), "anonymous");
	snprintf(r->role, sizeof(r->role), "anonymous");
	/* strip query from path */
	if (r->path) {
		char *q = strchr((char *)r->path, '?');
		if (q)
			*q = '\0';
	}
}

static void *worker_main(void *arg)
{
	struct fcgi_server *server = arg;
	FCGX_Request *req = calloc(1, sizeof(*req));

	if (!req || FCGX_InitRequest(req, server->listen_fd, 0) != 0) {
		free(req);
		return NULL;
	}

	while (!nb_atomic_int_load(&server->shutdown)) {
		int rc = FCGX_Accept_r(req);
		if (rc < 0)
			break;

		request_t r;
		char log_method[BUFSIZ];
		char log_path[PATH_MAX];

		populate_request(server, &r, req);
		snprintf(log_method, sizeof(log_method), "%s", r.method ? r.method : "-");
		snprintf(log_path, sizeof(log_path), "%s", r.path ? r.path : "-");
		int dispatched = router_dispatch(&r);
		long long duration = now_ms() - r.started_ms;

		if (r.status == 0)
			r.status = 200;
		if (r.logger)
			nb_log_write(r.logger, NB_LOG_INFO,
				     "access method=%s path=%s status=%d "
				     "duration_ms=%lld request_id=%s user=%s "
				     "stream=%d",
				     log_method, log_path, r.status, duration, r.request_id,
				     r.user_id, r.handed_off);
		__atomic_sub_fetch(&server->metrics.active, 1, __ATOMIC_RELAXED);
		if (!dispatched) {
			FCGX_Finish_r(req);
			if (FCGX_InitRequest(req, server->listen_fd, 0) != 0)
				break;
			continue;
		}
		/*
		 * The stream job now owns this request, including the streams
		 * that point back at it. Never finish or reuse it here.
		 */
		req = NULL;
		req = calloc(1, sizeof(*req));
		if (!req || FCGX_InitRequest(req, server->listen_fd, 0) != 0) {
			free(req);
			req = NULL;
			break;
		}
	}
	free(req);
	return NULL;
}

static void *maintenance_main(void *arg)
{
	struct fcgi_server *server = arg;
	const struct timespec tick = {0, 200 * 1000 * 1000};

	while (!nb_atomic_int_load(&server->shutdown)) {
		(void)store_maintenance(server->store);
		(void)login_token_prune();
		for (int i = 0; i < 300 && !nb_atomic_int_load(&server->shutdown); i++)
			nanosleep(&tick, NULL);
	}
	return NULL;
}

static int server_run_workers(struct fcgi_server *server, const char *database,
			      const char *trust_header, int stream_threads, int stream_queue,
			      int workers, const char *listen_spec, nb_thread ***thread_storage)
{
	nb_thread **threads = NULL;
	nb_thread_options options = {.stack_size = FCGI_WORKER_STACK_SIZE};
	int started = 0;
	int result = 0;

	server->store = session_store_open(NULL, database);
	if (!server->store) {
		result = 3;
		return result;
	}
	if (fcgi_bridge_init(&server->bridge, database) != 0) {
		result = 5;
		return result;
	}
	server->auth = auth_create(trust_header);
	server->streams = fcgi_stream_pool_create(stream_threads, stream_queue, NULL);
	threads = calloc((size_t)workers, sizeof(*threads));
	*thread_storage = threads;
	if (!threads || !server->auth || !server->streams) {
		result = 6;
		return result;
	}
	if (nb_thread_create(&server->maintenance_thread, maintenance_main, server, NULL) == 0)
		server->maintenance_started = 1;
	fprintf(stderr, "morph-fastcgi: listening on %s, %d workers, db=%s\n", listen_spec, workers,
		database);
	for (; started < workers && !nb_atomic_int_load(&server->shutdown); started++) {
		int error = nb_thread_create(&threads[started], worker_main, server, &options);

		if (error) {
			fprintf(stderr, "morph-fastcgi: worker creation failed: %s\n",
				nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, error));
			result = 6;
			server_stop(server);
			break;
		}
	}
	for (int i = 0; i < started; i++)
		nb_thread_join_take(&threads[i], NULL);
	if (server->maintenance_started) {
		nb_thread_join_take(&server->maintenance_thread, NULL);
		server->maintenance_started = 0;
	}
	return result;
}

int main(int argc, char **argv)
{
	struct fcgi_server state = {.listen_fd = -1};
	struct fcgi_server *server = &state;
	nb_thread **threads = NULL;
	int result = 0;
	const char *listen_spec = getenv_or("MORPH_FCGI_LISTEN", "unix:/run/morph-fastcgi.sock");
	const char *database = getenv_or("MORPH_FCGI_DB", "/var/lib/morph/morph.db");
	const char *trust_header = getenv("MORPH_FCGI_TRUST_HDR");
	int workers = atoi(getenv_or("MORPH_FCGI_WORKERS", FCGI_WORKERS_DEFAULT));
	int stream_threads = atoi(getenv_or("MORPH_FCGI_STREAM_THREADS", "32"));
	int stream_queue = atoi(getenv_or("MORPH_FCGI_STREAM_QUEUE", "256"));
	const char *log_file = getenv("MORPH_FCGI_LOG_FILE");

	(void)argc;
	(void)argv;
	if (log_file && *log_file &&
	    (server->logger = nb_log_new_file(NB_LOG_INFO, log_file, 5 * 1024 * 1024, 3)))
		server->logger_ready = 1;
	if (workers < 1)
		workers = 1;
	if (workers > FCGI_WORKERS_MAX)
		workers = FCGI_WORKERS_MAX;
	if (stream_threads < 1)
		stream_threads = 1;
	if (stream_threads > FCGI_STREAM_THREADS_MAX)
		stream_threads = FCGI_STREAM_THREADS_MAX;
	if (stream_queue < 1)
		stream_queue = 1;
	if (stream_queue > FCGI_STREAM_QUEUE_MAX)
		stream_queue = FCGI_STREAM_QUEUE_MAX;
	if (configure_trust_param(server, trust_header) != 0)
		return 4;
	if (FCGX_Init() != 0)
		return 1;
	signal(SIGPIPE, SIG_IGN);
	server->listen_fd = open_listen(server, listen_spec);
	if (server->listen_fd < 0)
		return 2;
	result = nb_signal_listener_start(&server->signals,
					  (const int[]){SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH},
					  5, on_signal, server, NULL);
	if (result != 0) {
		close(server->listen_fd);
		return 1;
	}
	result = server_run_workers(server, database, trust_header, stream_threads,
				    stream_queue, workers, listen_spec, &threads);
	if (!nb_atomic_int_load(&server->listen_closed))
		server_stop(server);
	nb_signal_listener_stop(&server->signals, NULL);
	fcgi_stream_pool_destroy(server->streams);
	server->streams = NULL;
	if (server->socket_path[0])
		(void)unlink(server->socket_path);
	free(threads);
	auth_destroy(server->auth);
	fcgi_bridge_shutdown(server->bridge);
	session_store_close(server->store);
	if (server->logger_ready) {
		nb_log_free(server->logger);
		server->logger = NULL;
		server->logger_ready = 0;
	}
	return result;
}
