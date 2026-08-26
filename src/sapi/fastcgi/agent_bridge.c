/* agent_bridge.c -- FastCGI ownership of the shared Morph runtime pool. */
#include "agent_bridge.h"

#include "agent/tool_runtime.h"
#include "config/config.h"
#include "runtime/runtime.h"
#include "util/error.h"
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

static struct {
	struct bridge_runtime_slot *slots;
	int slot_count;
	int min_workers;
	int max_workers;
	int starting_workers;
	int waiting_turns;
	int queue_max;
	int idle_seconds;
	int wait_seconds;
	int outstanding_turns;
	int stopping;
	int reaper_started;
	pthread_t reaper_thread;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	char config_path[PATH_MAX];
	char db_path[PATH_MAX];
	char output_dir[PATH_MAX];
} g_pool = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.changed = PTHREAD_COND_INITIALIZER,
};

/* Implemented by handlers/turns.c. */
int fcgi_turn_ask_user(const char *question,
		       const char *const *choices, int choices_count,
		       const char *selection_mode, int min_choices,
		       int max_choices, char ***answers, int *answers_count,
		       void *turn_data);
enum tool_operation_verdict
fcgi_turn_operation_approval(const struct tool_operation *op,
			     void *turn_data);

static int bridge_ask_user(const char *question,
			   const char *const *choices, int choices_count,
			   const char *selection_mode, int min_choices,
			   int max_choices, char ***answers,
			   int *answers_count, void *user_data)
{
	const struct tool_runtime_context *tool_runtime;

	(void)user_data;
	tool_runtime = tool_runtime_get_current();
	if (!tool_runtime || !tool_runtime->event_user_data)
		MORPH_RETURN(MORPH_ERR_NOT_INITIALIZED);
	return fcgi_turn_ask_user(question, choices, choices_count,
		selection_mode, min_choices, max_choices, answers,
		answers_count, tool_runtime->event_user_data);
}

static enum tool_operation_verdict bridge_operation_approval(
	const struct tool_operation *op, void *user_data)
{
	const struct tool_runtime_context *tool_runtime;

	(void)user_data;
	tool_runtime = tool_runtime_get_current();
	if (!tool_runtime || !tool_runtime->event_user_data)
		return TOOL_OP_DENY;
	return fcgi_turn_operation_approval(
		op, tool_runtime->event_user_data);
}

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
		MORPH_RETURN(-EINVAL);
	written = snprintf(dst, dst_size, "%s", src);
	if (written < 0 || (size_t)written >= dst_size)
		MORPH_RETURN(-ENAMETOOLONG);
	return 0;
}

static void bridge_runtime_options(struct runtime_options *options,
				   int process_replica)
{
	memset(options, 0, sizeof(*options));
	options->config_path = g_pool.config_path;
	options->db_path = g_pool.db_path;
	options->output_dir_override = g_pool.output_dir[0]
		? g_pool.output_dir : NULL;
	options->default_dynamic_tools_mode = "server";
	options->front_name = "fastcgi";
	options->ask_user_cb = bridge_ask_user;
	options->operation_approval_cb = bridge_operation_approval;
	options->enable_bash = 1;
	options->enable_apply_patch = 1;
	options->enable_config_write = 1;
	options->enable_img_annotate = 0;
	options->enable_shell_exts = 1;
	options->enable_sub_agents = 1;
	options->allocate_skill_registry = 1;
	options->auto_connect_mcp = 1;
	options->create_new_session = 1;
	options->process_replica = process_replica;
}

