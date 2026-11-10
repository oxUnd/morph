/* agent_bridge.c -- FastCGI ownership of the shared Morph runtime pool. */
#include "agent_bridge.h"

#include "agent/tool_runtime.h"
#include "config/config.h"
#include "runtime/runtime.h"
#include "util/error.h"
#include "util/cleanup.h"
#include "util/file.h"
#include "util/log.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FCGI_RUNTIME_MIN_DEFAULT 8
#define FCGI_RUNTIME_MAX_DEFAULT 64
#define FCGI_RUNTIME_MAX_LIMIT 256
#define FCGI_RUNTIME_QUEUE_DEFAULT 256
#define FCGI_RUNTIME_QUEUE_LIMIT 65536
#define FCGI_RUNTIME_IDLE_DEFAULT 300
#define FCGI_RUNTIME_WAIT_DEFAULT 600
#define FCGI_RUNTIME_SESSION_ID_MAX 64
#define FCGI_REAPER_INTERVAL_MAX 30

struct bridge_runtime_slot {
	struct runtime *runtime;
	int busy;
	int elastic;
	time_t idle_since;
	char session_id[FCGI_RUNTIME_SESSION_ID_MAX];
};

struct fcgi_turn_ticket {
	struct fcgi_bridge *pool;
	struct fcgi_turn_ticket *next;
	char session_id[FCGI_RUNTIME_SESSION_ID_MAX];
	struct timespec deadline;
	int queued;
};

struct fcgi_bridge {
	struct morph_log logger;
	struct bridge_runtime_slot *slots;
	struct fcgi_turn_ticket *turn_head;
	struct fcgi_turn_ticket *turn_tail;
	int slot_count;
	int min_workers;
	int max_workers;
	int starting_workers;
	int waiting_turns;
	int queue_max;
	int idle_seconds;
	int wait_seconds;
	int outstanding_turns;
	int outstanding_tickets;
	int outstanding_leases;
	int stopping;
	int reaper_started;
	pthread_t reaper_thread;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	char config_path[PATH_MAX];
	char db_path[PATH_MAX];
	char output_dir[PATH_MAX];
};

static int bridge_env_int(const char *name, int fallback,
			  int minimum, int maximum)
{
	const char *value = getenv(name);
	char *end = NULL;
	long parsed;

	if (!value || !value[0])
		return fallback;
	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0')
		return fallback;
	if (parsed < minimum)
		return minimum;
	if (parsed > maximum)
		return maximum;
	return (int)parsed;
}

static int bridge_copy_path(char *dst, size_t dst_size, const char *src)
{
	int written;

	if (!dst || dst_size == 0 || !src || !src[0])
		return -EINVAL;
	written = snprintf(dst, dst_size, "%s", src);
	if (written < 0 || (size_t)written >= dst_size)
		return -ENAMETOOLONG;
	return 0;
}

static void bridge_runtime_options(struct fcgi_bridge *pool, struct runtime_options *options,
				   int process_replica)
{
	memset(options, 0, sizeof(*options));
	options->logger = &pool->logger;
	options->config_path = pool->config_path;
	options->db_path = pool->db_path;
	options->output_dir_override = pool->output_dir[0] ? pool->output_dir : NULL;
	options->default_dynamic_tools_mode = "server";
	options->front_name = "fastcgi";
	options->enable_exec = 1;
	options->enable_apply_patch = 1;
	options->enable_config_write = 1;
	options->enable_img_annotate = 0;
	options->enable_shell_exts = 1;
	options->enable_multi_agent = 1;
	options->allocate_skill_registry = 1;
	options->auto_connect_mcp = 1;
	options->create_new_session = 1;
	options->process_replica = process_replica;
}

static int bridge_runtime_open(struct fcgi_bridge *pool, int process_replica, struct runtime **out)
{
	struct runtime_options options;
	int rc;

	bridge_runtime_options(pool, &options, process_replica);
	rc = runtime_open(&options, out);
	if (rc != 0)
		return rc;
	return 0;
}

static void bridge_runtime_close_slots(struct bridge_runtime_slot *slots,
				       int slot_count)
{
	if (!slots)
		return;
	for (int i = slot_count - 1; i > 0; i--)
		runtime_close(slots[i].runtime);
	if (slot_count > 0)
		runtime_close(slots[0].runtime);
}

