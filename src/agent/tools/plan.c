#include "plan.h"
#include "agent/tool.h"
#include "agent/plan.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/arena.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "util/error.h"

static struct plan_registry *g_plans;
static struct model *g_llm;

static char *format_plan(struct plan *p)
{
	if (!p)
		return strdup("(no plan)");

	char buf[4096];
	size_t pos = 0;

	int all_done = 1;
	for (int j = 0; j < p->step_count; j++) {
		if (strcmp(p->steps[j].status, "completed") != 0 &&
		    strcmp(p->steps[j].status, "failed") != 0 &&
		    strcmp(p->steps[j].status, "skipped") != 0) {
			all_done = 0;
			break;
		}
	}

	int rc = snprintf(buf + pos, sizeof(buf) - pos,
		"Plan \"%s\"", p->name);
	if (rc > 0 && (size_t)rc < sizeof(buf) - pos)
		pos += (size_t)rc;

	if (p->goal[0]) {
		rc = snprintf(buf + pos, sizeof(buf) - pos,
			"\n  Goal: %s", p->goal);
		if (rc > 0 && (size_t)rc < sizeof(buf) - pos)
			pos += (size_t)rc;
	}

	rc = snprintf(buf + pos, sizeof(buf) - pos,
		"\n  %d step(s)", p->step_count);
	if (rc > 0 && (size_t)rc < sizeof(buf) - pos)
		pos += (size_t)rc;
	if (all_done) {
		rc = snprintf(buf + pos, sizeof(buf) - pos, " [completed]");
		if (rc > 0 && (size_t)rc < sizeof(buf) - pos)
			pos += (size_t)rc;
	}

	for (int i = 0; i < p->step_count; i++) {
		const char *icon;
		if (strcmp(p->steps[i].status, "completed") == 0)
			icon = "x";
		else if (strcmp(p->steps[i].status, "in_progress") == 0)
			icon = ">";
		else if (strcmp(p->steps[i].status, "failed") == 0)
			icon = "!";
		else if (strcmp(p->steps[i].status, "skipped") == 0)
			icon = "-";
		else
			icon = " ";

		const char *marker = (i == p->active_step && p->active_step >= 0)
				   ? " <-- active" : "";

		rc = snprintf(buf + pos, sizeof(buf) - pos,
			"\n  [%s] %d. %s%s",
			icon, p->steps[i].id,
			p->steps[i].description, marker);
		if (rc > 0 && (size_t)rc < sizeof(buf) - pos)
			pos += (size_t)rc;
		else
			break;
	}

	return strdup(buf);
}

struct decompose_ctx {
	char *result;
	size_t len;
	size_t cap;
};

static int decompose_stream_cb(const char *token, void *user_data)
{
	struct decompose_ctx *ctx = user_data;
	size_t tlen = strlen(token);
	if (ctx->len + tlen + 1 >= ctx->cap) {
		ctx->cap = (ctx->len + tlen + 1) * 2 + 4096;
		char *new_r = realloc(ctx->result, ctx->cap);
		if (!new_r)
			return -ENOMEM;
		ctx->result = new_r;
	}
	memcpy(ctx->result + ctx->len, token, tlen);
	ctx->len += tlen;
	ctx->result[ctx->len] = '\0';
	return 0;
}

