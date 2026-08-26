#ifndef MORPH_FCGI_AGENT_BRIDGE_H
#define MORPH_FCGI_AGENT_BRIDGE_H

struct config;
struct runtime;

struct fcgi_bridge_pool_status {
	int workers;
	int busy_workers;
	int min_workers;
	int max_workers;
	int starting_workers;
	int waiting_turns;
	int queue_max;
	int idle_seconds;
};

int fcgi_bridge_init(const char *db_path);
void fcgi_bridge_shutdown(void);
struct runtime *fcgi_bridge_runtime(void);
int fcgi_bridge_runtime_acquire(const char *session_id,
				struct runtime **out);
void fcgi_bridge_runtime_release(struct runtime *runtime);
int fcgi_bridge_turn_begin(void);
void fcgi_bridge_turn_end(void);
int fcgi_bridge_runtime_count(void);
void fcgi_bridge_pool_status(struct fcgi_bridge_pool_status *status);
const struct config *fcgi_bridge_config(void);
const char *fcgi_artifact_output_dir(void);

#endif