static int bridge_runtime_open(int process_replica, struct runtime **out)
{
	struct runtime_options options;
	int rc;

	bridge_runtime_options(&options, process_replica);
	rc = runtime_open(&options, out);
	if (rc != 0)
		MORPH_RETURN(rc);
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

static int bridge_find_free_slot(const char *session_id,
				 int *session_busy)
{
	int free_slot = -1;

	*session_busy = 0;
	for (int i = 0; i < g_pool.slot_count; i++) {
		if (g_pool.slots[i].busy &&
		    strcmp(g_pool.slots[i].session_id, session_id) == 0) {
			*session_busy = 1;
			return -1;
		}
		if (!g_pool.slots[i].busy && free_slot < 0)
			free_slot = i;
	}
	return free_slot;
}

static int bridge_find_expired_slot(time_t now)
{
	for (int i = g_pool.slot_count - 1; i >= 0; i--) {
		struct bridge_runtime_slot *slot = &g_pool.slots[i];

		if (slot->elastic && !slot->busy && slot->idle_since > 0 &&
		    now - slot->idle_since >= g_pool.idle_seconds)
			return i;
	}
	return -1;
}

static void *bridge_reaper_main(void *user_data)
{
	(void)user_data;
	pthread_mutex_lock(&g_pool.lock);
	while (!g_pool.stopping) {
		struct bridge_runtime_slot victim;
		struct timespec deadline;
		int interval;
		int index = -1;

		if (g_pool.idle_seconds > 0 &&
		    g_pool.slot_count > g_pool.min_workers)
			index = bridge_find_expired_slot(time(NULL));
		if (index < 0) {
			interval = g_pool.idle_seconds > 0 &&
				g_pool.idle_seconds < FCGI_REAPER_INTERVAL_MAX
				? g_pool.idle_seconds : FCGI_REAPER_INTERVAL_MAX;
			bridge_deadline(&deadline, interval);
			(void)pthread_cond_timedwait(&g_pool.changed,
				&g_pool.lock, &deadline);
			continue;
		}
		victim = g_pool.slots[index];
		g_pool.slot_count--;
		if (index != g_pool.slot_count)
			g_pool.slots[index] = g_pool.slots[g_pool.slot_count];
		memset(&g_pool.slots[g_pool.slot_count], 0,
		       sizeof(g_pool.slots[g_pool.slot_count]));
		pthread_mutex_unlock(&g_pool.lock);
		runtime_close(victim.runtime);
		log_info("fastcgi runtime pool shrank: workers=%d",
			 fcgi_bridge_runtime_count());
		pthread_mutex_lock(&g_pool.lock);
	}
	pthread_mutex_unlock(&g_pool.lock);
	return NULL;
}

static int bridge_pool_configure(const char *db_path,
				 const char *config_path,
				 const char *output_dir)
{
	const char *legacy_workers = getenv("MORPH_FCGI_RUNTIME_WORKERS");
	int legacy_default = FCGI_RUNTIME_MIN_DEFAULT;
	int rc;

	if (legacy_workers && legacy_workers[0])
		legacy_default = bridge_env_int("MORPH_FCGI_RUNTIME_WORKERS",
			FCGI_RUNTIME_MIN_DEFAULT, 1, FCGI_RUNTIME_MAX_LIMIT);
	g_pool.min_workers = bridge_env_int("MORPH_FCGI_RUNTIME_MIN_WORKERS",
		legacy_default, 1, FCGI_RUNTIME_MAX_LIMIT);
	g_pool.max_workers = bridge_env_int("MORPH_FCGI_RUNTIME_MAX_WORKERS",
		FCGI_RUNTIME_MAX_DEFAULT, 1, FCGI_RUNTIME_MAX_LIMIT);
	if (g_pool.max_workers < g_pool.min_workers)
		g_pool.max_workers = g_pool.min_workers;
	g_pool.queue_max = bridge_env_int("MORPH_FCGI_RUNTIME_QUEUE_MAX",
		FCGI_RUNTIME_QUEUE_DEFAULT, 1, FCGI_RUNTIME_QUEUE_LIMIT);
	g_pool.idle_seconds = bridge_env_int("MORPH_FCGI_RUNTIME_IDLE_SECONDS",
		FCGI_RUNTIME_IDLE_DEFAULT, 0, INT_MAX);
	g_pool.wait_seconds = bridge_env_int("MORPH_FCGI_RUNTIME_WAIT_SECONDS",
		FCGI_RUNTIME_WAIT_DEFAULT, 1, INT_MAX);
	rc = bridge_copy_path(g_pool.db_path, sizeof(g_pool.db_path), db_path);
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = bridge_copy_path(g_pool.config_path, sizeof(g_pool.config_path),
		config_path);
	if (rc != 0)
		MORPH_RETURN(rc);
	g_pool.output_dir[0] = '\0';
	if (output_dir && output_dir[0]) {
		rc = bridge_copy_path(g_pool.output_dir,
			sizeof(g_pool.output_dir), output_dir);
		if (rc != 0)
			MORPH_RETURN(rc);
	}
	return 0;
}

int fcgi_bridge_init(const char *db_path)
{
	const char *config_path;
	const char *output_dir;
	char *expanded_output = NULL;
	const char *log_file;
	char default_config[PATH_MAX];
	const char *home;
	int rc;

	if (!db_path || !db_path[0])
		MORPH_RETURN(-EINVAL);
	pthread_mutex_lock(&g_pool.lock);
	if (g_pool.slots) {
		pthread_mutex_unlock(&g_pool.lock);
		return 0;
	}
	pthread_mutex_unlock(&g_pool.lock);
	home = getenv("HOME");
	config_path = getenv("MORPH_FCGI_CONFIG");
	if (!config_path || !config_path[0]) {
		if (!home || !home[0])
			MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
		snprintf(default_config, sizeof(default_config),
			 "%s/.morph/config.toml", home);
		config_path = default_config;
	}
	output_dir = getenv("MORPH_FCGI_OUTPUT_DIR");
	if (output_dir && output_dir[0]) {
		expanded_output = file_expand_path(output_dir);
		if (!expanded_output)
			MORPH_RETURN(-ENOMEM);
		rc = file_ensure_dir(expanded_output);
		if (rc != 0) {
			free(expanded_output);
			MORPH_RETURN(rc);
		}
		output_dir = expanded_output;
	}
	log_file = getenv("MORPH_FCGI_LOG_FILE");
	if (!log_file || !log_file[0])
		log_file = "/tmp/morph-fastcgi.log";
	log_init(log_file, getenv("MORPH_DEBUG") ? LOG_DEBUG : LOG_INFO);
	rc = bridge_pool_configure(db_path, config_path, output_dir);
	free(expanded_output);
	if (rc != 0)
		MORPH_RETURN(rc);
	g_pool.slots = calloc((size_t)g_pool.max_workers,
			      sizeof(*g_pool.slots));
	if (!g_pool.slots)
		MORPH_RETURN(-ENOMEM);
	g_pool.slot_count = 0;
	g_pool.starting_workers = 0;
	g_pool.waiting_turns = 0;
	g_pool.outstanding_turns = 0;
	g_pool.stopping = 0;
	for (int i = 0; i < g_pool.min_workers; i++) {
		rc = bridge_runtime_open(i > 0, &g_pool.slots[i].runtime);
		if (rc != 0) {
			bridge_runtime_close_slots(g_pool.slots, i);
			free(g_pool.slots);
			g_pool.slots = NULL;
			fprintf(stderr, "fcgi-bridge: runtime worker %d "
				"initialization failed: %s\n", i,
				morph_strerror(rc));
			MORPH_RETURN(rc);
		}
		g_pool.slots[i].idle_since = time(NULL);
		g_pool.slot_count++;
	}
	rc = pthread_create(&g_pool.reaper_thread, NULL,
			    bridge_reaper_main, NULL);
	if (rc != 0) {
		bridge_runtime_close_slots(g_pool.slots, g_pool.slot_count);
		free(g_pool.slots);
		g_pool.slots = NULL;
		g_pool.slot_count = 0;
		MORPH_RETURN(-rc);
	}
	g_pool.reaper_started = 1;
	log_info("fastcgi elastic runtime pool ready: min=%d max=%d queue=%d",
		 g_pool.min_workers, g_pool.max_workers, g_pool.queue_max);
	return 0;
}

void fcgi_bridge_shutdown(void)
{
	struct bridge_runtime_slot *slots;
	int slot_count;
	int join_reaper;

	pthread_mutex_lock(&g_pool.lock);
	if (!g_pool.slots) {
		pthread_mutex_unlock(&g_pool.lock);
		return;
	}
	g_pool.stopping = 1;
	join_reaper = g_pool.reaper_started;
	g_pool.reaper_started = 0;
	pthread_cond_broadcast(&g_pool.changed);
	pthread_mutex_unlock(&g_pool.lock);
	if (join_reaper)
		pthread_join(g_pool.reaper_thread, NULL);
	pthread_mutex_lock(&g_pool.lock);
	while (g_pool.outstanding_turns > 0)
		pthread_cond_wait(&g_pool.changed, &g_pool.lock);
	slots = g_pool.slots;
	slot_count = g_pool.slot_count;
	g_pool.slots = NULL;
	g_pool.slot_count = 0;
	pthread_mutex_unlock(&g_pool.lock);
	bridge_runtime_close_slots(slots, slot_count);
	free(slots);
}

struct runtime *fcgi_bridge_runtime(void)
{
	struct runtime *runtime = NULL;

	pthread_mutex_lock(&g_pool.lock);
	if (g_pool.slots && g_pool.slot_count > 0)
		runtime = g_pool.slots[0].runtime;
	pthread_mutex_unlock(&g_pool.lock);
	return runtime;
}

int fcgi_bridge_runtime_acquire(const char *session_id,
				struct runtime **out)
{
	struct timespec deadline;
	int queued = 0;
	int deadline_set = 0;
	int rc;

	if (!session_id || !session_id[0] || !out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	pthread_mutex_lock(&g_pool.lock);
	for (;;) {
		struct runtime *created = NULL;
		int session_busy;
		int free_slot;

		if (g_pool.stopping || !g_pool.slots) {
			rc = -ESHUTDOWN;
			break;
		}
		free_slot = bridge_find_free_slot(session_id, &session_busy);
		if (!session_busy && free_slot >= 0) {
			struct bridge_runtime_slot *slot =
				&g_pool.slots[free_slot];

			slot->busy = 1;
			slot->idle_since = 0;
			snprintf(slot->session_id, sizeof(slot->session_id),
				 "%s", session_id);
			*out = slot->runtime;
			rc = 0;
			break;
		}
		if (!session_busy &&
		    g_pool.slot_count + g_pool.starting_workers <
			g_pool.max_workers) {
			g_pool.starting_workers++;
			pthread_mutex_unlock(&g_pool.lock);
			rc = bridge_runtime_open(1, &created);
			pthread_mutex_lock(&g_pool.lock);
			g_pool.starting_workers--;
			pthread_cond_broadcast(&g_pool.changed);
			if (rc != 0)
				break;
			if (g_pool.stopping || !g_pool.slots) {
				pthread_mutex_unlock(&g_pool.lock);
				runtime_close(created);
				pthread_mutex_lock(&g_pool.lock);
				rc = -ESHUTDOWN;
				break;
			}
			if (g_pool.slot_count >= g_pool.max_workers) {
				pthread_mutex_unlock(&g_pool.lock);
				runtime_close(created);
				pthread_mutex_lock(&g_pool.lock);
				continue;
			}
			g_pool.slots[g_pool.slot_count].runtime = created;
			g_pool.slots[g_pool.slot_count].elastic = 1;
			g_pool.slots[g_pool.slot_count].idle_since = time(NULL);
			g_pool.slot_count++;
			log_info("fastcgi runtime pool grew: workers=%d",
				 g_pool.slot_count);
			continue;
		}
		if (!queued) {
			if (g_pool.waiting_turns >= g_pool.queue_max) {
				rc = -EAGAIN;
				break;
			}
			g_pool.waiting_turns++;
			queued = 1;
		}
		if (!deadline_set) {
			bridge_deadline(&deadline, g_pool.wait_seconds);
			deadline_set = 1;
		}
		rc = pthread_cond_timedwait(&g_pool.changed, &g_pool.lock,
					    &deadline);
		if (rc == ETIMEDOUT) {
			rc = -ETIMEDOUT;
			break;
		}
	}
	if (queued)
		g_pool.waiting_turns--;
	pthread_mutex_unlock(&g_pool.lock);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

void fcgi_bridge_runtime_release(struct runtime *runtime)
{
	if (!runtime)
		return;
	pthread_mutex_lock(&g_pool.lock);
	for (int i = 0; i < g_pool.slot_count; i++) {
		if (g_pool.slots[i].runtime != runtime)
			continue;
		g_pool.slots[i].busy = 0;
		g_pool.slots[i].idle_since = time(NULL);
		g_pool.slots[i].session_id[0] = '\0';
		pthread_cond_broadcast(&g_pool.changed);
		break;
	}
	pthread_mutex_unlock(&g_pool.lock);
}

int fcgi_bridge_turn_begin(void)
{
	pthread_mutex_lock(&g_pool.lock);
	if (g_pool.stopping || !g_pool.slots) {
		pthread_mutex_unlock(&g_pool.lock);
		MORPH_RETURN(-ESHUTDOWN);
	}
	g_pool.outstanding_turns++;
	pthread_mutex_unlock(&g_pool.lock);
	return 0;
}

void fcgi_bridge_turn_end(void)
{
	pthread_mutex_lock(&g_pool.lock);
	if (g_pool.outstanding_turns > 0)
		g_pool.outstanding_turns--;
	if (g_pool.outstanding_turns == 0)
		pthread_cond_broadcast(&g_pool.changed);
	pthread_mutex_unlock(&g_pool.lock);
}

void fcgi_bridge_pool_status(struct fcgi_bridge_pool_status *status)
{
	if (!status)
		return;
	memset(status, 0, sizeof(*status));
	pthread_mutex_lock(&g_pool.lock);
	status->workers = g_pool.slot_count;
	status->min_workers = g_pool.min_workers;
	status->max_workers = g_pool.max_workers;
	status->starting_workers = g_pool.starting_workers;
	status->waiting_turns = g_pool.waiting_turns;
	status->queue_max = g_pool.queue_max;
	status->idle_seconds = g_pool.idle_seconds;
	for (int i = 0; i < g_pool.slot_count; i++)
		if (g_pool.slots[i].busy)
			status->busy_workers++;
	pthread_mutex_unlock(&g_pool.lock);
}

int fcgi_bridge_runtime_count(void)
{
	struct fcgi_bridge_pool_status status;

	fcgi_bridge_pool_status(&status);
	return status.workers;
}

const struct config *fcgi_bridge_config(void)
{
	struct runtime *runtime = fcgi_bridge_runtime();

	return runtime ? runtime_config_get(runtime) : NULL;
}

const char *fcgi_artifact_output_dir(void)
{
	const struct config *config = fcgi_bridge_config();
	const char *env = getenv("MORPH_FCGI_OUTPUT_DIR");

	if (env && env[0])
		return env;
	if (config && config->general.output_dir[0])
		return config->general.output_dir;
	return "/var/lib/morph/output";
}