static int parse_steps_from_text(const char *text, const char **descs,
				 int max_steps)
{
	if (!text || !descs || max_steps <= 0)
		return 0;

	int count = 0;
	const char *p = text;

	while (*p && count < max_steps) {
		/* skip empty lines */
		while (*p == '\n' || *p == '\r' || *p == ' ')
			p++;

		if (!*p)
			break;

		/* Look for "N." or "N)" or "- " pattern */
		const char *line_start = p;

		/* skip number */
		while (*p >= '0' && *p <= '9')
			p++;
		/* skip ". " or ") " or ".  " */
		if (*p == '.' || *p == ')')
			p++;
		while (*p == ' ' || *p == '\t')
			p++;
		/* also handle "- " bullet */
		if (line_start == p && *p == '-' && *(p + 1) == ' ') {
			p += 2;
		}
		/* If we didn't find a number or bullet, skip this line */
		if (p == line_start) {
			while (*p && *p != '\n')
				p++;
			continue;
		}

		/* extract step text */
		const char *step_start = p;
		while (*p && *p != '\n')
			p++;

		size_t slen = (size_t)(p - step_start);
		while (slen > 0 && (step_start[slen - 1] == ' ' ||
				   step_start[slen - 1] == '\r'))
			slen--;

		if (slen > 0) {
			char *dup = malloc(slen + 1);
			if (dup) {
				memcpy(dup, step_start, slen);
				dup[slen] = '\0';
				descs[count] = dup;
				count++;
			}
		}
	}

	return count;
}

static int auto_decompose(const char *goal, const char **step_descs,
			  int max_steps)
{
	if (!goal || !step_descs || max_steps <= 0)
		return -EINVAL;
	if (!g_llm || !g_llm->chat || !g_llm->api_key[0])
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);

	char prompt[2048];
	snprintf(prompt, sizeof(prompt),
		"Break down the following goal into a step-by-step plan. "
		"Return ONLY a numbered list of %d-%d specific, actionable steps. "
		"Each step should start with a number followed by a period. "
		"Be concise.\n\nGoal: %s",
		2, max_steps, goal);

	struct arena *arena = arena_create(64 * 1024);
	if (!arena)
		return -ENOMEM;

	const char *sys = "You are a task planner. Break goals into numbered steps. "
			  "Output ONLY the numbered list, no preamble.";

	struct decompose_ctx ctx = {
		.result = malloc(8192),
		.len = 0,
		.cap = 8192,
	};
	if (!ctx.result) {
		arena_destroy(arena);
		return -ENOMEM;
	}
	ctx.result[0] = '\0';

	const char *messages[] = { prompt };
	int status = g_llm->chat(g_llm, arena, sys, messages, 1,
				 decompose_stream_cb, &ctx);
	arena_destroy(arena);

	if (status < 0 || !ctx.result[0]) {
		free(ctx.result);
		MORPH_RETURN(MORPH_ERR_LLM);
	}

	int count = parse_steps_from_text(ctx.result, step_descs, max_steps);
	free(ctx.result);
	return count;
}

static void free_step_descs(const char **descs, int count)
{
	for (int i = 0; i < count; i++)
		free((void *)descs[i]);
}

