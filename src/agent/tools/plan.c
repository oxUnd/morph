#include "plan.h"
#include "agent/tool.h"
#include "agent/plan.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/buf.h"
#include "util/str.h"
#include "util/utf8.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "util/error.h"
#include "util/sha256.h"
#include "agent/tool_context.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

struct plan_tool_context {
	unsigned int refs;
	pthread_mutex_t mutex;
	struct plan_registry *plans;
	struct tool_context *tctx;
	struct model *llm;
};

static void plan_tool_context_destroy(void *user_data)
{
	struct plan_tool_context *ctx = user_data;
	if (ctx && __atomic_sub_fetch(&ctx->refs, 1, __ATOMIC_ACQ_REL) == 0) {
		pthread_mutex_destroy(&ctx->mutex);
		free(ctx);
	}
}

static char *format_plan(struct plan *p)
{
	if (!p)
		return strdup("(no plan)");

	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
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
		return NULL;

	if (p->goal[0]) {
		rc = morph_buf_printf(&buf, "\n  Goal: %s", p->goal);
		if (rc != 0)
			return NULL;
	}

	rc = morph_buf_printf(&buf, "\n  %d step(s)", p->step_count);
	if (rc != 0)
		return NULL;
	if (all_done) {
		rc = morph_buf_puts(&buf, " [completed]");
		if (rc != 0)
			return NULL;
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
			return NULL;
	}

	return morph_buf_detach(&buf);
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

	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
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
			return NULL;
		if (p->goal[0]) {
			rc = morph_buf_printf(&buf, "\n  Goal: %s", p->goal);
			if (rc != 0)
				return NULL;
		}
		rc = morph_buf_printf(&buf, "\n  %d step(s)", p->step_count);
		if (rc != 0)
			return NULL;
		if (all_done) {
			rc = morph_buf_puts(&buf, " [all completed]");
			if (rc != 0)
				return NULL;
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
				return NULL;
		}
	}

	return morph_buf_detach(&buf);
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

	cJSON *obj MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	if (!obj)
		return NULL;

	cJSON *steps MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateArray();
	if (!steps)
		return NULL;

	if (!cJSON_AddStringToObject(obj, "id", p->id) ||
	    !cJSON_AddStringToObject(obj, "name", p->name) ||
	    !cJSON_AddStringToObject(obj, "goal", p->goal) ||
	    !cJSON_AddStringToObject(obj, "request", p->request) ||
	    !cJSON_AddStringToObject(obj, "constraints", p->constraints) ||
	    !cJSON_AddStringToObject(obj, "acceptance", p->acceptance) ||
	    !cJSON_AddStringToObject(obj, "rejected", p->rejected) ||
	    !cJSON_AddStringToObject(obj, "verification", p->verification) ||
	    !cJSON_AddNumberToObject(obj, "step_count", p->step_count) ||
	    !cJSON_AddNumberToObject(obj, "active_step", p->active_step) ||
	    !cJSON_AddBoolToObject(obj, "all_done", plan_all_done(p)))
		return NULL;

	for (int i = 0; i < p->step_count; i++) {
		struct plan_step *s = &p->steps[i];
		cJSON *step MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
		if (!step)
			return NULL;
		if (!cJSON_AddNumberToObject(step, "id", s->id) ||
		    !cJSON_AddStringToObject(step, "description", s->description) ||
		    !cJSON_AddStringToObject(step, "status", s->status) ||
		    !cJSON_AddStringToObject(step, "evidence", s->evidence) ||
		    !cJSON_AddStringToObject(step, "blocker", s->blocker) ||
		    !cJSON_AddStringToObject(step, "artifact", s->artifact) ||
		    !cJSON_AddBoolToObject(step, "active",
					   i == p->active_step && p->active_step >= 0)) {
			return NULL;
		}
		if (morph_json_array_take(steps, &step) != 0)
			return NULL;
	}

	if (morph_json_object_take(obj, "steps", &steps) != 0)
		return NULL;
	return morph_json_steal(&obj);
}

static cJSON *plan_registry_to_json(struct plan_registry *reg,
				    const char *command,
				    const char *selected_plan_id,
				    const char *selected_plan)
{
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	if (!root)
		return NULL;

	cJSON *plans MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateArray();
	if (!plans)
		return NULL;

	if (!cJSON_AddStringToObject(root, "kind", "plan_state") ||
	    !cJSON_AddStringToObject(root, "command",
				     command ? command : "") ||
	    !cJSON_AddNumberToObject(root, "count",
				     reg ? reg->count : 0))
		return NULL;

	if (selected_plan_id &&
	    !cJSON_AddStringToObject(root, "selected_plan_id",
				     selected_plan_id))
		return NULL;

	if (selected_plan &&
	    !cJSON_AddStringToObject(root, "selected_plan", selected_plan))
		return NULL;

	if (reg) {
		for (int i = 0; i < reg->count; i++) {
			cJSON *item MORPH_CLEANUP(morph_json_cleanup) =
				plan_to_json(&reg->plans[i]);
			if (!item)
				return NULL;
			if (morph_json_array_take(plans, &item) != 0)
				return NULL;
		}
	}

	if (morph_json_object_take(root, "plans", &plans) != 0)
		return NULL;
	return morph_json_steal(&root);
}

static cJSON *plan_ui_to_json(cJSON *data)
{
	cJSON *ui MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	cJSON *summary MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *item;

	if (!ui)
		return NULL;

	if (!cJSON_AddStringToObject(ui, "component", "plan") ||
	    !cJSON_AddStringToObject(ui, "version", "1"))
		return NULL;

	summary = cJSON_CreateObject();
	if (!summary)
		return NULL;
	if (data) {
		item = cJSON_GetObjectItem(data, "kind");
		if (cJSON_IsString(item) &&
		    !cJSON_AddStringToObject(summary, "kind",
					     item->valuestring))
			return NULL;
		item = cJSON_GetObjectItem(data, "command");
		if (cJSON_IsString(item) &&
		    !cJSON_AddStringToObject(summary, "command",
					     item->valuestring))
			return NULL;
		item = cJSON_GetObjectItem(data, "count");
		if (cJSON_IsNumber(item) &&
		    !cJSON_AddNumberToObject(summary, "count",
					     item->valuedouble))
			return NULL;
		item = cJSON_GetObjectItem(data, "selected_plan_id");
		if (cJSON_IsString(item) &&
		    !cJSON_AddStringToObject(summary, "selected_plan_id",
					     item->valuestring))
			return NULL;
		item = cJSON_GetObjectItem(data, "selected_plan");
		if (cJSON_IsString(item) &&
		    !cJSON_AddStringToObject(summary, "selected_plan",
					     item->valuestring))
			return NULL;
	}
	if (morph_json_object_take(ui, "data", &summary) != 0)
		return NULL;
	return morph_json_steal(&ui);
}

static int attach_plan_state(struct tool_result *result, struct plan_registry *reg,
			     const char *command, const char *selected_plan_id,
			     const char *selected_plan)
{
	cJSON *data MORPH_CLEANUP(morph_json_cleanup) =
		plan_registry_to_json(reg, command, selected_plan_id, selected_plan);
	if (!data)
		return -ENOMEM;
	cJSON *ui MORPH_CLEANUP(morph_json_cleanup) = plan_ui_to_json(data);
	if (!ui)
		return -ENOMEM;
	int rc = tool_result_success_take(result, &data);
	if (rc == 0)
		rc = tool_result_set_ui_take(result, &ui);
	if (rc == 0)
		rc = tool_result_finalize(result);
	return rc;
}

static int set_resultf(struct tool_result *result, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static int set_resultf(struct tool_result *result, const char *fmt, ...)
{
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
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

	return tool_result_success_json_text_copy(result, morph_buf_cstr_borrow(&buf));
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

static int auto_decompose(const struct tool_runtime_context *runtime, struct model *llm,
			  const char *goal, const char **step_descs, int max_steps)
{
	if (!goal || !step_descs || max_steps <= 0)
		return -EINVAL;
	if (!llm || !llm->chat || !model_api_key_borrow(llm)[0])
		return MORPH_ERR_NOT_CONFIGURED;

	char prompt[2048];
	snprintf(prompt, sizeof(prompt),
		"Break down the following goal into a step-by-step plan. "
		"Return ONLY a numbered list of %d-%d specific, actionable steps. "
		"Each step should start with a number followed by a period. "
		"Be concise.\n\nGoal: %s",
		2, max_steps, goal);

	const char *sys = "You are a task planner. Break goals into numbered steps. "
			  "Output ONLY the numbered list, no preamble.";

	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	struct tool_runtime_stream_sink stream;
	int rc2 = morph_buf_init(&buf, 8192);
	if (rc2 != 0) {
		return rc2;
	}

	struct model request_model;

	llm = model_request_view_init(llm, runtime ? &runtime->http : NULL, &request_model);
	const char *messages[] = { prompt };
	stream.runtime = runtime;
	stream.tool = "plan";
	stream.kind = "text";
	stream.buf = &buf;
	int status = llm->chat(llm, sys, messages, 1, NULL, tool_runtime_stream_to_buf_cb, &stream);

	if (status < 0 || buf.len == 0) {
		morph_buf_cleanup(&buf);
		return MORPH_ERR_LLM;
	}

	if (buf.failed)
		return buf.failed;
	return parse_steps_from_text(morph_buf_cstr_borrow(&buf), step_descs, max_steps);
}

static void free_step_descs(const char **descs, int count)
{
	for (int i = 0; i < count; i++)
		free((void *)descs[i]);
}

static int plan_tool_exec_inner(const char *args_json, struct tool_result *result, void *user_data)
{
	struct plan_tool_context *ctx = user_data;

	if (!result)
		return -EINVAL;
	if (!ctx || !ctx->plans)
		return MORPH_ERR_NOT_CONFIGURED;

	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_success_json_text_copy(result, "{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}

	cJSON *cmd = cJSON_GetObjectItem(root, "command");
	if (!cJSON_IsString(cmd)) {

		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"missing 'command' parameter. "
				"Commands: create, update, get, list\"}");
		return -EINVAL;
	}

	const char *command = cmd->valuestring;
	int rc = 0;

	if (strcmp(command, "create") == 0) {
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *goal_item = cJSON_GetObjectItem(root, "goal");
		cJSON *steps_item = cJSON_GetObjectItem(root, "steps");

		if (!cJSON_IsString(name_item) || !name_item->valuestring) {

			(void)tool_result_success_json_text_copy(
				result, "{\"error\":\"create requires 'name' (string)\"}");
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
			rc = auto_decompose(result->runtime, ctx->llm, goal, tmp_descs,
					    PLAN_MAX_STEPS);
			if (rc < 0) {
				free_step_descs(tmp_descs, PLAN_MAX_STEPS);

				(void)tool_result_success_json_text_copy(
					result, "{\"error\":\"auto-decompose failed\"}");
				return rc;
			}
			if (rc > 0) {
				step_count = rc;
				for (int i = 0; i < step_count; i++)
					step_descs[i] = tmp_descs[i];
				auto_decomposed = 1;
			}
		}

		if (step_count == 0) {

			(void)tool_result_success_json_text_copy(
				result, "{\"error\":\"create requires 'steps' (array of strings) "
					"or a 'goal' string to auto-decompose\"}");
			return -EINVAL;
		}

		struct plan *p = plan_add_borrow(ctx->plans, name_item->valuestring, goal,
						 step_descs, step_count);

		if (!p) {
			if (auto_decomposed)
				free_step_descs(step_descs, step_count);

			(void)tool_result_success_json_text_copy(
				result, "{\"error\":\"failed to create plan "
					"(max 8 plans, max 32 steps each)\"}");
			return -ENOSPC;
		}

		char *formatted MORPH_CLEANUP(morph_charp_cleanup) = format_plan(p);
		if (!formatted) {
			if (auto_decomposed)
				free_step_descs(step_descs, step_count);

			(void)tool_result_success_json_text_copy(result,
								 "{\"error\":\"out of memory\"}");
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

		if (rc != 0) {

			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command, p->id,
				       p->name);
		if (rc != 0) {

			return rc;
		}
		log_info(result && result->runtime ? result->runtime->http.logger : NULL,
			 "plan: created '%s' with %d steps%s", p->name, p->step_count,
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

			(void)tool_result_success_json_text_copy(
				result, "{\"error\":\"update requires 'plan_id' or 'plan', "
					"'step_id' (int), and 'status' (string). "
					"Status: pending, in_progress, completed, "
					"failed, skipped\"}");
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
			(void)tool_result_success_json_text_copy(result, err);

			return rc;
		}

		struct plan *p = plan_id ? plan_by_id_borrow(ctx->plans, plan_id)
					 : plan_borrow(ctx->plans, plan_name);
		char *formatted MORPH_CLEANUP(morph_charp_cleanup) = p ? format_plan(p) : NULL;

		rc = set_resultf(result, "Step %d marked as '%s'.\n%s",
				 step_id, status,
				 formatted ? formatted : "");

		if (rc != 0) {

			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command,
				       p ? p->id : plan_id,
				       p ? p->name : plan_name);
		if (rc != 0) {

			return rc;
		}
		log_info(result && result->runtime ? result->runtime->http.logger : NULL,
			 "plan: updated '%s' step %d -> %s",
			 p ? p->id : (plan_id ? plan_id : plan_name), step_id, status);

	} else if (strcmp(command, "get") == 0) {
		cJSON *plan_id_item = cJSON_GetObjectItem(root, "plan_id");
		cJSON *plan_item = cJSON_GetObjectItem(root, "plan");
		const char *plan_id = cJSON_IsString(plan_id_item)
				     ? plan_id_item->valuestring : NULL;

		if (plan_id || (cJSON_IsString(plan_item) &&
		    plan_item->valuestring)) {
			struct plan *p = plan_id ? plan_by_id_borrow(ctx->plans, plan_id)
						 : plan_borrow(ctx->plans, plan_item->valuestring);
			if (!p) {
				rc = set_resultf(result,
					"Plan \"%s\" not found.",
					plan_id ? plan_id :
					plan_item->valuestring);
			} else {
				char *formatted MORPH_CLEANUP(morph_charp_cleanup) = format_plan(p);
				rc = set_resultf(result, "%s",
					formatted ? formatted : "(empty)");
			}
		} else {
			char *formatted MORPH_CLEANUP(morph_charp_cleanup) =
				format_plans(ctx->plans);
			rc = set_resultf(result, "%s",
					 formatted ? formatted : "(empty)");
		}
		if (rc != 0) {

			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command,
				       plan_id,
				       cJSON_IsString(plan_item)
				       ? plan_item->valuestring : NULL);
		if (rc != 0) {

			return rc;
		}

	} else if (strcmp(command, "list") == 0) {
		char *formatted MORPH_CLEANUP(morph_charp_cleanup) = format_plans(ctx->plans);
		rc = set_resultf(result, "%s",
				 formatted ? formatted : "(empty)");

		if (rc != 0) {

			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command, NULL, NULL);
		if (rc != 0) {

			return rc;
		}

	} else {
		rc = set_resultf(result,
			"{\"error\":\"unknown command '%s'. "
			"Commands: create, update, get, list\"}",
			command);

		return rc != 0 ? rc : -EINVAL;
	}

	return 0;
}

static int plan_file_digest(struct plan_tool_context *ctx, const char *path,
			    char resolved[PATH_MAX], char hex[65])
{
	struct sha256_ctx hash;
	unsigned char digest[32];
	unsigned char buf[BUFSIZ];
	struct stat st;
	int fd;
	int rc;
	if (!ctx->tctx || !path)
		return -EINVAL;
	rc = tool_context_authorize_path(ctx->tctx, TOOL_PATH_READ, path, resolved, PATH_MAX);
	if (rc != 0)
		return rc;
	fd = open(resolved, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return morph_error_from_errno(errno);
	if (fstat(fd, &st) != 0) {
		rc = -errno;
		close(fd);
		return rc;
	}
	if (!S_ISREG(st.st_mode) || st.st_size > 64 * 1024 * 1024) {
		close(fd);
		return -EFBIG;
	}
	sha256_init(&hash);
	size_t total = 0;
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			rc = -errno;
			close(fd);
			return rc;
		}
		if (!n)
			break;
		total += (size_t)n;
		if (total > 64u * 1024u * 1024u) {
			close(fd);
			return -EFBIG;
		}
		sha256_update(&hash, buf, (size_t)n);
	}
	close(fd);
	sha256_final(&hash, digest);
	for (size_t i = 0; i < sizeof(digest); i++)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	return 0;
}

static int plan_capture_report(struct plan_tool_context *ctx, const cJSON *args, cJSON *selected)
{
	const cJSON *files = cJSON_GetObjectItemCaseSensitive(args, "files");
	const char *report = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "report"));
	const char *outcome =
	    cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "outcome"));
	cJSON *state;
	cJSON *inputs;
	int rc = 0;
	if (!cJSON_IsArray(files) || cJSON_GetArraySize(files) < 1 ||
	    cJSON_GetArraySize(files) > 16 || !report || !outcome ||
	    (strcmp(outcome, "passed") && strcmp(outcome, "failed")))
		return -EINVAL;
	state = cJSON_CreateObject();
	inputs = state ? cJSON_AddArrayToObject(state, "inputs") : NULL;
	if (!inputs || !cJSON_AddStringToObject(state, "status", "current") ||
	    !cJSON_AddStringToObject(state, "outcome", outcome) ||
	    !cJSON_AddStringToObject(state, "report", report)) {
		cJSON_Delete(state);
		return -ENOMEM;
	}
	/* Include the report itself so replacing its contents invalidates reuse. */
	for (int i = -1; i < cJSON_GetArraySize(files); i++) {
		const char *path =
		    i < 0 ? report : cJSON_GetStringValue(cJSON_GetArrayItem(files, i));
		char resolved[PATH_MAX];
		char hex[65];
		rc = plan_file_digest(ctx, path, resolved, hex);
		if (rc != 0)
			break;
		cJSON *input = cJSON_CreateObject();
		if (!input) {
			rc = -ENOMEM;
			break;
		}
		cJSON_AddItemToArray(inputs, input);
		if (!cJSON_AddStringToObject(input, "path", resolved) ||
		    !cJSON_AddStringToObject(input, "sha256", hex)) {
			rc = -ENOMEM;
			break;
		}
	}
	char *text = rc == 0 ? cJSON_PrintUnformatted(state) : NULL;
	if (rc == 0 && (!text || strlen(text) >= sizeof(ctx->plans->plans[0].verification)))
		rc = text ? -E2BIG : -ENOMEM;
	if (rc == 0) {
		cJSON *value = cJSON_CreateString(text);
		if (!value ||
		    !cJSON_ReplaceItemInObjectCaseSensitive(selected, "verification", value)) {
			cJSON_Delete(value);
			rc = -ENOMEM;
		}
	}
	free(text);
	cJSON_Delete(state);
	return rc;
}

void plan_tool_set_context(struct tool_registry *reg, struct tool_context *tctx)
{
	struct tool_entry *entry = tool_borrow(reg, "plan");
	if (entry && entry->user_data)
		((struct plan_tool_context *)entry->user_data)->tctx = tctx;
}

static int plan_refresh_reports(struct plan_tool_context *ctx)
{
	int changed = 0;
	if (!ctx || !ctx->tctx)
		return 0;
	for (int i = 0; i < ctx->plans->count; i++) {
		struct plan *p = &ctx->plans->plans[i];
		cJSON *state = cJSON_Parse(p->verification);
		const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(state, "status"));
		if (!status || strcmp(status, "current")) {
			cJSON_Delete(state);
			continue;
		}
		cJSON *input;
		int stale = 0;
		cJSON_ArrayForEach(input, cJSON_GetObjectItem(state, "inputs"))
		{
			const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(input, "path"));
			const char *expected =
			    cJSON_GetStringValue(cJSON_GetObjectItem(input, "sha256"));
			char resolved[PATH_MAX];
			char hex[65];
			if (!expected || plan_file_digest(ctx, path, resolved, hex) != 0 ||
			    strcmp(expected, hex)) {
				stale = 1;
				break;
			}
		}
		if (stale) {
			cJSON_ReplaceItemInObjectCaseSensitive(state, "status",
							       cJSON_CreateString("stale"));
			char *text = cJSON_PrintUnformatted(state);
			if (!text) {
				cJSON_Delete(state);
				return -ENOMEM;
			}
			snprintf(p->verification, sizeof(p->verification), "%s", text);
			free(text);
			changed = 1;
		}
		cJSON_Delete(state);
	}
	return changed ? plan_registry_save(ctx->plans) : 0;
}

int plan_tool_refresh_reports(struct tool_registry *reg)
{
	struct tool_entry *entry = reg ? tool_borrow(reg, "plan") : NULL;
	struct plan_tool_context *ctx = entry ? entry->user_data : NULL;
	if (!ctx) return 0;
	pthread_mutex_lock(&ctx->mutex);
	int rc = plan_refresh_reports(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

static int plan_tool_exec_checked(const char *args_json, struct tool_result *result, void *user_data)
{
	struct plan_tool_context *ctx = user_data;
	cJSON *args = cJSON_Parse(args_json);
	cJSON *before;
	cJSON *state;
	const char *command = cJSON_GetStringValue(cJSON_GetObjectItem(args, "command"));
	int rc;
	if (!ctx || !ctx->plans || !command) {
		cJSON_Delete(args);
		return -EINVAL;
	}
	rc = plan_refresh_reports(ctx);
	if (rc != 0) {
		cJSON_Delete(args);
		return rc;
	}
	before = plan_registry_json_create(ctx->plans);
	if (!before) {
		cJSON_Delete(args);
		return -ENOMEM;
	}
	if (!strcmp(command, "checkpoint") || !strcmp(command, "verify"))
		rc = 0;
	else
		rc = plan_tool_exec_inner(args_json, result, user_data);
	state = rc == 0 ? plan_registry_json_create(ctx->plans) : NULL;
	if (rc == 0 && !state)
		rc = -ENOMEM;
	if (rc == 0 && (!strcmp(command, "checkpoint") || !strcmp(command, "create") ||
			!strcmp(command, "update") || !strcmp(command, "verify"))) {
		const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(args, "plan_id"));
		const char *name = cJSON_GetStringValue(
		    cJSON_GetObjectItem(args, !strcmp(command, "create") ? "name" : "plan"));
		cJSON *selected = NULL;
		cJSON *entry;
		cJSON_ArrayForEach(entry, state)
		{
			const char *entry_id =
			    cJSON_GetStringValue(cJSON_GetObjectItem(entry, "id"));
			const char *entry_name =
			    cJSON_GetStringValue(cJSON_GetObjectItem(entry, "name"));
			if ((id && entry_id && !strcmp(id, entry_id)) ||
			    (!id && name && entry_name && !strcmp(name, entry_name))) {
				if (!selected || !strcmp(command, "create"))
					selected = entry;
			}
		}
		if (!selected)
			rc = -ENOENT;
		if (rc == 0 && !strcmp(command, "verify"))
			rc = plan_capture_report(ctx, args, selected);
		const char *fields[] = {"request", "constraints", "acceptance", "rejected"};
		for (size_t i = 0; selected && i < sizeof(fields) / sizeof(fields[0]); i++) {
			cJSON *value = cJSON_GetObjectItemCaseSensitive(args, fields[i]);
			if (!value)
				continue;
			cJSON *copy = cJSON_Duplicate(value, 1);
			if (!copy ||
			    !cJSON_ReplaceItemInObjectCaseSensitive(selected, fields[i], copy)) {
				cJSON_Delete(copy);
				rc = -ENOMEM;
				break;
			}
		}
		cJSON *step_id = cJSON_GetObjectItem(args, "step_id");
		if (selected && cJSON_IsNumber(step_id)) {
			cJSON *step = cJSON_GetArrayItem(cJSON_GetObjectItem(selected, "steps"),
							 step_id->valueint - 1);
			const char *step_fields[] = {"evidence", "blocker", "artifact"};
			if (!step)
				rc = -ENOENT;
			for (size_t i = 0; step && i < sizeof(step_fields) / sizeof(step_fields[0]);
			     i++) {
				cJSON *value =
				    cJSON_GetObjectItemCaseSensitive(args, step_fields[i]);
				if (!value)
					continue;
				cJSON *copy = cJSON_Duplicate(value, 1);
				if (!copy || !cJSON_ReplaceItemInObjectCaseSensitive(
						 step, step_fields[i], copy)) {
					cJSON_Delete(copy);
					rc = -ENOMEM;
					break;
				}
			}
		}
		if (rc == 0)
			rc = plan_registry_restore(ctx->plans, state);
		if (rc == 0)
			rc = attach_plan_state(
			    result, ctx->plans, command,
			    cJSON_GetStringValue(cJSON_GetObjectItem(selected, "id")), name);
		if (rc == 0)
			rc = plan_registry_save(ctx->plans);
	}
	if (rc != 0) {
		(void)plan_registry_restore(ctx->plans, before);
		(void)tool_result_error_copy(result, "plan_checkpoint_failed", morph_strerror(rc));
	}
	cJSON_Delete(state);
	cJSON_Delete(before);
	cJSON_Delete(args);
	return rc;
}

static int plan_tool_exec(const char *args_json, struct tool_result *result, void *user_data)
{
	struct plan_tool_context *ctx = user_data;
	if (!ctx) return -EINVAL;
	pthread_mutex_lock(&ctx->mutex);
	int rc = plan_tool_exec_checked(args_json, result, user_data);
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}

#define UPDATE_PLAN_NAME "Task plan"

static void update_plan_candidate_cleanup(struct plan **candidate)
{
	if (candidate) {
		free(*candidate);
		*candidate = NULL;
	}
}

static int update_plan_parse(const cJSON *args, struct plan *candidate)
{
	const cJSON *steps = cJSON_GetObjectItemCaseSensitive(args, "plan");
	const cJSON *explanation = cJSON_GetObjectItemCaseSensitive(args, "explanation");
	if (!cJSON_IsObject(args) || !cJSON_IsArray(steps) ||
	    cJSON_GetArraySize(steps) > PLAN_MAX_STEPS ||
	    (explanation && (!cJSON_IsString(explanation) || utf8valid(explanation->valuestring))))
		return -EINVAL;
	int fields = 0;
	const cJSON *field;
	cJSON_ArrayForEach (field, args) {
		int bit = !strcmp(field->string, "plan")	  ? 1
			  : !strcmp(field->string, "explanation") ? 2
								  : 0;
		if (!bit || (fields & bit))
			return -EINVAL;
		fields |= bit;
	}
	candidate->active_step = -1;
	candidate->step_count = cJSON_GetArraySize(steps);
	for (int i = 0; i < candidate->step_count; i++) {
		const cJSON *item = cJSON_GetArrayItem(steps, i);
		const char *text =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "step"));
		const char *status =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "status"));
		if (!cJSON_IsObject(item) || !text || !status || utf8valid(text) ||
		    strlen(text) >= sizeof(candidate->steps[i].description) ||
		    !morph_str_trim_borrow((morph_str_t){strlen(text), text}).len ||
		    (strcmp(status, "pending") && strcmp(status, "in_progress") &&
		     strcmp(status, "completed")))
			return -EINVAL;
		fields = 0;
		cJSON_ArrayForEach (field, item) {
			int bit = !strcmp(field->string, "step")     ? 1
				  : !strcmp(field->string, "status") ? 2
								     : 0;
			if (!bit || (fields & bit))
				return -EINVAL;
			fields |= bit;
		}
		if (!strcmp(status, "in_progress")) {
			if (candidate->active_step >= 0)
				return -EINVAL;
			candidate->active_step = i;
		}
		candidate->steps[i].id = i + 1;
		memcpy(candidate->steps[i].description, text, strlen(text) + 1);
		memcpy(candidate->steps[i].status, status, strlen(status) + 1);
	}
	return 0;
}

