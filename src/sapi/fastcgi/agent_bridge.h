#ifndef MORPH_FCGI_AGENT_BRIDGE_H
#define MORPH_FCGI_AGENT_BRIDGE_H

struct config;
struct fcgi_bridge;
struct fcgi_turn_ticket;
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

int fcgi_bridge_init(struct fcgi_bridge **out, const char *db_path);
void fcgi_bridge_shutdown(struct fcgi_bridge *pool);
struct runtime *fcgi_bridge_runtime_borrow(struct fcgi_bridge *pool);
/* Creates an empty-slot ticket owner. The pool waits for all tickets on shutdown. */
int fcgi_bridge_turn_enqueue(struct fcgi_bridge *pool, const char *session_id,
			     struct fcgi_turn_ticket **out, int *position);
/* Borrows the ticket on every return path; destroy it after acquire returns.
 * Success returns a runtime lease, which keeps the pool alive until it is
 * released through fcgi_bridge_runtime_release(), never runtime_close(). */
int fcgi_bridge_turn_acquire(struct fcgi_bridge *pool,
			     struct fcgi_turn_ticket *ticket,
			     struct runtime **out);
/* Cancels a queued ticket without destroying its owner or a leased runtime. */
void fcgi_bridge_turn_cancel(struct fcgi_bridge *pool,
			     struct fcgi_turn_ticket *ticket);
void fcgi_turn_ticket_destroy(struct fcgi_turn_ticket *ticket);
void fcgi_turn_ticket_cleanup(struct fcgi_turn_ticket **owner);
int fcgi_bridge_runtime_acquire(struct fcgi_bridge *pool, const char *session_id,
				struct runtime **out);
void fcgi_bridge_runtime_release(struct fcgi_bridge *pool, struct runtime *runtime);
int fcgi_bridge_turn_begin(struct fcgi_bridge *pool);
void fcgi_bridge_turn_end(struct fcgi_bridge *pool);
int fcgi_bridge_runtime_count(struct fcgi_bridge *pool);
void fcgi_bridge_pool_status(struct fcgi_bridge *pool, struct fcgi_bridge_pool_status *status);
const struct config *fcgi_bridge_config_borrow(struct fcgi_bridge *pool);
const char *fcgi_artifact_output_dir_borrow(struct fcgi_bridge *pool);

#endif