static int plan_tool_exec(const char *args_json, char **result_json,
			  void *user_data)
{
	(void)user_data;
	if (!result_json)
		return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		*result_json = strdup("{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}

	cJSON *cmd = cJSON_GetObjectItem(root, "command");
	if (!cJSON_IsString(cmd)) {
		cJSON_Delete(root);
		*result_json = strdup(
			"{\"error\":\"missing 'command' parameter. "
			"Commands: create, update, get, list\"}");
		return -EINVAL;
	}

	const char *command = cmd->valuestring;
	char out_buf[8192];
	int rc = 0;

	if (strcmp(command, "create") == 0) {
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *goal_item = cJSON_GetObjectItem(root, "goal");
		cJSON *steps_item = cJSON_GetObjectItem(root, "steps");

		if (!cJSON_IsString(name_item) || !name_item->valuestring) {
			cJSON_Delete(root);
			*result_json = strdup(
				"{\"error\":\"create requires 'name' (string)\"}");
			return -EINVAL;
		}

		const char *goal = cJSON_IsString(goal_item)
				  ? goal_item->valuestring : "";

		int step_count = 0;
		const char *step_descs[PLAN_MAX_STEPS];
		memset(step_descs, 0, sizeof(step_descs));
		int auto_decomposed = 0;

		if (cJSON_IsArray(steps_item) && cJSON_GetArraySize(steps_item) > 0) {
			step_count = cJSON_GetArraySize(steps_item);
			if (step_count > PLAN_MAX_STEPS)
				step_count = PLAN_MAX_STEPS;
			for (int i = 0; i < step_count; i++) {
				cJSON *s = cJSON_GetArrayItem(steps_item, i);
				step_descs[i] = cJSON_IsString(s)
						? s->valuestring : "";
			}
		} else if (goal && goal[0]) {
			/* Auto-decompose goal into steps using LLM */
			const char *tmp_descs[PLAN_MAX_STEPS];
			memset(tmp_descs, 0, sizeof(tmp_descs));
			rc = auto_decompose(goal, tmp_descs, PLAN_MAX_STEPS);
			if (rc < 0) {
				free_step_descs(tmp_descs, PLAN_MAX_STEPS);
				cJSON_Delete(root);
				*result_json = strdup(
					"{\"error\":\"auto-decompose failed\"}");
				MORPH_RETURN(rc);
			}
			if (rc > 0) {
				step_count = rc;
				for (int i = 0; i < step_count; i++)
					step_descs[i] = tmp_descs[i];
				auto_decomposed = 1;
			}
		}

		if (step_count == 0) {
			cJSON_Delete(root);
			*result_json = strdup(
				"{\"error\":\"create requires 'steps' (array of strings) "
				"or a 'goal' string to auto-decompose\"}");
			return -EINVAL;
		}

		struct plan *p = plan_create(g_plans, name_item->valuestring,
			goal, step_descs, step_count);

		if (!p) {
			if (auto_decomposed)
				free_step_descs(step_descs, step_count);
			cJSON_Delete(root);
			*result_json = strdup(
				"{\"error\":\"failed to create plan "
				"(max 8 plans, max 32 steps each)\"}");
			return -ENOSPC;
		}

		char *formatted = format_plan(p);
		if (!formatted) {
			if (auto_decomposed)
				free_step_descs(step_descs, step_count);
			cJSON_Delete(root);
			*result_json = strdup("{\"error\":\"out of memory\"}");
			return -ENOMEM;
		}

		if (auto_decomposed) {
			snprintf(out_buf, sizeof(out_buf),
				"Plan created (auto-decomposed from goal).\n%s",
				formatted);
			free_step_descs(step_descs, step_count);
		} else {
			snprintf(out_buf, sizeof(out_buf),
				"Plan created.\n%s", formatted);
		}
		*result_json = strdup(out_buf);
		free(formatted);
		log_info("plan: created '%s' with %d steps%s",
			 p->name, p->step_count,
			 auto_decomposed ? " (auto)" : "");

	} else if (strcmp(command, "update") == 0) {
		cJSON *plan_item = cJSON_GetObjectItem(root, "plan");
		cJSON *step_id_item = cJSON_GetObjectItem(root, "step_id");
		cJSON *status_item = cJSON_GetObjectItem(root, "status");

		const char *plan_name = cJSON_IsString(plan_item)
				       ? plan_item->valuestring : NULL;
		int step_id = cJSON_IsNumber(step_id_item)
			     ? (int)step_id_item->valuedouble : -1;
		const char *status = cJSON_IsString(status_item)
				    ? status_item->valuestring : NULL;

		if (!plan_name || step_id < 0 || !status) {
			cJSON_Delete(root);
			*result_json = strdup(
				"{\"error\":\"update requires 'plan' (string), "
				"'step_id' (int), and 'status' (string). "
				"Status: pending, in_progress, completed, "
				"failed, skipped\"}");
			return -EINVAL;
		}

		rc = plan_update_step(g_plans, plan_name, step_id, status);
		if (rc < 0) {
			char err[256];
			const char *msg;
			if (rc == -ENOENT)
				msg = "plan or step not found";
			else
				msg = "invalid parameters";
			snprintf(err, sizeof(err),
				"{\"error\":\"%s\"}", msg);
			*result_json = strdup(err);
			cJSON_Delete(root);
			return rc;
		}

		struct plan *p = plan_find(g_plans, plan_name);
		char *formatted = p ? format_plan(p) : NULL;

		snprintf(out_buf, sizeof(out_buf),
			"Step %d marked as '%s'.\n%s",
			step_id, status,
			formatted ? formatted : "");
		*result_json = strdup(out_buf);
		free(formatted);
		log_info("plan: updated '%s' step %d -> %s",
			 plan_name, step_id, status);

	} else if (strcmp(command, "get") == 0) {
		cJSON *plan_item = cJSON_GetObjectItem(root, "plan");

		if (cJSON_IsString(plan_item) && plan_item->valuestring) {
			struct plan *p = plan_find(g_plans,
						   plan_item->valuestring);
			if (!p) {
				snprintf(out_buf, sizeof(out_buf),
					"Plan \"%s\" not found.",
					plan_item->valuestring);
			} else {
				char *formatted = format_plan(p);
				snprintf(out_buf, sizeof(out_buf), "%s",
					formatted ? formatted : "(empty)");
				free(formatted);
			}
		} else {
			char fmt[4096];
			rc = plan_get_formatted(g_plans, fmt, sizeof(fmt));
			if (rc < 0) {
				cJSON_Delete(root);
				*result_json = strdup(
					"{\"error\":\"plan formatting failed\"}");
				MORPH_RETURN(rc);
			}
			snprintf(out_buf, sizeof(out_buf), "%s", fmt);
		}
		*result_json = strdup(out_buf);

	} else if (strcmp(command, "list") == 0) {
		char fmt[4096];
		rc = plan_get_formatted(g_plans, fmt, sizeof(fmt));
		if (rc < 0) {
			cJSON_Delete(root);
			*result_json = strdup(
				"{\"error\":\"plan formatting failed\"}");
			MORPH_RETURN(rc);
		}
		snprintf(out_buf, sizeof(out_buf), "%s", fmt);
		*result_json = strdup(out_buf);

	} else {
		cJSON_Delete(root);
		snprintf(out_buf, sizeof(out_buf),
			"{\"error\":\"unknown command '%s'. "
			"Commands: create, update, get, list\"}",
			command);
		*result_json = strdup(out_buf);
		cJSON_Delete(root);
		return -EINVAL;
	}

	cJSON_Delete(root);
	return 0;
}

int plan_tool_init(struct tool_registry *reg, struct plan_registry *plans,
		   struct model *llm)
{
	if (!reg || !plans)
		return -EINVAL;
	g_plans = plans;
	g_llm = llm;
	return tool_register(reg, "plan",
		"Create and manage multi-step plans. "
		"Commands: create (name, goal, steps), "
		"update (plan, step_id, status), get (plan), list. "
		"If 'goal' is provided without 'steps', the plan is "
		"auto-decomposed into steps using AI.",
		"{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\",\"description\":\"create|update|get|list\","
		"\"enum\":[\"create\",\"update\",\"get\",\"list\"]},"
		"\"name\":{\"type\":\"string\",\"description\":\"Plan name (for create)\"},"
		"\"goal\":{\"type\":\"string\",\"description\":\"Plan goal/objective. If steps not provided, auto-decomposed into steps\"},"
		"\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
		"\"description\":\"Array of step descriptions (optional if goal provided)\"},"
		"\"plan\":{\"type\":\"string\",\"description\":\"Plan name to act on (for update/get)\"},"
		"\"step_id\":{\"type\":\"integer\",\"description\":\"Step ID (for update)\"},"
		"\"status\":{\"type\":\"string\",\"description\":\"New status (for update): pending, in_progress, completed, failed, skipped\"}"
		"},\"required\":[\"command\"]}",
		plan_tool_exec, NULL, NULL);
}