static int update_plan_apply(struct plan_tool_context *ctx, const cJSON *args,
			     const struct plan *candidate, struct tool_result *result,
			     cJSON **event_data)
{
	struct plan *target = plan_borrow(ctx->plans, UPDATE_PLAN_NAME);
	if (!target) {
		const char *placeholder[] = {"Task plan"};
		if (ctx->plans->count >= PLAN_MAX_PLANS)
			return -ENOSPC;
		target = plan_add_borrow(ctx->plans, UPDATE_PLAN_NAME, "", placeholder, 1);
		if (!target)
			return -ENOMEM;
	}
	memcpy(target->steps, candidate->steps, sizeof(target->steps));
	target->step_count = candidate->step_count;
	target->active_step = candidate->active_step;
	*event_data = plan_registry_to_json(ctx->plans, "update_plan", target->id, target->name);
	if (!*event_data)
		return -ENOMEM;
	const cJSON *explanation = cJSON_GetObjectItemCaseSensitive(args, "explanation");
	if (explanation &&
	    !cJSON_AddStringToObject(*event_data, "explanation", explanation->valuestring))
		return -ENOMEM;
	cJSON *steps MORPH_CLEANUP(morph_json_cleanup) =
		cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(args, "plan"), 1);
	if (!steps)
		return -ENOMEM;
	int rc = morph_json_object_take(*event_data, "plan", &steps);
	if (rc == 0)
		rc = tool_result_success_text_copy(result, "Plan updated");
	if (rc == 0)
		rc = tool_result_finalize(result);
	if (rc == 0)
		rc = plan_registry_save(ctx->plans);
	return rc;
}

