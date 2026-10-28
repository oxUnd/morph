#ifndef PLAN_H
#define PLAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

struct db;

#define PLAN_NAME_MAX 64
#define PLAN_ID_MAX 40
#define PLAN_GOAL_MAX 512
#define PLAN_STEP_DESC_MAX 256
#define PLAN_MAX_STEPS 32
#define PLAN_MAX_PLANS 8
#define PLAN_STATUS_MAX 16

struct plan_step {
	int id;
	char description[PLAN_STEP_DESC_MAX];
	char evidence[512];
	char blocker[256];
	char artifact[512];
	char status[PLAN_STATUS_MAX]; /* pending, in_progress, completed, failed, skipped */
};

struct plan {
	char id[PLAN_ID_MAX];
	char name[PLAN_NAME_MAX];
	char goal[PLAN_GOAL_MAX];
	char request[2048];
	char constraints[2048];
	char acceptance[2048];
	char rejected[1024];
	char verification[8192];
	struct plan_step steps[PLAN_MAX_STEPS];
	int step_count;
	int active_step; /* index of current active step, -1 if none */
};

struct plan_registry {
	struct db *db;
	int64_t session_id;
	struct plan plans[PLAN_MAX_PLANS];
	int count;
};

int plan_registry_bind(struct plan_registry *reg, struct db *db, int64_t session_id);
int plan_registry_save(struct plan_registry *reg);
cJSON *plan_registry_json(const struct plan_registry *reg);
int plan_registry_restore(struct plan_registry *reg, const cJSON *json);

void plan_registry_init(struct plan_registry *reg);

struct plan *plan_create(struct plan_registry *reg, const char *name,
			 const char *goal, const char **step_descs,
			 int step_count);

struct plan *plan_find(struct plan_registry *reg, const char *name);
struct plan *plan_find_by_id(struct plan_registry *reg, const char *id);

int plan_update_step(struct plan_registry *reg, const char *plan_name,
		     int step_id, const char *status);
int plan_update_step_by_id(struct plan_registry *reg, const char *plan_id,
			   int step_id, const char *status);

int plan_get_formatted(struct plan_registry *reg, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif
