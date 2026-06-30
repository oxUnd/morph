#include "plan.h"
#include "agent/tool.h"
#include "agent/plan.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/arena.h"
#include "util/buf.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "util/error.h"

struct plan_tool_context {
	struct plan_registry *plans;
	struct model *llm;
};

static void plan_tool_context_destroy(void *user_data)
{
	free(user_data);
}

static char *format_plan(struct plan *p)
{
	if (!p)
		return strdup("(no plan)");

	morph_buf_t buf;
	int rc = morph_buf_init(&buf, 4096);
	if (rc != 0)
		return NULL;

	int all_done = 1;
	for (int j = 0; j < p->step_count; j++) {
		if (strcmp(p->steps[j].status, "completed") != 0 &&
		    strcmp(p->steps[j].status, "failed") != 0 &&
		    strcmp(p->steps[j].status, "skipped") != 0) {
			all_done = 0;
			break;
		}
	}

	rc = morph_buf_printf(&buf, "Plan \"%s\" [%s]", p->name, p->id);
	if (rc != 0)
		goto fail;

	if (p->goal[0]) {
		rc = morph_buf_printf(&buf, "\n  Goal: %s", p->goal);
		if (rc != 0)
			goto fail;
	}

	rc = morph_buf_printf(&buf, "\n  %d step(s)", p->step_count);
	if (rc != 0)
		goto fail;
	if (all_done) {
		rc = morph_buf_puts(&buf, " [completed]");
		if (rc != 0)
			goto fail;
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

		rc = morph_buf_printf(&buf, "\n  [%s] %d. %s%s",
				      icon, p->steps[i].id,
				      p->steps[i].description, marker);
		if (rc != 0)
			goto fail;
	}

	return morph_buf_detach(&buf);

fail:
	morph_buf_cleanup(&buf);
	return NULL;
}

static const char *plan_status_icon(const char *status)
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

static char *format_plans(struct plan_registry *reg)
{
	if (!reg || reg->count == 0)
		return strdup("No plans yet. Use plan create to start one.");

	morph_buf_t buf;
	int rc = morph_buf_init(&buf, 4096);
	if (rc != 0)
		return NULL;

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

		rc = morph_buf_printf(&buf, "%sPlan \"%s\" [%s]",
				      i > 0 ? "\n" : "", p->name,
				      p->id);
		if (rc != 0)
			goto fail;
		if (p->goal[0]) {
			rc = morph_buf_printf(&buf, "\n  Goal: %s", p->goal);
			if (rc != 0)
				goto fail;
		}
		rc = morph_buf_printf(&buf, "\n  %d step(s)", p->step_count);
		if (rc != 0)
			goto fail;
		if (all_done) {
			rc = morph_buf_puts(&buf, " [all completed]");
			if (rc != 0)
				goto fail;
		}

		for (int j = 0; j < p->step_count; j++) {
			struct plan_step *s = &p->steps[j];
			const char *icon = plan_status_icon(s->status);
			const char *marker = (j == p->active_step &&
					      p->active_step >= 0)
					   ? " <-- active" : "";

			rc = morph_buf_printf(&buf, "\n  [%s] %d. %s%s",
					      icon, s->id,
					      s->description, marker);
			if (rc != 0)
				goto fail;
		}
	}

	return morph_buf_detach(&buf);

fail:
	morph_buf_cleanup(&buf);
	return NULL;
}

static int plan_all_done(struct plan *p)
{
	if (!p)
		return 0;
	for (int i = 0; i < p->step_count; i++) {
		if (strcmp(p->steps[i].status, "completed") != 0 &&
		    strcmp(p->steps[i].status, "failed") != 0 &&
		    strcmp(p->steps[i].status, "skipped") != 0)
			return 0;
	}
	return 1;
}

static cJSON *plan_to_json(struct plan *p)
{
	if (!p)
		return NULL;

	cJSON *obj = cJSON_CreateObject();
	if (!obj)
		return NULL;

	cJSON *steps = cJSON_CreateArray();
	if (!steps)
		goto fail;

	if (!cJSON_AddStringToObject(obj, "id", p->id) ||
	    !cJSON_AddStringToObject(obj, "name", p->name) ||
	    !cJSON_AddStringToObject(obj, "goal", p->goal) ||
	    !cJSON_AddNumberToObject(obj, "step_count", p->step_count) ||
	    !cJSON_AddNumberToObject(obj, "active_step", p->active_step) ||
	    !cJSON_AddBoolToObject(obj, "all_done", plan_all_done(p)))
		goto fail;

	for (int i = 0; i < p->step_count; i++) {
		struct plan_step *s = &p->steps[i];
		cJSON *step = cJSON_CreateObject();
		if (!step)
			goto fail;
		if (!cJSON_AddNumberToObject(step, "id", s->id) ||
		    !cJSON_AddStringToObject(step, "description",
					     s->description) ||
		    !cJSON_AddStringToObject(step, "status", s->status) ||
		    !cJSON_AddBoolToObject(step, "active",
					   i == p->active_step &&
					   p->active_step >= 0)) {
			cJSON_Delete(step);
			goto fail;
		}
		cJSON_AddItemToArray(steps, step);
	}

	cJSON_AddItemToObject(obj, "steps", steps);
	return obj;

fail:
	cJSON_Delete(steps);
	cJSON_Delete(obj);
	return NULL;
}