static int update_plan_exec(const char *arguments, struct tool_result *result, void *opaque)
{
	struct plan_tool_context *ctx = opaque;
	cJSON *args MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(arguments);
	cJSON *before MORPH_CLEANUP(morph_json_cleanup) = NULL;
	cJSON *event_data MORPH_CLEANUP(morph_json_cleanup) = NULL;
	struct plan *candidate MORPH_CLEANUP(update_plan_candidate_cleanup) =
		calloc(1, sizeof(*candidate));
	if (!ctx || !result || !ctx->plans)
		return -EINVAL;
	if (!candidate)
		return -ENOMEM;
	int rc = update_plan_parse(args, candidate);
	if (rc != 0) {
		(void)tool_result_error_copy(
			result, "invalid_arguments",
			"Provide plan as a complete list of nonempty steps with pending, "
			"in_progress or completed status, at most one in_progress step, and "
			"optional string explanation. Unknown fields and oversized lists "
			"or step text are not supported.");
		return rc;
	}
	pthread_mutex_lock(&ctx->mutex);
	before = plan_registry_json_create(ctx->plans);
	rc = before ? update_plan_apply(ctx, args, candidate, result, &event_data) : -ENOMEM;
	if (rc != 0 && before) {
		int restore_rc = plan_registry_restore(ctx->plans, before);
		if (restore_rc != 0)
			rc = restore_rc;
	}
	pthread_mutex_unlock(&ctx->mutex);
	if (rc != 0) {
		(void)tool_result_error_copy(result, "plan_update_failed", morph_strerror(rc));
		return rc;
	}
	const struct tool_runtime_context *runtime = result->runtime;
	if (runtime && runtime->event_cb) {
		struct morph_event event = {
			.type = MORPH_EVENT_BACKGROUND,
			.name = "plan.updated",
			.phase = "end",
			.message = "Plan updated",
			.data = event_data,
			.turn_id = runtime->turn_id,
		};
		(void)morph_event_emit(runtime->event_cb, runtime->event_user_data, &event);
	}
	return 0;
}

static int update_plan_register(struct tool_registry *reg, struct plan_tool_context *ctx)
{
	__atomic_add_fetch(&ctx->refs, 1, __ATOMIC_RELAXED);
	struct tool_spec spec MORPH_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "update_plan",
		.description = "Update the task checklist with a complete list of steps and "
			       "their pending, in_progress or completed status. At most one step "
			       "may "
			       "be in_progress. Include an optional explanation when the plan "
			       "changes.",
		.input_schema =
			"{\"type\":\"object\",\"properties\":{"
			"\"explanation\":{\"type\":\"string\"},\"plan\":{\"type\":\"array\","
			"\"maxItems\":32,\"items\":{\"type\":\"object\",\"properties\":{"
			"\"step\":{\"type\":\"string\"},\"status\":{\"type\":\"string\","
			"\"enum\":[\"pending\",\"in_progress\",\"completed\"]}},"
			"\"required\":[\"step\",\"status\"],\"additionalProperties\":false}}}"
			","
			"\"required\":[\"plan\"],\"additionalProperties\":false}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.flags = TOOL_FLAG_READONLY,
		.exec = update_plan_exec,
		.user_data = ctx,
		.user_data_destroy = plan_tool_context_destroy,
	};
	return tool_register_take(reg, &spec);
}
int plan_tool_init(struct tool_registry *reg, struct plan_registry *plans,
		   struct model *llm)
{
	if (!reg || !plans)
		return -EINVAL;