static void bridge_deadline(struct timespec *deadline, int seconds)
{
	clock_gettime(CLOCK_REALTIME, deadline);
	deadline->tv_sec += seconds;
}

static int bridge_find_free_slot(struct fcgi_bridge *pool, const char *session_id,
				 int *session_busy)
{
	int free_slot = -1;

	*session_busy = 0;
	for (int i = 0; i < pool->slot_count; i++) {
		if (pool->slots[i].busy && strcmp(pool->slots[i].session_id, session_id) == 0) {
			*session_busy = 1;
			return -1;
		}
		if (!pool->slots[i].busy && free_slot < 0)
			free_slot = i;
	}
	return free_slot;
}

static int bridge_find_expired_slot(struct fcgi_bridge *pool, time_t now)
{
	for (int i = pool->slot_count - 1; i >= 0; i--) {
		struct bridge_runtime_slot *slot = &pool->slots[i];

		if (slot->elastic && !slot->busy && slot->idle_since > 0 &&
		    now - slot->idle_since >= pool->idle_seconds)
			return i;
	}
	return -1;
}

static int bridge_session_busy(struct fcgi_bridge *pool, const char *session_id)
{
	for (int i = 0; i < pool->slot_count; i++)
		if (pool->slots[i].busy &&
		    strcmp(pool->slots[i].session_id, session_id) == 0)
			return 1;
	return 0;
}

static struct fcgi_turn_ticket *bridge_first_runnable_ticket(
	struct fcgi_bridge *pool)
{
	for (struct fcgi_turn_ticket *ticket = pool->turn_head; ticket;
	     ticket = ticket->next)
		if (!bridge_session_busy(pool, ticket->session_id))
			return ticket;
	return NULL;
}

static void bridge_ticket_unlink(struct fcgi_bridge *pool,
				 struct fcgi_turn_ticket *ticket)
{
	struct fcgi_turn_ticket **link = &pool->turn_head;

	while (*link && *link != ticket)
		link = &(*link)->next;
	if (!*link)
		return;
	*link = ticket->next;
	if (pool->turn_tail == ticket)
		pool->turn_tail = NULL;
	if (!pool->turn_head)
		pool->turn_tail = NULL;
	else if (!pool->turn_tail) {
		struct fcgi_turn_ticket *tail = pool->turn_head;

		while (tail->next)
			tail = tail->next;
		pool->turn_tail = tail;
	}
}

static void *bridge_reaper_main(void *user_data)
{
	struct fcgi_bridge *pool = user_data;
	pthread_mutex_lock(&pool->lock);
	while (!pool->stopping) {
		struct bridge_runtime_slot victim;
		struct timespec deadline;
		int interval;
		int index = -1;

		if (pool->idle_seconds > 0 && pool->slot_count > pool->min_workers)
			index = bridge_find_expired_slot(pool, time(NULL));
		if (index < 0) {
			interval = pool->idle_seconds > 0 &&
						   pool->idle_seconds < FCGI_REAPER_INTERVAL_MAX
					   ? pool->idle_seconds
					   : FCGI_REAPER_INTERVAL_MAX;
			bridge_deadline(&deadline, interval);
			(void)pthread_cond_timedwait(&pool->changed, &pool->lock, &deadline);
			continue;
		}
		victim = pool->slots[index];
		pool->slot_count--;
		if (index != pool->slot_count)
			pool->slots[index] = pool->slots[pool->slot_count];
		memset(&pool->slots[pool->slot_count], 0, sizeof(pool->slots[pool->slot_count]));
		pthread_mutex_unlock(&pool->lock);
		runtime_close(victim.runtime);
		log_info(&pool->logger, "fastcgi runtime pool shrank: workers=%d",
			 fcgi_bridge_runtime_count(pool));
		pthread_mutex_lock(&pool->lock);
	}
	pthread_mutex_unlock(&pool->lock);
	return NULL;
}

