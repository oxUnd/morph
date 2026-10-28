#include "plan.h"
#include "util/id.h"
#include "util/error.h"
#include "util/utf8.h"
#include "db/database.h"
#include <stdlib.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

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

struct plan *plan_find_by_id(struct plan_registry *reg, const char *id)
{
	if (!reg || !id)
		return NULL;
	for (int i = 0; i < reg->count; i++) {
		if (strcmp(reg->plans[i].id, id) == 0)
			return &reg->plans[i];
	}
	return NULL;
}

static int plan_generate_id(struct plan_registry *reg, char *id, size_t size)
{
	int rc;

	if (!reg || !id || size < PLAN_ID_MAX)
		return -EINVAL;
	for (int tries = 0; tries < 16; tries++) {
		rc = morph_random_id("pln_", id, size);
		if (rc < 0)
			return rc;
		if (!plan_find_by_id(reg, id))
			return 0;
	}
	return -EEXIST;
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
	if (plan_generate_id(reg, p->id, sizeof(p->id)) != 0)
		return NULL;
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

static int plan_update_step_ptr(struct plan *p, int step_id,
				const char *status)
{
	if (!p || !status)
		return -EINVAL;

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

int plan_update_step(struct plan_registry *reg, const char *plan_name,
		     int step_id, const char *status)
{
	struct plan *p;

	if (!reg || !plan_name || !status)
		return -EINVAL;
	p = plan_find_by_name(reg, plan_name);
	if (!p)
		return -ENOENT;
	return plan_update_step_ptr(p, step_id, status);
}

int plan_update_step_by_id(struct plan_registry *reg, const char *plan_id,
			   int step_id, const char *status)
{
	struct plan *p;

	if (!reg || !plan_id || !status)
		return -EINVAL;
	p = plan_find_by_id(reg, plan_id);
	if (!p)
		return -ENOENT;
	return plan_update_step_ptr(p, step_id, status);
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
	int rc = 0;

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

		if ((rc = append_str(buf, buf_size, &pos,
			"%sPlan \"%s\" [%s]", pos > 0 ? "\n" : "",
			p->name, p->id)) < 0)
			goto out;
		if (p->goal[0]) {
			if ((rc = append_str(buf, buf_size, &pos,
				"\n  Goal: %s", p->goal)) < 0)
				goto out;
		}
		if ((rc = append_str(buf, buf_size, &pos,
			"\n  %d step(s)", p->step_count)) < 0)
			goto out;
		if (all_done) {
			if ((rc = append_str(buf, buf_size, &pos,
				" [all completed]")) < 0)
				goto out;
		}

		for (int j = 0; j < p->step_count; j++) {
			struct plan_step *s = &p->steps[j];
			const char *icon = status_icon(s->status);
			const char *marker = (j == p->active_step && p->active_step >= 0)
					   ? " <-- active" : "";

			if ((rc = append_str(buf, buf_size, &pos,
				"\n  [%s] %d. %s%s",
				icon, s->id, s->description, marker)) < 0)
				goto out;
		}
	}

out:
	if (pos == 0 && reg->count == 0 && rc == 0) {
		snprintf(buf, buf_size,
			"No plans yet. Use plan create to start one.");
	}
	return rc;
}

/* A checkpoint is independent of model history and survives compaction. */
cJSON *plan_registry_json(const struct plan_registry *reg)
{
	cJSON *root = cJSON_CreateArray();
	if (!root)
		return NULL;
	for (int i = 0; i < reg->count; i++) {
		const struct plan *p = &reg->plans[i];
		cJSON *obj = cJSON_CreateObject();
		cJSON *steps = cJSON_CreateArray();
		if (!obj || !steps) {
			cJSON_Delete(obj);
			cJSON_Delete(steps);
			cJSON_Delete(root);
			return NULL;
		}
		cJSON_AddItemToArray(root, obj);
		cJSON_AddItemToObject(obj, "steps", steps);
#define PLAN_WRITE(field)                                                                          \
	do {                                                                                       \
		if (!cJSON_AddStringToObject(obj, #field, p->field)) {                             \
			cJSON_Delete(root);                                                        \
			return NULL;                                                               \
		}                                                                                  \
	} while (0)
		PLAN_WRITE(id);
		PLAN_WRITE(name);
		PLAN_WRITE(goal);
		PLAN_WRITE(request);
		PLAN_WRITE(constraints);
		PLAN_WRITE(acceptance);
		PLAN_WRITE(rejected);
		PLAN_WRITE(verification);
#undef PLAN_WRITE
		for (int j = 0; j < p->step_count; j++) {
			const struct plan_step *step = &p->steps[j];
			cJSON *entry = cJSON_CreateObject();
			if (!entry) {
				cJSON_Delete(root);
				return NULL;
			}
			cJSON_AddItemToArray(steps, entry);
#define STEP_WRITE(field)                                                                          \
	do {                                                                                       \
		if (!cJSON_AddStringToObject(entry, #field, step->field)) {                        \
			cJSON_Delete(root);                                                        \
			return NULL;                                                               \
		}                                                                                  \
	} while (0)
			STEP_WRITE(description);
			STEP_WRITE(status);
			STEP_WRITE(evidence);
			STEP_WRITE(blocker);
			STEP_WRITE(artifact);
#undef STEP_WRITE
		}
	}
	return root;
}

static int plan_restore_text(const cJSON *obj, const char *key, char *dst, size_t cap)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!value)
		return 0;
	if (!cJSON_IsString(value) || strlen(value->valuestring) >= cap ||
	    utf8valid(value->valuestring))
		MORPH_RETURN(MORPH_ERR_FORMAT);
	memcpy(dst, value->valuestring, strlen(value->valuestring) + 1);
	return 0;
}

int plan_registry_restore(struct plan_registry *reg, const cJSON *json)
{
	struct plan_registry *copy;
	int rc = 0;
	if (!reg || !cJSON_IsArray(json) || cJSON_GetArraySize(json) > PLAN_MAX_PLANS)
		MORPH_RETURN(MORPH_ERR_FORMAT);
	copy = calloc(1, sizeof(*copy));
	if (!copy)
		MORPH_RETURN(-ENOMEM);
	copy->db = reg->db;
	copy->session_id = reg->session_id;
	copy->count = cJSON_GetArraySize(json);
	for (int i = 0; i < copy->count && rc == 0; i++) {
		struct plan *p = &copy->plans[i];
		const cJSON *obj = cJSON_GetArrayItem(json, i);
		const cJSON *steps = cJSON_GetObjectItemCaseSensitive(obj, "steps");
#define PLAN_READ(field)                                                                           \
	do {                                                                                       \
		if (rc == 0)                                                                       \
			rc = plan_restore_text(obj, #field, p->field, sizeof(p->field));           \
	} while (0)
		PLAN_READ(id);
		PLAN_READ(name);
		PLAN_READ(goal);
		PLAN_READ(request);
		PLAN_READ(constraints);
		PLAN_READ(acceptance);
		PLAN_READ(rejected);
		PLAN_READ(verification);
#undef PLAN_READ
		if (!p->id[0] || !p->name[0] || !cJSON_IsArray(steps) ||
		    cJSON_GetArraySize(steps) > PLAN_MAX_STEPS) {
			rc = MORPH_ERR_FORMAT;
			break;
		}
		p->step_count = cJSON_GetArraySize(steps);
		p->active_step = -1;
		for (int j = 0; j < p->step_count && rc == 0; j++) {
			struct plan_step *step = &p->steps[j];
			const cJSON *entry = cJSON_GetArrayItem(steps, j);
			step->id = j + 1;
#define STEP_READ(field)                                                                           \
	do {                                                                                       \
		if (rc == 0)                                                                       \
			rc = plan_restore_text(entry, #field, step->field, sizeof(step->field));   \
	} while (0)
			STEP_READ(description);
			STEP_READ(status);
			STEP_READ(evidence);
			STEP_READ(blocker);
			STEP_READ(artifact);
#undef STEP_READ
			if (!strcmp(step->status, "in_progress"))
				p->active_step = j;
			else if (strcmp(step->status, "pending") &&
				 strcmp(step->status, "completed") &&
				 strcmp(step->status, "failed") && strcmp(step->status, "skipped"))
				rc = MORPH_ERR_FORMAT;
		}
	}
	if (rc == 0)
		*reg = *copy;
	free(copy);
	MORPH_RETURN(rc);
}

int plan_registry_save(struct plan_registry *reg)
{
	cJSON *json;
	char *text;
	sqlite3_stmt *stmt = NULL;
	int rc;
	if (!reg || !reg->db || reg->session_id <= 0)
		return 0;
	json = plan_registry_json(reg);
	text = json ? cJSON_PrintUnformatted(json) : NULL;
	cJSON_Delete(json);
	if (!text)
		MORPH_RETURN(-ENOMEM);
	rc = sqlite3_prepare_v2(
	    reg->db->handle,
	    "INSERT INTO session_plans(session_id,state_json) VALUES(?,?) "
	    "ON CONFLICT(session_id) DO UPDATE SET state_json=excluded.state_json",
	    -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, reg->session_id);
		sqlite3_bind_text(stmt, 2, text, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	free(text);
	MORPH_RETURN(rc == SQLITE_DONE ? 0 : MORPH_ERR_DB);
}

int plan_registry_bind(struct plan_registry *reg, struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	int rc;
	if (!reg || !db || session_id <= 0)
		MORPH_RETURN(-EINVAL);
	if (reg->db == db && reg->session_id == session_id)
		return 0;
	rc = sqlite3_prepare_v2(
	    db->handle, "SELECT state_json FROM session_plans WHERE session_id=?", -1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		cJSON *json = cJSON_Parse((const char *)sqlite3_column_text(stmt, 0));
		rc = plan_registry_restore(reg, json);
		cJSON_Delete(json);
	} else if (rc == SQLITE_DONE) {
		plan_registry_init(reg);
		rc = 0;
	} else
		rc = MORPH_ERR_DB;
	sqlite3_finalize(stmt);
	if (rc == 0) {
		reg->db = db;
		reg->session_id = session_id;
	}
	MORPH_RETURN(rc);
}