	struct plan_tool_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	int mutex_rc = pthread_mutex_init(&ctx->mutex, NULL);
	if (mutex_rc != 0) { free(ctx); return -mutex_rc; }
	ctx->refs = 1;
	ctx->tctx = NULL;
	ctx->plans = plans;
	ctx->llm = llm;

	struct tool_spec spec MORPH_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "plan",
		.description = "Create and manage multi-step plans. "
			       "Plans persist across compaction and resumed sessions. "
			       "Record exact request, constraints, acceptance criteria and "
			       "rejected approaches. "
			       "Before finishing, verify acceptance and record evidence/artifacts; "
			       "do not claim unverified completion. "
			       "Commands: verify (plan_id or plan, files, report, outcome) records "
			       "a report against file hashes. "
			       "Reuse current reports for unchanged files; stale reports require "
			       "rechecking. Checkpoint (plan_id or plan, request, constraints, "
			       "acceptance, rejected), create (name, goal, steps), "
			       "update (plan_id or plan, step_id, status), "
			       "get (plan_id or plan), list. "
			       "If 'goal' is provided without 'steps', the plan is "
			       "auto-decomposed into steps using AI.",
		.input_schema =
			"{\"type\":\"object\",\"properties\":{"
			"\"command\":{\"type\":\"string\",\"description\":\"create|update|get|"
			"list\","
			"\"enum\":[\"create\",\"update\",\"get\",\"list\",\"checkpoint\","
			"\"verify\"]},"
			"\"name\":{\"type\":\"string\",\"description\":\"Plan name (for create)\"},"
			"\"goal\":{\"type\":\"string\",\"description\":\"Plan goal/objective. If "
			"steps "
			"not provided, auto-decomposed into steps\"},"
			"\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
			"\"description\":\"Array of step descriptions (optional if goal "
			"provided)\"},"
			"\"plan_id\":{\"type\":\"string\",\"description\":\"Stable plan id to act "
			"on "
			"(preferred for update/get)\"},"
			"\"plan\":{\"type\":\"string\",\"description\":\"Plan name to act on (for "
			"update/get)\"},"
			"\"step_id\":{\"type\":\"integer\",\"description\":\"Step ID (for "
			"update)\"},"
			"\"files\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
			"\"report\":{\"type\":\"string\"},\"outcome\":{\"type\":\"string\","
			"\"enum\":["
			"\"passed\",\"failed\"]},"
			"\"request\":{\"type\":\"string\"},"
			"\"constraints\":{\"type\":\"string\"},"
			"\"acceptance\":{\"type\":\"string\"},"
			"\"rejected\":{\"type\":\"string\"},"
			"\"evidence\":{\"type\":\"string\"},"
			"\"blocker\":{\"type\":\"string\"},"
			"\"artifact\":{\"type\":\"string\"},"
			"\"status\":{\"type\":\"string\",\"description\":\"New status (for "
			"update): "
			"pending, in_progress, completed, failed, skipped\"}"
			"},\"required\":[\"command\"]}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = plan_tool_exec,
		.user_data = ctx,
		.user_data_destroy = plan_tool_context_destroy};
	int rc = tool_register_take(reg, &spec);
	return rc == 0 ? update_plan_register(reg, ctx) : rc;
}

int plan_tool_rebind(struct tool_registry *reg, struct plan_registry *plans,
		     struct model *llm)
{
	struct tool_entry *entry = reg ? tool_borrow(reg, "plan") : NULL;
	if (!entry || !plans)
		return -EINVAL;
	unsigned int flags = entry->flags;
	int timeout = entry->timeout_seconds;
	int disabled = tool_is_disabled(reg, "plan");
	struct tool_entry *update = tool_borrow(reg, "update_plan");
	unsigned int update_flags = update ? update->flags : TOOL_FLAG_READONLY;
	int update_timeout = update ? update->timeout_seconds : 0;
	int update_disabled = update ? tool_is_disabled(reg, "update_plan") : disabled;
	int rc = update ? tool_unregister(reg, "update_plan") : 0;
	if (rc == 0)
		rc = tool_unregister(reg, "plan");
	if (rc == 0)
		rc = plan_tool_init(reg, plans, llm);
	if (rc != 0)
		return rc;
	entry = tool_borrow(reg, "plan");
	entry->flags = flags;
	entry->timeout_seconds = timeout;
	update = tool_borrow(reg, "update_plan");
	update->flags = update_flags;
	update->timeout_seconds = update_timeout;
	plan_tool_set_context(reg, reg->permission_context);
	if (disabled)
		rc = tool_disable(reg, "plan");
	if (rc == 0 && update_disabled)
		rc = tool_disable(reg, "update_plan");
	return rc;
}