static cJSON *plan_registry_to_json(struct plan_registry *reg,
				    const char *command,
				    const char *selected_plan_id,
				    const char *selected_plan)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	cJSON *plans = cJSON_CreateArray();
	if (!plans)
		goto fail;

	if (!cJSON_AddStringToObject(root, "kind", "plan_state") ||
	    !cJSON_AddStringToObject(root, "command",
				     command ? command : "") ||
	    !cJSON_AddNumberToObject(root, "count",
				     reg ? reg->count : 0))
		goto fail;

	if (selected_plan_id &&
	    !cJSON_AddStringToObject(root, "selected_plan_id",
				     selected_plan_id))
		goto fail;

	if (selected_plan &&
	    !cJSON_AddStringToObject(root, "selected_plan", selected_plan))
		goto fail;

	if (reg) {
		for (int i = 0; i < reg->count; i++) {
			cJSON *item = plan_to_json(&reg->plans[i]);
			if (!item)
				goto fail;
			cJSON_AddItemToArray(plans, item);
		}
	}

	cJSON_AddItemToObject(root, "plans", plans);
	return root;

fail:
	cJSON_Delete(plans);
	cJSON_Delete(root);
	return NULL;
}

static cJSON *plan_ui_to_json(cJSON *data)
{
	cJSON *ui = cJSON_CreateObject();
	if (!ui)
		return NULL;

	if (!cJSON_AddStringToObject(ui, "component", "plan") ||
	    !cJSON_AddStringToObject(ui, "version", "1"))
		goto fail;

	cJSON *copy = cJSON_Duplicate(data, 1);
	if (!copy)
		goto fail;
	cJSON_AddItemToObject(ui, "data", copy);
	return ui;

fail:
	cJSON_Delete(ui);
	return NULL;
}

static int attach_plan_state(struct tool_result *result,
			     struct plan_registry *reg,
			     const char *command,
			     const char *selected_plan_id,
			     const char *selected_plan)
{
	cJSON *data = plan_registry_to_json(reg, command, selected_plan_id,
					    selected_plan);
	if (!data)
		return -ENOMEM;

	cJSON *ui = plan_ui_to_json(data);
	if (!ui) {
		cJSON_Delete(data);
		return -ENOMEM;
	}

	int rc = tool_result_take_data(result, data);
	if (rc != 0) {
		cJSON_Delete(ui);
		return rc;
	}

	rc = tool_result_take_ui(result, ui);
	if (rc != 0)
		return rc;
	return 0;
}

static int set_resultf(struct tool_result *result, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static int set_resultf(struct tool_result *result, const char *fmt, ...)
{
	morph_buf_t buf;
	va_list ap;
	int rc;

	if (!result || !fmt)
		return -EINVAL;

	rc = morph_buf_init(&buf, 1024);
	if (rc != 0)
		return rc;

	va_start(ap, fmt);
	rc = morph_buf_vprintf(&buf, fmt, ap);
	va_end(ap);
	if (rc != 0) {
		morph_buf_cleanup(&buf);
		return rc;
	}

	(void)tool_result_take_text(result, morph_buf_detach(&buf));
	if (!result->text.data)
		return -ENOMEM;
	return 0;
}

static int decompose_stream_cb(const char *token, void *user_data)
{
	return morph_buf_append_cb(token, user_data);
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

static int auto_decompose(struct model *llm, const char *goal,
			  const char **step_descs, int max_steps)
{
	if (!goal || !step_descs || max_steps <= 0)
		return -EINVAL;
	if (!llm || !llm->chat || !llm->api_key[0])
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

	morph_buf_t buf;
	int rc2 = morph_buf_init(&buf, 8192);
	if (rc2 != 0) {
		arena_destroy(arena);
		return rc2;
	}

	const char *messages[] = { prompt };
	int status = llm->chat(llm, arena, sys, messages, 1,
			       decompose_stream_cb, &buf);
	arena_destroy(arena);

	if (status < 0 || buf.len == 0) {
		morph_buf_cleanup(&buf);
		MORPH_RETURN(MORPH_ERR_LLM);
	}

	char *text = morph_buf_detach(&buf);
	if (!text)
		MORPH_RETURN(-ENOMEM);
	int count = parse_steps_from_text(text, step_descs, max_steps);
	free(text);
	return count;
}

static void free_step_descs(const char **descs, int count)
{
	for (int i = 0; i < count; i++)
		free((void *)descs[i]);
}

static int plan_tool_exec(const char *args_json, struct tool_result *result,
			  void *user_data)
{
	struct plan_tool_context *ctx = user_data;

	if (!result)
		return -EINVAL;
	if (!ctx || !ctx->plans)
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_take_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	cJSON *cmd = cJSON_GetObjectItem(root, "command");
	if (!cJSON_IsString(cmd)) {
		cJSON_Delete(root);
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing 'command' parameter. "
			"Commands: create, update, get, list\"}"));
		return -EINVAL;
	}

	const char *command = cmd->valuestring;
	int rc = 0;

	if (strcmp(command, "create") == 0) {
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *goal_item = cJSON_GetObjectItem(root, "goal");
		cJSON *steps_item = cJSON_GetObjectItem(root, "steps");

		if (!cJSON_IsString(name_item) || !name_item->valuestring) {
			cJSON_Delete(root);
			(void)tool_result_take_text(result, strdup(
				"{\"error\":\"create requires 'name' (string)\"}"));
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
			rc = auto_decompose(ctx->llm, goal, tmp_descs,
					    PLAN_MAX_STEPS);
			if (rc < 0) {
				free_step_descs(tmp_descs, PLAN_MAX_STEPS);
				cJSON_Delete(root);
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"auto-decompose failed\"}"));
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
			(void)tool_result_take_text(result, strdup(
				"{\"error\":\"create requires 'steps' (array of strings) "
				"or a 'goal' string to auto-decompose\"}"));
			return -EINVAL;
		}

		struct plan *p = plan_create(ctx->plans, name_item->valuestring,
			goal, step_descs, step_count);

		if (!p) {
			if (auto_decomposed)
				free_step_descs(step_descs, step_count);
			cJSON_Delete(root);
			(void)tool_result_take_text(result, strdup(
				"{\"error\":\"failed to create plan "
				"(max 8 plans, max 32 steps each)\"}"));
			return -ENOSPC;
		}

		char *formatted = format_plan(p);
		if (!formatted) {
			if (auto_decomposed)
				free_step_descs(step_descs, step_count);
			cJSON_Delete(root);
			(void)tool_result_take_text(result, strdup("{\"error\":\"out of memory\"}"));
			return -ENOMEM;
		}

		if (auto_decomposed) {
			rc = set_resultf(result,
				"Plan created (auto-decomposed from goal).\n%s",
				formatted);
			free_step_descs(step_descs, step_count);
		} else {
			rc = set_resultf(result, "Plan created.\n%s",
					 formatted);
		}
		free(formatted);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command, p->id,
				       p->name);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
		log_info("plan: created '%s' with %d steps%s",
			 p->name, p->step_count,
			 auto_decomposed ? " (auto)" : "");

	} else if (strcmp(command, "update") == 0) {
		cJSON *plan_id_item = cJSON_GetObjectItem(root, "plan_id");
		cJSON *plan_item = cJSON_GetObjectItem(root, "plan");
		cJSON *step_id_item = cJSON_GetObjectItem(root, "step_id");
		cJSON *status_item = cJSON_GetObjectItem(root, "status");

		const char *plan_id = cJSON_IsString(plan_id_item)
				     ? plan_id_item->valuestring : NULL;
		const char *plan_name = cJSON_IsString(plan_item)
				       ? plan_item->valuestring : NULL;
		int step_id = cJSON_IsNumber(step_id_item)
			     ? (int)step_id_item->valuedouble : -1;
		const char *status = cJSON_IsString(status_item)
				    ? status_item->valuestring : NULL;

		if ((!plan_id && !plan_name) || step_id < 0 || !status) {
			cJSON_Delete(root);
			(void)tool_result_take_text(result, strdup(
				"{\"error\":\"update requires 'plan_id' or 'plan', "
				"'step_id' (int), and 'status' (string). "
				"Status: pending, in_progress, completed, "
				"failed, skipped\"}"));
			return -EINVAL;
		}

		rc = plan_id ? plan_update_step_by_id(ctx->plans, plan_id,
						      step_id, status) :
			       plan_update_step(ctx->plans, plan_name,
						step_id, status);
		if (rc < 0) {
			char err[256];
			const char *msg;
			if (rc == -ENOENT)
				msg = "plan or step not found";
			else
				msg = "invalid parameters";
			snprintf(err, sizeof(err),
				"{\"error\":\"%s\"}", msg);
			(void)tool_result_take_text(result, strdup(err));
			cJSON_Delete(root);
			return rc;
		}

		struct plan *p = plan_id ? plan_find_by_id(ctx->plans,
							   plan_id) :
					   plan_find(ctx->plans, plan_name);
		char *formatted = p ? format_plan(p) : NULL;

		rc = set_resultf(result, "Step %d marked as '%s'.\n%s",
				 step_id, status,
				 formatted ? formatted : "");
		free(formatted);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command,
				       p ? p->id : plan_id,
				       p ? p->name : plan_name);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
		log_info("plan: updated '%s' step %d -> %s",
			 p ? p->id : (plan_id ? plan_id : plan_name),
			 step_id, status);

	} else if (strcmp(command, "get") == 0) {
		cJSON *plan_id_item = cJSON_GetObjectItem(root, "plan_id");
		cJSON *plan_item = cJSON_GetObjectItem(root, "plan");
		const char *plan_id = cJSON_IsString(plan_id_item)
				     ? plan_id_item->valuestring : NULL;

		if (plan_id || (cJSON_IsString(plan_item) &&
		    plan_item->valuestring)) {
			struct plan *p = plan_id ? plan_find_by_id(ctx->plans,
								   plan_id) :
						   plan_find(ctx->plans,
							     plan_item->valuestring);
			if (!p) {
				rc = set_resultf(result,
					"Plan \"%s\" not found.",
					plan_id ? plan_id :
					plan_item->valuestring);
			} else {
				char *formatted = format_plan(p);
				rc = set_resultf(result, "%s",
					formatted ? formatted : "(empty)");
				free(formatted);
			}
		} else {
			char *formatted = format_plans(ctx->plans);
			rc = set_resultf(result, "%s",
					 formatted ? formatted : "(empty)");
			free(formatted);
		}
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command,
				       plan_id,
				       cJSON_IsString(plan_item)
				       ? plan_item->valuestring : NULL);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}

	} else if (strcmp(command, "list") == 0) {
		char *formatted = format_plans(ctx->plans);
		rc = set_resultf(result, "%s",
				 formatted ? formatted : "(empty)");
		free(formatted);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command, NULL, NULL);
		if (rc != 0) {
			cJSON_Delete(root);
			return rc;
		}

	} else {
		rc = set_resultf(result,
			"{\"error\":\"unknown command '%s'. "
			"Commands: create, update, get, list\"}",
			command);
		cJSON_Delete(root);
		return rc != 0 ? rc : -EINVAL;
	}

	cJSON_Delete(root);
	return 0;
}

int plan_tool_init(struct tool_registry *reg, struct plan_registry *plans,
		   struct model *llm)
{
	if (!reg || !plans)
		return -EINVAL;

	struct plan_tool_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->plans = plans;
	ctx->llm = llm;

	int rc = tool_register(reg, "plan",
		"Create and manage multi-step plans. "
		"Commands: create (name, goal, steps), "
		"update (plan_id or plan, step_id, status), "
		"get (plan_id or plan), list. "
		"If 'goal' is provided without 'steps', the plan is "
		"auto-decomposed into steps using AI.",
		"{\"type\":\"object\",\"properties\":{"
		"\"command\":{\"type\":\"string\",\"description\":\"create|update|get|list\","
		"\"enum\":[\"create\",\"update\",\"get\",\"list\"]},"
		"\"name\":{\"type\":\"string\",\"description\":\"Plan name (for create)\"},"
		"\"goal\":{\"type\":\"string\",\"description\":\"Plan goal/objective. If steps not provided, auto-decomposed into steps\"},"
		"\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
		"\"description\":\"Array of step descriptions (optional if goal provided)\"},"
		"\"plan_id\":{\"type\":\"string\",\"description\":\"Stable plan id to act on (preferred for update/get)\"},"
		"\"plan\":{\"type\":\"string\",\"description\":\"Plan name to act on (for update/get)\"},"
		"\"step_id\":{\"type\":\"integer\",\"description\":\"Step ID (for update)\"},"
		"\"status\":{\"type\":\"string\",\"description\":\"New status (for update): pending, in_progress, completed, failed, skipped\"}"
		"},\"required\":[\"command\"]}",
		plan_tool_exec, ctx, plan_tool_context_destroy);
	if (rc != 0)
		free(ctx);
	return rc;
}
