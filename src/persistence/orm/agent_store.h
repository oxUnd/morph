#ifndef MORPH_ORM_AGENT_STORE_H
#define MORPH_ORM_AGENT_STORE_H

#include "persistence/orm/database.h"
#include <nb/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

struct store_agent_thread {
	int64_t root_session_id;
	char *agent_id;
	char *parent_agent_id;
	int64_t session_id;
	char *model;
	char *effort;
	int depth;
	char *description;
	int status;
	char *result;
	int error_code;
	int iterations;
	int64_t started_at;
	int64_t ended_at;
	char *assignment_json;
};

struct store_agent_budget {
	int64_t used;
	int64_t input_tokens;
	int64_t cached_tokens;
	int64_t output_tokens;
	int reminder_level;
};

struct store_agent_mail {
	int64_t id;
	char *sender;
	char *kind;
	char *body;
};

void agent_store_threads_cleanup(nb_vec *records);
void agent_store_mail_cleanup(nb_vec *records);
int agent_store_thread_put(struct db *db, const struct store_agent_thread *record);
int agent_store_threads_copy(struct db *db, int64_t session_id, nb_vec *records);
int agent_store_mail_send(struct db *db, int64_t root, const char *sender, const char *recipient,
			  const char *kind, const char *body);
int agent_store_finish_mail_clear(struct db *db, int64_t root, const char *recipient);
int agent_store_mail_pending(struct db *db, int64_t root, const char *recipient,
			     int new_tasks_only);
int agent_store_mail_copy(struct db *db, int64_t root, const char *recipient, nb_vec *records);
int agent_store_mail_ack(struct db *db, int64_t root, const char *recipient, int64_t cursor);
int agent_store_budget_copy(struct db *db, int64_t root, struct store_agent_budget *record);
int agent_store_budget_put(struct db *db, int64_t root, const struct store_agent_budget *record);
int agent_store_history_fork(struct db *db, int64_t source, int64_t destination);
int agent_store_event_put(struct db *db, int64_t root, const char *agent_id,
			  const char *event_json);
int agent_store_event_latest_copy(struct db *db, int64_t root, const char *agent_id, char **json);
int agent_store_events_copy(struct db *db, int64_t root, const char *agent_id, nb_vec *records);

#ifdef __cplusplus
}
#endif

#endif
