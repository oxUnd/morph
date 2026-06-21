#ifndef SCHEDULED_TASK_H
#define SCHEDULED_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "database.h"
#include <stdint.h>

#define SCHEDULED_TASK_TEXT_MAX 256
#define SCHEDULED_TASK_TYPE_MAX 32

struct scheduled_task_input {
	const char *title;
	const char *kind;
	const char *trigger_type;
	int64_t next_run_at;
	int interval_seconds;
	int64_t timeout_at;
	int max_attempts;
	const char *action_type;
	const char *payload_json;
	const char *policy_json;
	const char *notify_json;
};

struct scheduled_task {
	int64_t id;
	char title[SCHEDULED_TASK_TEXT_MAX];
	char kind[SCHEDULED_TASK_TYPE_MAX];
	char status[SCHEDULED_TASK_TYPE_MAX];
	char trigger_type[SCHEDULED_TASK_TYPE_MAX];
	int64_t next_run_at;
	int interval_seconds;
	int64_t timeout_at;
	int attempts;
	int max_attempts;
	char action_type[SCHEDULED_TASK_TYPE_MAX];
	char *payload_json;
	char *policy_json;
	char *notify_json;
	char last_error[SCHEDULED_TASK_TEXT_MAX];
	int64_t created_at;
	int64_t updated_at;
};

struct notification {
	int64_t id;
	int64_t task_id;
	char level[SCHEDULED_TASK_TYPE_MAX];
	char title[SCHEDULED_TASK_TEXT_MAX];
	char *body;
	int64_t created_at;
	int64_t read_at;
	char delivery_status[SCHEDULED_TASK_TYPE_MAX];
};

int scheduled_task_create(struct db *db,
			  const struct scheduled_task_input *input,
			  struct scheduled_task *out);
int scheduled_task_get(struct db *db, int64_t id, struct scheduled_task *out);
int scheduled_task_list(struct db *db, const char *status, int limit,
			struct scheduled_task **out, int *count);
int scheduled_task_list_due(struct db *db, int64_t now, int limit,
			    struct scheduled_task **out, int *count);
int scheduled_task_update_run(struct db *db, int64_t id, const char *status,
			      int64_t next_run_at, int attempts,
			      const char *last_error);
int scheduled_task_cancel(struct db *db, int64_t id);
int scheduled_task_run_due(struct db *db, int64_t now, int limit, int *ran);
int scheduled_task_run_due_collect(struct db *db, int64_t now, int limit,
				   struct notification **notifications,
				   int *count);
void scheduled_task_cleanup(struct scheduled_task *task);
void scheduled_task_free_list(struct scheduled_task *tasks, int count);

int notification_create(struct db *db, int64_t task_id, const char *level,
			const char *title, const char *body,
			const char *delivery_status,
			struct notification *out);
int notification_list_unread(struct db *db, int limit,
			     struct notification **out, int *count);
int notification_mark_read(struct db *db, int64_t id, int64_t read_at);
void notification_cleanup(struct notification *notification);
void notification_free_list(struct notification *notifications, int count);

#ifdef __cplusplus
}
#endif

#endif
