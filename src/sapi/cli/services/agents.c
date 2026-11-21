#include "sapi/cli/services/agents.h"
#include <nb/err.h>
#include "sapi/cli/models/agents.h"

#include "sapi/cli/application.h"
#include "sapi/cli/models/jobs.h"
#include "sapi/cli/services/sessions.h"
#include "runtime/runtime.h"
#include <nb/cleanup.h>
#include <nb/str.h>
#include <nb/utf8.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct cli_agents_snapshot {
	struct agent_info *items;
	int count;
};

struct cli_agents_event_snapshot {
	char **items;
	int count;
};

static void cli_agents_snapshot_cleanup(struct cli_agents_snapshot *snapshot)
{
	if (snapshot->items)
		runtime_agent_list_free(snapshot->items, snapshot->count);

	snapshot->items = NULL;
	snapshot->count = 0;
}

static void cli_agents_event_snapshot_cleanup(struct cli_agents_event_snapshot *snapshot)
{
	if (snapshot->items)
		runtime_agent_events_free(snapshot->items, snapshot->count);

	snapshot->items = NULL;
	snapshot->count = 0;
}

static const char *cli_agents_text(const cJSON *json, const char *key)
{
	const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));

	return value ? value : "";
}

static int cli_agents_integer(const cJSON *json, const char *key, int64_t *out)
{
	const cJSON *field = cJSON_GetObjectItemCaseSensitive(json, key);

	*out = 0;
	if (!field)
		return 0;

	double value = cJSON_GetNumberValue(field);

	if (!cJSON_IsNumber(field) || !isfinite(value) || value < -0x1p63 ||
	    value >= 0x1p63 || trunc(value) != value)
		return NB_ERR_PROTOCOL;

	*out = (int64_t)value;
	return 0;
}

static int cli_agents_parse_status(const char *text, enum agent_status *out)
{
	if (!strcmp(text, "queued") || !strcmp(text, "pending"))
		*out = AGENT_PENDING;
	else if (!strcmp(text, "running"))
		*out = AGENT_RUNNING;
	else if (!strcmp(text, "completed"))
		*out = AGENT_COMPLETED;
	else if (!strcmp(text, "interrupted") || !strcmp(text, "cancelled"))
		*out = AGENT_CANCELLED;
	else if (!strcmp(text, "failed"))
		*out = AGENT_FAILED;
	else
		return NB_ERR_PROTOCOL;

	return 0;
}

int cli_agents_budget(struct cli_application *ctx, struct agent_budget *out)
{
	if (!ctx || !out)
		return NB_ERR_INVAL;

	if (!ctx->sessions || cli_sessions_current_id(ctx) <= 0)
		return runtime_agent_budget(ctx->runtime, out);

	cJSON *json NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = cli_sessions_request_current(ctx, "agent.budget", NULL, &json);
	if (rc != 0)
		return rc;

	struct agent_budget budget = {0};
	rc = cli_agents_integer(json, "used", &budget.used);
	if (rc == 0)
		rc = cli_agents_integer(json, "limit", &budget.limit);

	if (rc == 0)
		rc = cli_agents_integer(json, "request_tokens", &budget.request_tokens);

	if (rc == 0)
		rc = cli_agents_integer(json, "input_tokens", &budget.input_tokens);

	if (rc == 0)
		rc = cli_agents_integer(json, "cached_tokens", &budget.cached_tokens);

	if (rc == 0)
		rc = cli_agents_integer(json, "output_tokens", &budget.output_tokens);

	if (rc == 0)
		rc = cli_agents_integer(json, "reserved", &budget.reserved);

	if (rc != 0)
		return rc;

	budget.reminder_level = (int)cJSON_GetNumberValue(
		cJSON_GetObjectItem(json, "reminder_level"));
	budget.persistence_error = (int)cJSON_GetNumberValue(
		cJSON_GetObjectItem(json, "persistence_error"));
	budget.exhausted = cJSON_IsTrue(cJSON_GetObjectItem(json, "exhausted"));
	*out = budget;
	return 0;
}

static void cli_agents_copy_name(char *out, size_t capacity, const char *text)
{
	(void)nb_str_copy_to(nb_str_from_parts(text, nb_utf8_clamp_bytes(text, capacity - 1)),
		out, capacity);
}

