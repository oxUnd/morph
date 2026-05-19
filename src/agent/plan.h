#ifndef PLAN_H
#define PLAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stddef.h>

#define PLAN_NAME_MAX 64
#define PLAN_GOAL_MAX 512
#define PLAN_STEP_DESC_MAX 256
#define PLAN_MAX_STEPS 32
#define PLAN_MAX_PLANS 8
#define PLAN_STATUS_MAX 16

struct plan_step {
	int id;
	char description[PLAN_STEP_DESC_MAX];
	char status[PLAN_STATUS_MAX]; /* pending, in_progress, completed, failed, skipped */
};

struct plan {
	char name[PLAN_NAME_MAX];
	char goal[PLAN_GOAL_MAX];
	struct plan_step steps[PLAN_MAX_STEPS];
	int step_count;
	int active_step; /* index of current active step, -1 if none */
};

struct plan_registry {
	struct plan plans[PLAN_MAX_PLANS];
	int count;
};

void plan_registry_init(struct plan_registry *reg);

struct plan *plan_create(struct plan_registry *reg, const char *name,
			 const char *goal, const char **step_descs,
			 int step_count);

struct plan *plan_find(struct plan_registry *reg, const char *name);

int plan_update_step(struct plan_registry *reg, const char *plan_name,
		     int step_id, const char *status);

int plan_get_formatted(struct plan_registry *reg, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif
