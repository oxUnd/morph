#ifndef MEMORY_H
#define MEMORY_H

struct morph_log;
#ifdef __cplusplus
extern "C" {
#endif

#include "db/database.h"
#include "agent/react.h"
#include "persistence/memory_store.h"
#include "persistence/preferences.h"

struct model;
struct memory_worker;

struct memory_options {
	struct morph_log *logger;
	/* Borrowed dependencies; the owner drains the worker before model teardown. */
	struct memory_worker *worker;
	struct model *llm;
	int enabled;
	int capture_input_enabled;
	int save_tasks_enabled;
	int learn_from_chats_enabled;
	int max_facts;
	int max_past_tasks;
	int max_tips;
	/* Unicode codepoint budget; complete records only, with recovery notices. */
	int max_reply_chars;
	/* Background writes require the memory version captured when the job was queued. */
	int check_version;
	int64_t memory_version;
	int64_t background_job_id;
	/* Durable second-stage retries reuse already committed learning records. */
	int organize_only;
	memory_session_visible_fn visible_fn;
	const char *visible_user_id;
	void *visible_user_data;
};

/* Each runtime owns an independent queue and worker. */
struct memory_worker *memory_worker_create(struct morph_log *logger);
void memory_worker_destroy(struct memory_worker *worker);

/* Prepare local memory and recover background work without interpreting input. */

int memory_accept_input(struct db *db, int64_t session_id, const char *input,
			const char *event_token, const struct memory_options *opts);

/* Creates owned context text in an empty output; no relevant memory yields NULL. */
int memory_reply_text_create(struct db *db, int64_t session_id, const char *query,
			     const struct memory_options *opts, char **out);

char *memory_session_text_create(struct db *db, int64_t session_id, int max_past_tasks);

char *memory_query_text_create(struct db *db, int64_t current_session_id,
			       const struct memory_query_options *opts);

int memory_save_turn(struct db *db, int64_t session_id, const char *user_input,
		     const char *assistant_output, const struct react_step *steps, int success,
		     const struct memory_options *opts);

/* Asynchronous variant: deep-copies all inputs and posts a job to a
 * background worker thread that owns its own SQLite connection. The
 * caller may free user_input/assistant_output/steps and continue to
 * interact with the foreground db immediately after this returns.
 *
 * Returns 0 on enqueue, negative errno on alloc / thread creation
 * failure (caller may then fall back to memory_save_turn). */
int memory_save_turn_async(struct db *db, int64_t session_id, const char *user_input,
			   const char *assistant_output, const struct react_step *steps,
			   int success, const struct memory_options *opts);

/* Drain the async queue and join the worker thread. CLI must call this
 * before db_close() so any in-flight job finishes against a still-open
 * database file. Safe to call when the worker was never started. */
void memory_async_shutdown(struct memory_worker *worker);
/* Recover queued/crashed advisory jobs; preferences are already committed. */
int memory_async_resume(struct db *db, const struct memory_options *opts);
int memory_retry_updates(struct db *db, int64_t session_id, const struct memory_options *opts);
char *memory_background_text_create(struct db *db, int64_t session_id);

struct cJSON;
int memory_report_create(struct db *db, int64_t session_id, const struct memory_options *options,
			 const char *query, struct cJSON **out);

/* Return non-zero when background memory saving has queued or in-flight
 * work that shutdown may need to wait for. */
int memory_async_pending(struct memory_worker *worker);

int memory_clear(struct db *db, int64_t session_id, enum memory_clear_scope scope);

#ifdef __cplusplus
}
#endif

#endif
