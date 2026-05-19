#include "plan.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void plan_registry_init(struct plan_registry *reg)
{
	if (!reg)
		return;
	memset(reg, 0, sizeof(*reg));
}

static struct plan *plan_find_by_name(struct plan_registry *reg, const char *name)
{
	if (!reg || !name)
		return NULL;
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->plans[i].name, name) == 0)
			return &reg->plans[i];
	}
	return NULL;
}

static int append_str(char *buf, size_t buf_size, size_t *pos, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static int append_str(char *buf, size_t buf_size, size_t *pos, const char *fmt, ...)
{
	if (*pos >= buf_size - 1)
		return -ENOSPC;

	va_list args;
	va_start(args, fmt);
	int rc = vsnprintf(buf + *pos, buf_size - *pos, fmt, args);
	va_end(args);

	if (rc < 0)
		return -EINVAL;
	if ((size_t)rc >= buf_size - *pos) {
		*pos = buf_size - 1;
		buf[*pos] = '\0';
		return -ENOSPC;
	}
	*pos += (size_t)rc;
	return 0;
}

struct plan *plan_create(struct plan_registry *reg, const char *name,
			 const char *goal, const char **step_descs,
			 int step_count)
{
	if (!reg || !name || !step_descs || step_count <= 0)
		return NULL;
	if (reg->count >= PLAN_MAX_PLANS)
		return NULL;
	if (step_count > PLAN_MAX_STEPS)
		step_count = PLAN_MAX_STEPS;

	struct plan *p = &reg->plans[reg->count];
	memset(p, 0, sizeof(*p));
	strncpy(p->name, name, sizeof(p->name) - 1);
	if (goal)
		strncpy(p->goal, goal, sizeof(p->goal) - 1);
	p->active_step = 0;
	p->step_count = step_count;

	for (int i = 0; i < step_count; i++) {
		p->steps[i].id = i + 1;
		if (step_descs[i])
			strncpy(p->steps[i].description, step_descs[i],
				sizeof(p->steps[i].description) - 1);
		if (i == 0)
			strncpy(p->steps[i].status, "in_progress",
				sizeof(p->steps[i].status) - 1);
		else
			strncpy(p->steps[i].status, "pending",
				sizeof(p->steps[i].status) - 1);
	}

	reg->count++;
	return p;
}

struct plan *plan_find(struct plan_registry *reg, const char *name)
{
	return plan_find_by_name(reg, name);
}

int plan_update_step(struct plan_registry *reg, const char *plan_name,
		     int step_id, const char *status)
{
	if (!reg || !plan_name || !status)
		return -EINVAL;

	struct plan *p = plan_find_by_name(reg, plan_name);
	if (!p)
		return -ENOENT;

	int idx = -1;
	for (int i = 0; i < p->step_count; i++) {
		if (p->steps[i].id == step_id) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		return -ENOENT;

	strncpy(p->steps[idx].status, status, sizeof(p->steps[idx].status) - 1);

	if (strcmp(status, "completed") == 0 ||
	    strcmp(status, "failed") == 0 ||
	    strcmp(status, "skipped") == 0) {
		if (p->active_step == idx) {
			int next = -1;
			for (int i = idx + 1; i < p->step_count; i++) {
				if (strcmp(p->steps[i].status, "pending") == 0) {
					next = i;
					break;
				}
			}
			if (next >= 0) {
				p->active_step = next;
				strncpy(p->steps[next].status, "in_progress",
					sizeof(p->steps[next].status) - 1);
			} else {
				p->active_step = -1;
			}
		}
	} else if (strcmp(status, "in_progress") == 0) {
		p->active_step = idx;
	}

	return 0;
}

static const char *status_icon(const char *status)
{
	if (!status)
		return " ";
	if (strcmp(status, "completed") == 0)
		return "x";
	if (strcmp(status, "in_progress") == 0)
		return ">";
	if (strcmp(status, "failed") == 0)
		return "!";
	if (strcmp(status, "skipped") == 0)
		return "-";
	return " ";
}

int plan_get_formatted(struct plan_registry *reg, char *buf, size_t buf_size)
{
	if (!reg || !buf || buf_size == 0)
		return -EINVAL;

	size_t pos = 0;

	for (int i = 0; i < reg->count; i++) {
		struct plan *p = &reg->plans[i];

		int all_done = 1;
		for (int j = 0; j < p->step_count; j++) {
			if (strcmp(p->steps[j].status, "completed") != 0 &&
			    strcmp(p->steps[j].status, "failed") != 0 &&
			    strcmp(p->steps[j].status, "skipped") != 0) {
				all_done = 0;
				break;
			}
		}

		if (append_str(buf, buf_size, &pos,
			"%sPlan \"%s\"", pos > 0 ? "\n" : "", p->name) < 0)
			goto out;
		if (p->goal[0]) {
			if (append_str(buf, buf_size, &pos,
				"\n  Goal: %s", p->goal) < 0)
				goto out;
		}
		if (append_str(buf, buf_size, &pos,
			"\n  %d step(s)", p->step_count) < 0)
			goto out;
		if (all_done) {
			if (append_str(buf, buf_size, &pos,
				" [all completed]") < 0)
				goto out;
		}

		for (int j = 0; j < p->step_count; j++) {
			struct plan_step *s = &p->steps[j];
			const char *icon = status_icon(s->status);
			const char *marker = (j == p->active_step && p->active_step >= 0)
					   ? " <-- active" : "";

			if (append_str(buf, buf_size, &pos,
				"\n  [%s] %d. %s%s",
				icon, s->id, s->description, marker) < 0)
				goto out;
		}
	}

out:
	if (pos == 0 && reg->count == 0) {
		snprintf(buf, buf_size,
			"No plans yet. Use plan create to start one.");
	}
	return 0;
}