static int bridge_pool_configure(struct fcgi_bridge *pool, const char *db_path,
				 const char *config_path, const char *output_dir)
{
	const char *legacy_workers = getenv("MORPH_FCGI_RUNTIME_WORKERS");
	int legacy_default = FCGI_RUNTIME_MIN_DEFAULT;
	int rc;

	if (legacy_workers && legacy_workers[0])
		legacy_default = bridge_env_int("MORPH_FCGI_RUNTIME_WORKERS",
			FCGI_RUNTIME_MIN_DEFAULT, 1, FCGI_RUNTIME_MAX_LIMIT);
	pool->min_workers = bridge_env_int("MORPH_FCGI_RUNTIME_MIN_WORKERS", legacy_default, 1,
					   FCGI_RUNTIME_MAX_LIMIT);
	pool->max_workers = bridge_env_int("MORPH_FCGI_RUNTIME_MAX_WORKERS",
					   FCGI_RUNTIME_MAX_DEFAULT, 1, FCGI_RUNTIME_MAX_LIMIT);
	if (pool->max_workers < pool->min_workers)
		pool->max_workers = pool->min_workers;
	pool->queue_max = bridge_env_int("MORPH_FCGI_RUNTIME_QUEUE_MAX", FCGI_RUNTIME_QUEUE_DEFAULT,
					 1, FCGI_RUNTIME_QUEUE_LIMIT);
	pool->idle_seconds = bridge_env_int("MORPH_FCGI_RUNTIME_IDLE_SECONDS",
					    FCGI_RUNTIME_IDLE_DEFAULT, 0, INT_MAX);
	pool->wait_seconds = bridge_env_int("MORPH_FCGI_RUNTIME_WAIT_SECONDS",
					    FCGI_RUNTIME_WAIT_DEFAULT, 1, INT_MAX);
	rc = bridge_copy_path(pool->db_path, sizeof(pool->db_path), db_path);
	if (rc != 0)
		return rc;
	rc = bridge_copy_path(pool->config_path, sizeof(pool->config_path), config_path);
	if (rc != 0)
		return rc;
	pool->output_dir[0] = '\0';
	if (output_dir && output_dir[0]) {
		rc = bridge_copy_path(pool->output_dir, sizeof(pool->output_dir), output_dir);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int bridge_initialize(struct fcgi_bridge *pool, const char *db_path)
{
	const char *config_path;
	const char *output_dir;
	char *expanded_output = NULL;
	const char *log_file;
	char default_config[PATH_MAX];
	const char *home;
	int rc;

	if (!db_path || !db_path[0])
		return -EINVAL;
	pthread_mutex_lock(&pool->lock);
	if (pool->slots) {
		pthread_mutex_unlock(&pool->lock);
		return 0;
	}
	pthread_mutex_unlock(&pool->lock);
	home = getenv("HOME");
	config_path = getenv("MORPH_FCGI_CONFIG");
	if (!config_path || !config_path[0]) {
		if (!home || !home[0])
			return MORPH_ERR_NOT_CONFIGURED;
		snprintf(default_config, sizeof(default_config),
			 "%s/.morph/config.toml", home);
		config_path = default_config;
	}
	output_dir = getenv("MORPH_FCGI_OUTPUT_DIR");
	if (output_dir && output_dir[0]) {
		expanded_output = file_expand_path_copy(output_dir);
		if (!expanded_output)
			return -ENOMEM;
		rc = file_ensure_dir(expanded_output);
		if (rc != 0) {
			free(expanded_output);
			return rc;
		}
		output_dir = expanded_output;
	}
	log_file = getenv("MORPH_FCGI_LOG_FILE");
	if (!log_file || !log_file[0])
		log_file = "/tmp/morph-fastcgi.log";
	rc = log_init(&pool->logger, log_file, getenv("MORPH_DEBUG") ? LOG_DEBUG : LOG_INFO);
	if (rc != 0) {
		free(expanded_output);
		return rc;
	}
	rc = bridge_pool_configure(pool, db_path, config_path, output_dir);
	free(expanded_output);
	if (rc != 0)
		return rc;
	pool->slots = calloc((size_t)pool->max_workers, sizeof(*pool->slots));
	if (!pool->slots)
		return -ENOMEM;
	pool->slot_count = 0;
	pool->starting_workers = 0;
	pool->waiting_turns = 0;
	pool->outstanding_turns = 0;
	pool->stopping = 0;
	for (int i = 0; i < pool->min_workers; i++) {
		rc = bridge_runtime_open(pool, i > 0, &pool->slots[i].runtime);
		if (rc != 0) {
			bridge_runtime_close_slots(pool->slots, i);
			free(pool->slots);
			pool->slots = NULL;
			fprintf(stderr, "fcgi-bridge: runtime worker %d "
				"initialization failed: %s\n", i,
				morph_strerror(rc));
			return rc;
		}
		pool->slots[i].idle_since = time(NULL);
		pool->slot_count++;
	}
	rc = pthread_create(&pool->reaper_thread, NULL, bridge_reaper_main, pool);
	if (rc != 0) {
		bridge_runtime_close_slots(pool->slots, pool->slot_count);
		free(pool->slots);
		pool->slots = NULL;
		pool->slot_count = 0;
		return -rc;
	}
	pool->reaper_started = 1;
	log_info(&pool->logger, "fastcgi elastic runtime pool ready: min=%d max=%d queue=%d",
		 pool->min_workers, pool->max_workers, pool->queue_max);
	return 0;
}

static void bridge_shutdown(struct fcgi_bridge *pool)
{
	struct bridge_runtime_slot *slots;
	int slot_count;
	int join_reaper;

	pthread_mutex_lock(&pool->lock);
	if (!pool->slots) {
		pthread_mutex_unlock(&pool->lock);
		return;
	}
	pool->stopping = 1;
	join_reaper = pool->reaper_started;
	pool->reaper_started = 0;
	pthread_cond_broadcast(&pool->changed);
	pthread_mutex_unlock(&pool->lock);
	if (join_reaper)
		pthread_join(pool->reaper_thread, NULL);
	pthread_mutex_lock(&pool->lock);
	while (pool->outstanding_turns > 0 || pool->outstanding_tickets > 0 ||
	       pool->outstanding_leases > 0)
		pthread_cond_wait(&pool->changed, &pool->lock);
	slots = pool->slots;
	slot_count = pool->slot_count;
	pool->slots = NULL;
	pool->slot_count = 0;
	pthread_mutex_unlock(&pool->lock);
	bridge_runtime_close_slots(slots, slot_count);
	free(slots);
}

struct runtime *fcgi_bridge_runtime_borrow(struct fcgi_bridge *pool)
{
	struct runtime *runtime = NULL;

	pthread_mutex_lock(&pool->lock);
	if (pool->slots && pool->slot_count > 0)
		runtime = pool->slots[0].runtime;
	pthread_mutex_unlock(&pool->lock);
	return runtime;
}

int fcgi_bridge_turn_enqueue(struct fcgi_bridge *pool, const char *session_id,
			     struct fcgi_turn_ticket **out, int *position)
{
	struct fcgi_turn_ticket *ticket;
	int rc = 0;

	if (!pool || !session_id || !session_id[0] || !out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	ticket = calloc(1, sizeof(*ticket));
	if (!ticket)
		return -ENOMEM;
	pthread_mutex_lock(&pool->lock);
	if (pool->stopping || !pool->slots) {
		rc = -ESHUTDOWN;
	} else if (pool->waiting_turns >= pool->queue_max) {
		rc = -EAGAIN;
	} else {
		snprintf(ticket->session_id, sizeof(ticket->session_id), "%s", session_id);
		bridge_deadline(&ticket->deadline, pool->wait_seconds);
		ticket->queued = 1;
		ticket->pool = pool;
		if (pool->turn_tail)
			pool->turn_tail->next = ticket;
		else
			pool->turn_head = ticket;
		pool->turn_tail = ticket;
		pool->waiting_turns++;
		pool->outstanding_tickets++;
		if (position)
			*position = pool->waiting_turns;
		pthread_cond_broadcast(&pool->changed);
		*out = ticket;
	}
	pthread_mutex_unlock(&pool->lock);
	if (rc != 0)
		free(ticket);
	return rc;
}

static void bridge_ticket_cancel_locked(struct fcgi_bridge *pool, struct fcgi_turn_ticket *ticket)
{
	if (!ticket->queued)
		return;
	bridge_ticket_unlink(pool, ticket);
	ticket->queued = 0;
	if (pool->waiting_turns > 0)
		pool->waiting_turns--;
	pthread_cond_broadcast(&pool->changed);
}

void fcgi_bridge_turn_cancel(struct fcgi_bridge *pool, struct fcgi_turn_ticket *ticket)
{
	if (!pool || !ticket || ticket->pool != pool)
		return;
	pthread_mutex_lock(&pool->lock);
	bridge_ticket_cancel_locked(pool, ticket);
	pthread_mutex_unlock(&pool->lock);
}

void fcgi_turn_ticket_destroy(struct fcgi_turn_ticket *ticket)
{
	struct fcgi_bridge *pool;

	if (!ticket)
		return;
	pool = ticket->pool;
	if (pool) {
		pthread_mutex_lock(&pool->lock);
		bridge_ticket_cancel_locked(pool, ticket);
		if (pool->outstanding_tickets > 0)
			pool->outstanding_tickets--;
		ticket->pool = NULL;
		pthread_cond_broadcast(&pool->changed);
		pthread_mutex_unlock(&pool->lock);
	}
	free(ticket);
}

void fcgi_turn_ticket_cleanup(struct fcgi_turn_ticket **owner)
{
	struct fcgi_turn_ticket *ticket;

	if (!owner)
		return;
	ticket = *owner;
	*owner = NULL;
	fcgi_turn_ticket_destroy(ticket);
}

int fcgi_bridge_turn_acquire(struct fcgi_bridge *pool, struct fcgi_turn_ticket *ticket,
			     struct runtime **out)
{
	int rc = 0;

	if (!pool || !ticket || ticket->pool != pool || !out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	pthread_mutex_lock(&pool->lock);
	for (;;) {
		struct runtime *created = NULL;
		struct fcgi_turn_ticket *first;
		int session_busy;
		int free_slot;

		if (pool->stopping || !pool->slots) {
			rc = -ESHUTDOWN;
			break;
		}
		if (!ticket->queued) {
			rc = -ECANCELED;
			break;
		}
		first = bridge_first_runnable_ticket(pool);
		if (first != ticket) {
			rc = pthread_cond_timedwait(&pool->changed, &pool->lock, &ticket->deadline);
			if (rc == ETIMEDOUT) {
				rc = -ETIMEDOUT;
				break;
			}
			if (rc != 0) {
				rc = -rc;
				break;
			}
			continue;
		}
		free_slot = bridge_find_free_slot(pool, ticket->session_id, &session_busy);
		if (!session_busy && free_slot >= 0) {
			struct bridge_runtime_slot *slot = &pool->slots[free_slot];

			slot->busy = 1;
			pool->outstanding_leases++;
			slot->idle_since = 0;
			snprintf(slot->session_id, sizeof(slot->session_id), "%s",
				 ticket->session_id);
			*out = slot->runtime;
			rc = 0;
			break;
		}
		if (!session_busy &&
		    pool->slot_count + pool->starting_workers < pool->max_workers) {
			pool->starting_workers++;
			pthread_mutex_unlock(&pool->lock);
			rc = bridge_runtime_open(pool, 1, &created);
			pthread_mutex_lock(&pool->lock);
			pool->starting_workers--;
			pthread_cond_broadcast(&pool->changed);
			if (rc != 0)
				break;
			if (pool->stopping || !pool->slots) {
				pthread_mutex_unlock(&pool->lock);
				runtime_close(created);
				pthread_mutex_lock(&pool->lock);
				rc = -ESHUTDOWN;
				break;
			}
			if (pool->slot_count >= pool->max_workers) {
				pthread_mutex_unlock(&pool->lock);
				runtime_close(created);
				pthread_mutex_lock(&pool->lock);
				continue;
			}
			pool->slots[pool->slot_count].runtime = created;
			pool->slots[pool->slot_count].elastic = 1;
			pool->slots[pool->slot_count].idle_since = time(NULL);
			pool->slot_count++;
			log_info(&pool->logger, "fastcgi runtime pool grew: workers=%d",
				 pool->slot_count);
			continue;
		}
		rc = pthread_cond_timedwait(&pool->changed, &pool->lock, &ticket->deadline);
		if (rc == ETIMEDOUT) {
			rc = -ETIMEDOUT;
			break;
		}
		if (rc != 0) {
			rc = -rc;
			break;
		}
	}
	if (ticket->queued) {
		bridge_ticket_unlink(pool, ticket);
		ticket->queued = 0;
		if (pool->waiting_turns > 0)
			pool->waiting_turns--;
		pthread_cond_broadcast(&pool->changed);
	}
	pthread_mutex_unlock(&pool->lock);
	return rc;
}

int fcgi_bridge_runtime_acquire(struct fcgi_bridge *pool, const char *session_id,
				struct runtime **out)
{
	struct fcgi_turn_ticket *ticket MORPH_CLEANUP(fcgi_turn_ticket_cleanup) = NULL;
	int rc;

	rc = fcgi_bridge_turn_enqueue(pool, session_id, &ticket, NULL);
	if (rc != 0)
		return rc;
	return fcgi_bridge_turn_acquire(pool, ticket, out);
}

void fcgi_bridge_runtime_release(struct fcgi_bridge *pool, struct runtime *runtime)
{
	if (!runtime)
		return;
	pthread_mutex_lock(&pool->lock);
	for (int i = 0; i < pool->slot_count; i++) {
		if (pool->slots[i].runtime != runtime || !pool->slots[i].busy)
			continue;
		pool->slots[i].busy = 0;
		pool->outstanding_leases--;
		pool->slots[i].idle_since = time(NULL);
		pool->slots[i].session_id[0] = '\0';
		pthread_cond_broadcast(&pool->changed);
		break;
	}
	pthread_mutex_unlock(&pool->lock);
}

int fcgi_bridge_turn_begin(struct fcgi_bridge *pool)
{
	pthread_mutex_lock(&pool->lock);
	if (pool->stopping || !pool->slots) {
		pthread_mutex_unlock(&pool->lock);
		return -ESHUTDOWN;
	}
	pool->outstanding_turns++;
	pthread_mutex_unlock(&pool->lock);
	return 0;
}

void fcgi_bridge_turn_end(struct fcgi_bridge *pool)
{
	pthread_mutex_lock(&pool->lock);
	if (pool->outstanding_turns > 0)
		pool->outstanding_turns--;
	if (pool->outstanding_turns == 0)
		pthread_cond_broadcast(&pool->changed);
	pthread_mutex_unlock(&pool->lock);
}

void fcgi_bridge_pool_status(struct fcgi_bridge *pool, struct fcgi_bridge_pool_status *status)
{
	if (!status)
		return;
	memset(status, 0, sizeof(*status));
	pthread_mutex_lock(&pool->lock);
	status->workers = pool->slot_count;
	status->min_workers = pool->min_workers;
	status->max_workers = pool->max_workers;
	status->starting_workers = pool->starting_workers;
	status->waiting_turns = pool->waiting_turns;
	status->queue_max = pool->queue_max;
	status->idle_seconds = pool->idle_seconds;
	for (int i = 0; i < pool->slot_count; i++)
		if (pool->slots[i].busy)
			status->busy_workers++;
	pthread_mutex_unlock(&pool->lock);
}

int fcgi_bridge_runtime_count(struct fcgi_bridge *pool)
{
	struct fcgi_bridge_pool_status status;

	fcgi_bridge_pool_status(pool, &status);
	return status.workers;
}

const struct config *fcgi_bridge_config_borrow(struct fcgi_bridge *pool)
{
	struct runtime *runtime = fcgi_bridge_runtime_borrow(pool);

	return runtime ? runtime_config_borrow(runtime) : NULL;
}

const char *fcgi_artifact_output_dir_borrow(struct fcgi_bridge *pool)
{
	const struct config *config = fcgi_bridge_config_borrow(pool);
	if (config && config->general.output_dir[0])
		return config->general.output_dir;
	return "/var/lib/morph/output";
}

int fcgi_bridge_init(struct fcgi_bridge **out, const char *db_path)
{
	struct fcgi_bridge *pool;
	int error;

	if (!out)
		return -EINVAL;
	if (*out)
		return -EEXIST;
	pool = calloc(1, sizeof(*pool));
	if (!pool)
		return -ENOMEM;
	error = pthread_mutex_init(&pool->lock, NULL);
	if (error) {
		free(pool);
		return -error;
	}
	error = pthread_cond_init(&pool->changed, NULL);
	if (error) {
		pthread_mutex_destroy(&pool->lock);
		free(pool);
		return -error;
	}
	error = bridge_initialize(pool, db_path);
	if (error) {
		fcgi_bridge_shutdown(pool);
		return error;
	}
	*out = pool;
	return 0;
}

void fcgi_bridge_shutdown(struct fcgi_bridge *pool)
{
	if (!pool)
		return;
	bridge_shutdown(pool);
	log_shutdown(&pool->logger);
	pthread_cond_destroy(&pool->changed);
	pthread_mutex_destroy(&pool->lock);
	free(pool);
}