static int cli_agents_parse_item(const cJSON *json, struct agent_info *item)
{
	const char *id = cli_agents_text(json, "agent_id");
	int64_t error = 0, iterations = 0;
	int rc = cli_agents_parse_status(cli_agents_text(json, "status"), &item->status);

	if (!id[0] || rc != 0)
		return NB_ERR_PROTOCOL;

	cli_agents_copy_name(item->id, sizeof(item->id), id);
	cli_agents_copy_name(item->agent_name, sizeof(item->agent_name),
		cli_agents_text(json, "name"));
	cli_agents_copy_name(item->mode, sizeof(item->mode), cli_agents_text(json, "mode"));
	item->description = strdup(cli_agents_text(json, "description"));
	item->activity = strdup(cli_agents_text(json, "activity"));
	item->result = strdup(cli_agents_text(json, "result"));
	if (!item->description || !item->activity || !item->result)
		return NB_ERR_NOMEM;

	rc = cli_agents_integer(json, "root_session_id", &item->parent_session_id);
	if (rc == 0)
		rc = cli_agents_integer(json, "session_id", &item->child_session_id);

	if (rc == 0)
		rc = cli_agents_integer(json, "started_at_ms", &item->started_at_ms);

	if (rc == 0)
		rc = cli_agents_integer(json, "ended_at_ms", &item->ended_at_ms);

	if (rc == 0)
		rc = cli_agents_integer(json, "error_code", &error);

	if (rc == 0)
		rc = cli_agents_integer(json, "iteration_count", &iterations);

	if (rc != 0 || error < INT_MIN || error > INT_MAX ||
	    iterations < 0 || iterations > INT_MAX)
		return NB_ERR_PROTOCOL;

	item->error_code = (int)error;
	item->iteration_count = (int)iterations;
	return 0;
}

static int cli_agents_protocol_snapshot(struct cli_application *ctx, const char *method,
	int64_t session, const char *id, struct agent_info **out, int *count)
{
	struct cli_agents_snapshot snapshot NB_CLEANUP(cli_agents_snapshot_cleanup) = {0};
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *response NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!out || !count)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	*count = 0;
	if (session <= 0)
		return id ? NB_ERR_NOT_FOUND : 0;

	if (!params || !cJSON_AddNumberToObject(params, "session_id", (double)session) ||
	    (id && !cJSON_AddStringToObject(params, "agent_id", id)))
		return NB_ERR_NOMEM;

	int rc = cli_sessions_request(ctx, method, params, &response);

	if (rc != 0)
		return rc;

	const cJSON *items = cJSON_GetObjectItemCaseSensitive(response, "agents");

	if (!cJSON_IsArray(items))
		return NB_ERR_PROTOCOL;

	snapshot.count = cJSON_GetArraySize(items);
	if (snapshot.count > 0) {
		snapshot.items = calloc((size_t)snapshot.count, sizeof(*snapshot.items));
		if (!snapshot.items)
			return NB_ERR_NOMEM;
	}

	for (int i = 0; i < snapshot.count; i++) {
		rc = cli_agents_parse_item(cJSON_GetArrayItem(items, i), &snapshot.items[i]);
		if (rc != 0)
			return rc;
	}

	*out = snapshot.items;
	*count = snapshot.count;
	snapshot.items = NULL;
	snapshot.count = 0;
	return 0;
}

static int cli_agents_protocol_action(struct cli_application *ctx, const char *method,
	const char *id, const char *message)
{
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *response NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!id)
		return NB_ERR_INVAL;

	if (!params || !cJSON_AddStringToObject(params, "agent_id", id) ||
	    (message && !cJSON_AddStringToObject(params, "message", message)))
		return NB_ERR_NOMEM;

	return cli_sessions_request_current(ctx, method, params, &response);
}

static void cli_agents_refresh_cleanup(struct cli_application **owner)
{
	(*owner)->agents_refreshing = 0;
}

int cli_agents_refresh(struct cli_application *ctx)
{
	struct agent_info *items = NULL;
	int count = 0;
	int64_t session = 0;
	int rc;

	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;

	if (ctx->agents_refreshing)
		return 0;

	ctx->agents_refreshing = 1;
	struct cli_application *owner NB_CLEANUP(cli_agents_refresh_cleanup) = ctx;

	if (ctx->sessions) {
		session = cli_sessions_current_id(ctx);
	} else {
		rc = runtime_session_current_id(ctx->runtime, &session);
		if (rc != 0)
			return rc;
	}

	rc = cli_agents_activity(ctx, session, &items, &count);
	if (rc != 0)
		return rc;

	if (ctx->sessions && cli_sessions_current_id(ctx) != session) {
		runtime_agent_list_free(items, count);
		return 0;
	}

	cli_agents_model_set(&ctx->agents, items, count, session);
	cli_jobs_model_set_session(ctx->jobs, session);
	return 0;
}

