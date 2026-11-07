#ifndef MORPH_FCGI_SERVICES_TURN_JOB_H
#define MORPH_FCGI_SERVICES_TURN_JOB_H

struct fcgi_bridge;
struct session_store;

enum fcgi_turn_failure {
	FCGI_TURN_FAILURE_NONE,
	FCGI_TURN_FAILURE_INTERNAL,
	FCGI_TURN_FAILURE_DAILY_QUOTA,
	FCGI_TURN_FAILURE_CONCURRENCY_QUOTA,
	FCGI_TURN_FAILURE_QUEUE_FULL,
	FCGI_TURN_FAILURE_SHUTDOWN,
	FCGI_TURN_FAILURE_THREAD
};

struct fcgi_turn_receipt {
	enum fcgi_turn_failure failure;
	char turn_id[64];
	int queue_position;
};

/* Copies the receipt before starting its worker; no caller borrows a live job.
 * Failure identifies the rejection stage without conflating shared errno values. */
int fcgi_turn_job_submit(struct fcgi_bridge *bridge, struct session_store *store,
			 const char *session_id, const char *user_id, const char *input,
			 struct fcgi_turn_receipt *receipt);

#endif
