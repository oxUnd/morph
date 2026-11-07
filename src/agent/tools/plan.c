#include "plan.h"
#include "agent/tool.h"
#include "agent/plan.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/json.h"
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
	pthread_mutex_t mutex;
	struct plan_registry *plans;
	struct tool_context *tctx;
	struct model *llm;
};

static void plan_tool_context_destroy(void *user_data)
{
	struct plan_tool_context *ctx = user_data;
	if (ctx) pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
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

static struct json *plan_to_json(struct plan *p)
{
	if (!p)
		return NULL;

	struct json *obj = json_object();
	if (!obj)
		return NULL;

	struct json *steps = json_array();
	if (!steps)
		goto fail;

	if (!json_add_string(obj, "id", p->id) || !json_add_string(obj, "name", p->name) ||
	    !json_add_string(obj, "goal", p->goal) ||
	    !json_add_string(obj, "request", p->request) ||
	    !json_add_string(obj, "constraints", p->constraints) ||
	    !json_add_string(obj, "acceptance", p->acceptance) ||
	    !json_add_string(obj, "rejected", p->rejected) ||
	    !json_add_string(obj, "verification", p->verification) ||
	    !json_add_number(obj, "step_count", p->step_count) ||
	    !json_add_number(obj, "active_step", p->active_step) ||
	    !json_add_bool(obj, "all_done", plan_all_done(p)))
		goto fail;

	for (int i = 0; i < p->step_count; i++) {
		struct plan_step *s = &p->steps[i];
		struct json *step = json_object();
		if (!step)
			goto fail;
		if (!json_add_number(step, "id", s->id) ||
		    !json_add_string(step, "description", s->description) ||
		    !json_add_string(step, "status", s->status) ||
		    !json_add_string(step, "evidence", s->evidence) ||
		    !json_add_string(step, "blocker", s->blocker) ||
		    !json_add_string(step, "artifact", s->artifact) ||
		    !json_add_bool(step, "active", i == p->active_step && p->active_step >= 0)) {
			json_free(step);
			goto fail;
		}
		json_array_add(steps, step);
	}

	json_object_add(obj, "steps", steps);
	return obj;

fail:
	json_free(steps);
	json_free(obj);
	return NULL;
}

static struct json *plan_registry_to_json(struct plan_registry *reg, const char *command,
					  const char *selected_plan_id, const char *selected_plan)
{
	struct json *root = json_object();
	if (!root)
		return NULL;

	struct json *plans = json_array();
	if (!plans)
		goto fail;

	if (!json_add_string(root, "kind", "plan_state") ||
	    !json_add_string(root, "command", command ? command : "") ||
	    !json_add_number(root, "count", reg ? reg->count : 0))
		goto fail;

	if (selected_plan_id && !json_add_string(root, "selected_plan_id", selected_plan_id))
		goto fail;

	if (selected_plan && !json_add_string(root, "selected_plan", selected_plan))
		goto fail;

	if (reg) {
		for (int i = 0; i < reg->count; i++) {
			struct json *item = plan_to_json(&reg->plans[i]);
			if (!item)
				goto fail;
			json_array_add(plans, item);
		}
	}

	json_object_add(root, "plans", plans);
	return root;

fail:
	json_free(plans);
	json_free(root);
	return NULL;
}

static struct json *plan_ui_to_json(struct json *data)
{
	struct json *ui = json_object();
	struct json *summary = NULL;
	struct json *item;

	if (!ui)
		return NULL;

	if (!json_add_string(ui, "component", "plan") || !json_add_string(ui, "version", "1"))
		goto fail;

	summary = json_object();
	if (!summary)
		goto fail;
	if (data) {
		item = json_member_ci(data, "kind");
		if (json_is_string_value(item) &&
		    !json_add_string(summary, "kind", json_string_value(item)))
			goto fail;
		item = json_member_ci(data, "command");
		if (json_is_string_value(item) &&
		    !json_add_string(summary, "command", json_string_value(item)))
			goto fail;
		item = json_member_ci(data, "count");
		if (json_is_number_value(item) &&
		    !json_add_number(summary, "count", json_number_value(item)))
			goto fail;
		item = json_member_ci(data, "selected_plan_id");
		if (json_is_string_value(item) &&
		    !json_add_string(summary, "selected_plan_id", json_string_value(item)))
			goto fail;
		item = json_member_ci(data, "selected_plan");
		if (json_is_string_value(item) &&
		    !json_add_string(summary, "selected_plan", json_string_value(item)))
			goto fail;
	}
	json_object_add(ui, "data", summary);
	summary = NULL;
	return ui;

fail:
	json_free(summary);
	json_free(ui);
	return NULL;
}

static int attach_plan_state(struct tool_result *result,
			     struct plan_registry *reg,
			     const char *command,
			     const char *selected_plan_id,
			     const char *selected_plan)
{
	struct json *data = plan_registry_to_json(reg, command, selected_plan_id, selected_plan);
	if (!data)
		return -ENOMEM;

	struct json *ui = plan_ui_to_json(data);
	if (!ui) {
		json_free(data);
		return -ENOMEM;
	}

	tool_result_clear(result);
	result->data = data;
	result->ui = ui;
	return tool_result_finalize(result);
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

	return tool_result_success_json_text(result, morph_buf_detach(&buf));
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
	if (!llm || !llm->chat || !llm->api_key[0])
		return MORPH_ERR_NOT_CONFIGURED;

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
	struct tool_runtime_stream_sink stream;
	int rc2 = morph_buf_init(&buf, 8192);
	if (rc2 != 0) {
		arena_destroy(arena);
		return rc2;
	}

	struct model request_model;

	llm = model_bind_http_request(llm, runtime ? &runtime->http : NULL, &request_model);
	const char *messages[] = { prompt };
	stream.runtime = runtime;
	stream.tool = "plan";
	stream.kind = "text";
	stream.buf = &buf;
	int status = llm->chat(llm, arena, sys, messages, 1, NULL,
			       tool_runtime_stream_to_buf_cb, &stream);
	arena_destroy(arena);

	if (status < 0 || buf.len == 0) {
		morph_buf_cleanup(&buf);
		return MORPH_ERR_LLM;
	}

	char *text = morph_buf_detach(&buf);
	if (!text)
		return -ENOMEM;
	int count = parse_steps_from_text(text, step_descs, max_steps);
	free(text);
	return count;
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

	struct json *root = json_parse_prefix(args_json, NULL, NULL);
	if (!root) {
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	struct json *cmd = json_member_ci(root, "command");
	if (!json_is_string_value(cmd)) {
		json_free(root);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing 'command' parameter. "
			"Commands: create, update, get, list\"}"));
		return -EINVAL;
	}

	const char *command = json_string_value(cmd);
	int rc = 0;

	if (strcmp(command, "create") == 0) {
		struct json *name_item = json_member_ci(root, "name");
		struct json *goal_item = json_member_ci(root, "goal");
		struct json *steps_item = json_member_ci(root, "steps");

		if (!json_is_string_value(name_item) || !json_string_value(name_item)) {
			json_free(root);
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"create requires 'name' (string)\"}"));
			return -EINVAL;
		}

		const char *goal =
			json_is_string_value(goal_item) ? json_string_value(goal_item) : "";

		int step_count = 0;
		const char *step_descs[PLAN_MAX_STEPS];
		memset(step_descs, 0, sizeof(step_descs));
		int auto_decomposed = 0;

		if (json_is_array_value(steps_item) && json_count(steps_item) > 0) {
			step_count = json_count(steps_item);
			if (step_count > PLAN_MAX_STEPS)
				step_count = PLAN_MAX_STEPS;
			for (int i = 0; i < step_count; i++) {
				struct json *s = json_at(steps_item, i);
				step_descs[i] = json_is_string_value(s) ? json_string_value(s) : "";
			}
		} else if (goal && goal[0]) {
			/* Auto-decompose goal into steps using LLM */
			const char *tmp_descs[PLAN_MAX_STEPS];
			memset(tmp_descs, 0, sizeof(tmp_descs));
			rc = auto_decompose(result->runtime, ctx->llm, goal, tmp_descs,
					    PLAN_MAX_STEPS);
			if (rc < 0) {
				free_step_descs(tmp_descs, PLAN_MAX_STEPS);
				json_free(root);
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"auto-decompose failed\"}"));
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
			json_free(root);
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"create requires 'steps' (array of strings) "
				"or a 'goal' string to auto-decompose\"}"));
			return -EINVAL;
		}

		struct plan *p = plan_create(ctx->plans, json_string_value(name_item), goal,
					     step_descs, step_count);

		if (!p) {
			if (auto_decomposed)
				free_step_descs(step_descs, step_count);
			json_free(root);
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"failed to create plan "
				"(max 8 plans, max 32 steps each)\"}"));
			return -ENOSPC;
		}

		char *formatted = format_plan(p);
		if (!formatted) {
			if (auto_decomposed)
				free_step_descs(step_descs, step_count);
			json_free(root);
			(void)tool_result_success_json_text(result, strdup("{\"error\":\"out of memory\"}"));
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
			json_free(root);
			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command, p->id,
				       p->name);
		if (rc != 0) {
			json_free(root);
			return rc;
		}
		log_info(result && result->runtime ? result->runtime->http.logger : NULL,
			 "plan: created '%s' with %d steps%s", p->name, p->step_count,
			 auto_decomposed ? " (auto)" : "");

	} else if (strcmp(command, "update") == 0) {
		struct json *plan_id_item = json_member_ci(root, "plan_id");
		struct json *plan_item = json_member_ci(root, "plan");
		struct json *step_id_item = json_member_ci(root, "step_id");
		struct json *status_item = json_member_ci(root, "status");

		const char *plan_id =
			json_is_string_value(plan_id_item) ? json_string_value(plan_id_item) : NULL;
		const char *plan_name =
			json_is_string_value(plan_item) ? json_string_value(plan_item) : NULL;
		int step_id = json_is_number_value(step_id_item)
				      ? (int)json_number_value(step_id_item)
				      : -1;
		const char *status =
			json_is_string_value(status_item) ? json_string_value(status_item) : NULL;

		if ((!plan_id && !plan_name) || step_id < 0 || !status) {
			json_free(root);
			(void)tool_result_success_json_text(result, strdup(
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
			(void)tool_result_success_json_text(result, strdup(err));
			json_free(root);
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
			json_free(root);
			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command,
				       p ? p->id : plan_id,
				       p ? p->name : plan_name);
		if (rc != 0) {
			json_free(root);
			return rc;
		}
		log_info(result && result->runtime ? result->runtime->http.logger : NULL,
			 "plan: updated '%s' step %d -> %s",
			 p ? p->id : (plan_id ? plan_id : plan_name), step_id, status);

	} else if (strcmp(command, "get") == 0) {
		struct json *plan_id_item = json_member_ci(root, "plan_id");
		struct json *plan_item = json_member_ci(root, "plan");
		const char *plan_id =
			json_is_string_value(plan_id_item) ? json_string_value(plan_id_item) : NULL;

		if (plan_id || (json_is_string_value(plan_item) && json_string_value(plan_item))) {
			struct plan *p =
				plan_id ? plan_find_by_id(ctx->plans, plan_id)
					: plan_find(ctx->plans, json_string_value(plan_item));
			if (!p) {
				rc = set_resultf(result, "Plan \"%s\" not found.",
						 plan_id ? plan_id : json_string_value(plan_item));
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
			json_free(root);
			return rc;
		}
		rc = attach_plan_state(
			result, ctx->plans, command, plan_id,
			json_is_string_value(plan_item) ? json_string_value(plan_item) : NULL);
		if (rc != 0) {
			json_free(root);
			return rc;
		}

	} else if (strcmp(command, "list") == 0) {
		char *formatted = format_plans(ctx->plans);
		rc = set_resultf(result, "%s",
				 formatted ? formatted : "(empty)");
		free(formatted);
		if (rc != 0) {
			json_free(root);
			return rc;
		}
		rc = attach_plan_state(result, ctx->plans, command, NULL, NULL);
		if (rc != 0) {
			json_free(root);
			return rc;
		}

	} else {
		rc = set_resultf(result,
			"{\"error\":\"unknown command '%s'. "
			"Commands: create, update, get, list\"}",
			command);
		json_free(root);
		return rc != 0 ? rc : -EINVAL;
	}

	json_free(root);
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

static int plan_capture_report(struct plan_tool_context *ctx, const struct json *args,
			       struct json *selected)
{
	const struct json *files = json_member(args, "files");
	const char *report = json_string_value(json_member(args, "report"));
	const char *outcome = json_string_value(json_member(args, "outcome"));
	struct json *state;
	struct json *inputs;
	int rc = 0;
	if (!json_is_array_value(files) || json_count(files) < 1 || json_count(files) > 16 ||
	    !report || !outcome || (strcmp(outcome, "passed") && strcmp(outcome, "failed")))
		return -EINVAL;
	state = json_object();
	inputs = state ? json_add_array(state, "inputs") : NULL;
	if (!inputs || !json_add_string(state, "status", "current") ||
	    !json_add_string(state, "outcome", outcome) ||
	    !json_add_string(state, "report", report)) {
		json_free(state);
		return -ENOMEM;
	}
	/* Include the report itself so replacing its contents invalidates reuse. */
	for (int i = -1; i < json_count(files); i++) {
		const char *path = i < 0 ? report : json_string_value(json_at(files, i));
		char resolved[PATH_MAX];
		char hex[65];
		rc = plan_file_digest(ctx, path, resolved, hex);
		if (rc != 0)
			break;
		struct json *input = json_object();
		if (!input) {
			rc = -ENOMEM;
			break;
		}
		json_array_add(inputs, input);
		if (!json_add_string(input, "path", resolved) ||
		    !json_add_string(input, "sha256", hex)) {
			rc = -ENOMEM;
			break;
		}
	}
	char *text = rc == 0 ? json_stringify(state) : NULL;
	if (rc == 0 && (!text || strlen(text) >= sizeof(ctx->plans->plans[0].verification)))
		rc = text ? -E2BIG : -ENOMEM;
	if (rc == 0) {
		struct json *value = json_string(text);
		if (!value || json_object_replace(selected, "verification", value) != 0) {
			json_free(value);
			rc = -ENOMEM;
		}
	}
	free(text);
	json_free(state);
	return rc;
}

void plan_tool_set_context(struct tool_registry *reg, struct tool_context *tctx)
{
	struct tool_entry *entry = tool_lookup(reg, "plan");
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
		struct json *state = json_parse_prefix(p->verification, NULL, NULL);
		const char *status = json_string_value(json_member_ci(state, "status"));
		if (!status || strcmp(status, "current")) {
			json_free(state);
			continue;
		}
		struct json *input;
		int stale = 0;
		json_foreach (input, json_member_ci(state, "inputs")) {
			const char *path = json_string_value(json_member_ci(input, "path"));
			const char *expected = json_string_value(json_member_ci(input, "sha256"));
			char resolved[PATH_MAX];
			char hex[65];
			if (!expected || plan_file_digest(ctx, path, resolved, hex) != 0 ||
			    strcmp(expected, hex)) {
				stale = 1;
				break;
			}
		}
		if (stale) {
			json_object_replace(state, "status", json_string("stale"));
			char *text = json_stringify(state);
			if (!text) {
				json_free(state);
				return -ENOMEM;
			}
			snprintf(p->verification, sizeof(p->verification), "%s", text);
			free(text);
			changed = 1;
		}
		json_free(state);
	}
	return changed ? plan_registry_save(ctx->plans) : 0;
}

int plan_tool_refresh_reports(struct tool_registry *reg)
{
	struct tool_entry *entry = reg ? tool_lookup(reg, "plan") : NULL;
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
	struct json *args = json_parse_prefix(args_json, NULL, NULL);
	struct json *before;
	struct json *state;
	const char *command = json_string_value(json_member_ci(args, "command"));
	int rc;
	if (!ctx || !ctx->plans || !command) {
		json_free(args);
		return -EINVAL;
	}
	rc = plan_refresh_reports(ctx);
	if (rc != 0) {
		json_free(args);
		return rc;
	}
	before = plan_registry_json(ctx->plans);
	if (!before) {
		json_free(args);
		return -ENOMEM;
	}
	if (!strcmp(command, "checkpoint") || !strcmp(command, "verify"))
		rc = 0;
	else
		rc = plan_tool_exec_inner(args_json, result, user_data);
	state = rc == 0 ? plan_registry_json(ctx->plans) : NULL;
	if (rc == 0 && !state)
		rc = -ENOMEM;
	if (rc == 0 && (!strcmp(command, "checkpoint") || !strcmp(command, "create") ||
			!strcmp(command, "update") || !strcmp(command, "verify"))) {
		const char *id = json_string_value(json_member_ci(args, "plan_id"));
		const char *name = json_string_value(
			json_member_ci(args, !strcmp(command, "create") ? "name" : "plan"));
		struct json *selected = NULL;
		struct json *entry;
		json_foreach (entry, state) {
			const char *entry_id = json_string_value(json_member_ci(entry, "id"));
			const char *entry_name = json_string_value(json_member_ci(entry, "name"));
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
			struct json *value = json_member(args, fields[i]);
			if (!value)
				continue;
			struct json *copy = json_clone(value, 1);
			if (!copy || json_object_replace(selected, fields[i], copy) != 0) {
				json_free(copy);
				rc = -ENOMEM;
				break;
			}
		}
		struct json *step_id = json_member_ci(args, "step_id");
		if (selected && json_is_number_value(step_id)) {
			struct json *step = json_at(json_member_ci(selected, "steps"),
						    json_int_value(step_id) - 1);
			const char *step_fields[] = {"evidence", "blocker", "artifact"};
			if (!step)
				rc = -ENOENT;
			for (size_t i = 0; step && i < sizeof(step_fields) / sizeof(step_fields[0]);
			     i++) {
				struct json *value = json_member(args, step_fields[i]);
				if (!value)
					continue;
				struct json *copy = json_clone(value, 1);
				if (!copy || json_object_replace(step, step_fields[i], copy) != 0) {
					json_free(copy);
					rc = -ENOMEM;
					break;
				}
			}
		}
		if (rc == 0)
			rc = plan_registry_restore(ctx->plans, state);
		if (rc == 0)
			rc = attach_plan_state(result, ctx->plans, command,
					       json_string_value(json_member_ci(selected, "id")),
					       name);
		if (rc == 0)
			rc = plan_registry_save(ctx->plans);
	}
	if (rc != 0) {
		(void)plan_registry_restore(ctx->plans, before);
		(void)tool_result_error(result, "plan_checkpoint_failed", morph_strerror(rc));
	}
	json_free(state);
	json_free(before);
	json_free(args);
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
	ctx->tctx = NULL;
	ctx->plans = plans;
	ctx->llm = llm;

	int rc = tool_register(
	    reg,
	    &(struct tool_spec){
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
		    "\"command\":{\"type\":\"string\",\"description\":\"create|update|get|list\","
		    "\"enum\":[\"create\",\"update\",\"get\",\"list\",\"checkpoint\",\"verify\"]},"
		    "\"name\":{\"type\":\"string\",\"description\":\"Plan name (for create)\"},"
		    "\"goal\":{\"type\":\"string\",\"description\":\"Plan goal/objective. If steps "
		    "not provided, auto-decomposed into steps\"},"
		    "\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
		    "\"description\":\"Array of step descriptions (optional if goal provided)\"},"
		    "\"plan_id\":{\"type\":\"string\",\"description\":\"Stable plan id to act on "
		    "(preferred for update/get)\"},"
		    "\"plan\":{\"type\":\"string\",\"description\":\"Plan name to act on (for "
		    "update/get)\"},"
		    "\"step_id\":{\"type\":\"integer\",\"description\":\"Step ID (for update)\"},"
		    "\"files\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
		    "\"report\":{\"type\":\"string\"},\"outcome\":{\"type\":\"string\",\"enum\":["
		    "\"passed\",\"failed\"]},"
		    "\"request\":{\"type\":\"string\"},"
		    "\"constraints\":{\"type\":\"string\"},"
		    "\"acceptance\":{\"type\":\"string\"},"
		    "\"rejected\":{\"type\":\"string\"},"
		    "\"evidence\":{\"type\":\"string\"},"
		    "\"blocker\":{\"type\":\"string\"},"
		    "\"artifact\":{\"type\":\"string\"},"
		    "\"status\":{\"type\":\"string\",\"description\":\"New status (for update): "
		    "pending, in_progress, completed, failed, skipped\"}"
		    "},\"required\":[\"command\"]}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = plan_tool_exec,
		.user_data = ctx,
		.user_data_destroy = plan_tool_context_destroy});
	if (rc != 0)
		plan_tool_context_destroy(ctx);
	return rc;
}