int cli_agents_list(struct cli_application *ctx, int64_t session, struct agent_info **items,
		    int *count)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;

	if (ctx->sessions && (session > 0 || cli_sessions_current_id(ctx) > 0))
		return cli_agents_protocol_snapshot(ctx, "agent.list",
			session > 0 ? session : cli_sessions_current_id(ctx), NULL, items, count);

	return runtime_agent_list(ctx->runtime, session, items, count);
}

int cli_agents_activity(struct cli_application *ctx, int64_t session, struct agent_info **items,
			int *count)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;

	if (ctx->sessions && (session > 0 || cli_sessions_current_id(ctx) > 0))
		return cli_agents_protocol_snapshot(ctx, "agent.activity",
			session > 0 ? session : cli_sessions_current_id(ctx), NULL, items, count);

	return runtime_agent_activity(ctx->runtime, session, items, count);
}

int cli_agents_read(struct cli_application *ctx, const char *id, struct agent_info **out)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;

	if (ctx->sessions) {
		int count = 0;
		int rc = cli_agents_protocol_snapshot(ctx, "agent.read",
			cli_sessions_current_id(ctx), id, out, &count);

		if (rc == 0 && count != 1) {
			runtime_agent_list_free(*out, count);
			*out = NULL;
			return NB_ERR_PROTOCOL;
		}

		return rc;
	}

	return runtime_agent_read(ctx->runtime, id, out);
}

int cli_agents_subscribe(struct cli_application *ctx, const char *id, char ***events,
			 int *count, int64_t *sequence)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;

	if (ctx->sessions) {
		struct cli_agents_event_snapshot snapshot
			NB_CLEANUP(cli_agents_event_snapshot_cleanup) = {0};
		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *response NB_CLEANUP(cJSON_DeleteOwner) = NULL;

		if (!events || !count || !sequence || !id)
			return NB_ERR_INVAL;

		if (*events)
			return NB_ERR_EXISTS;

		*count = 0;
		*sequence = 0;
		if (!params || !cJSON_AddStringToObject(params, "agent_id", id))
			return NB_ERR_NOMEM;

		int rc = cli_sessions_request_current(ctx, "agent.subscribe", params, &response);

		if (rc != 0)
			return rc;

		const cJSON *items = cJSON_GetObjectItemCaseSensitive(response, "events");
		int64_t frontier = 0;

		if (!cJSON_IsArray(items) ||
		    cli_agents_integer(response, "sequence", &frontier) != 0 || frontier < 0)
			return NB_ERR_PROTOCOL;

		snapshot.count = cJSON_GetArraySize(items);
		if (snapshot.count > 0) {
			snapshot.items = calloc((size_t)snapshot.count, sizeof(*snapshot.items));
			if (!snapshot.items)
				return NB_ERR_NOMEM;
		}

		for (int i = 0; i < snapshot.count; i++) {
			const char *text = cJSON_GetStringValue(cJSON_GetArrayItem(items, i));

			if (!text)
				return NB_ERR_PROTOCOL;

			snapshot.items[i] = strdup(text);
			if (!snapshot.items[i])
				return NB_ERR_NOMEM;
		}

		*events = snapshot.items;
		*count = snapshot.count;
		*sequence = frontier;
		snapshot.items = NULL;
		snapshot.count = 0;
		return 0;
	}

	return runtime_agent_subscribe(ctx->runtime, id, events, count, sequence);
}

int cli_agents_select(struct cli_application *ctx, const char *id)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;

	if (ctx->sessions)
		return cli_sessions_current_id(ctx) > 0
			? cli_agents_protocol_action(ctx, "agent.select", id ? id : "", NULL) : 0;

	return runtime_agent_select(ctx->runtime, id);
}

int cli_agents_interrupt(struct cli_application *ctx, const char *id)
{
	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;

	if (ctx->sessions)
		return cli_agents_protocol_action(ctx, "agent.interrupt", id, NULL);

	return runtime_agent_interrupt(ctx->runtime, id);
}

int cli_agents_followup(struct cli_application *ctx, const char *id, const char *message)
{
	if (!ctx || !ctx->runtime || !message)
		return NB_ERR_INVAL;

	if (ctx->sessions)
		return cli_agents_protocol_action(ctx, "agent.followup", id, message);

	return runtime_agent_followup(ctx->runtime, id, message);
}

void cli_agents_list_free(struct agent_info *items, int count)
{
	runtime_agent_list_free(items, count);
}

void cli_agents_events_free(char **items, int count)
{
	runtime_agent_events_free(items, count);
}
