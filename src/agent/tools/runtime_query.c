#include "runtime_query.h"
#include "agent/memory.h"
#include "agent/tool_runtime.h"
#include "credits.h"
#include "util/error.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void summary_to_json(cJSON *parent, const char *name,
			    const struct credit_summary *s)
{
	cJSON *obj;

	if (!parent || !name || !s)
		return;
	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddNumberToObject(obj, "credits", (double)s->credits);
	cJSON_AddNumberToObject(obj, "estimated_cost", s->estimated_cost);
	cJSON_AddNumberToObject(obj, "event_count", s->event_count);
	cJSON_AddItemToObject(parent, name, obj);
}

static int credits_exec(const char *args_json, struct tool_result *result,
			void *user_data)
{
	const struct tool_runtime_context *rt = tool_runtime_get_current();
	struct credit_summary today;
	struct credit_summary session;
	struct credit_summary total;
	cJSON *root;
	char *json;
	int rc;

	(void)args_json;
	(void)user_data;
	if (!rt || !rt->db || !rt->config || !rt->user_id ||
	    !rt->credit_session_id)
		return tool_result_json_error(result,
					      "runtime context is unavailable");

	rc = credit_summary_today(rt->db, rt->user_id, &today);
	if (rc != 0)
		return rc;
	rc = credit_summary_session(rt->db, rt->credit_session_id, &session);
	if (rc != 0)
		return rc;
	rc = credit_summary_total(rt->db, rt->user_id, &total);
	if (rc != 0)
		return rc;

	root = cJSON_CreateObject();
	if (!root)
		MORPH_RETURN(-ENOMEM);
	summary_to_json(root, "today", &today);
	summary_to_json(root, "session", &session);
	summary_to_json(root, "total", &total);
	cJSON_AddStringToObject(root, "currency", rt->config->credits.currency);
	cJSON_AddNumberToObject(root, "daily_limit",
				rt->config->credits.daily_limit);
	cJSON_AddBoolToObject(root, "over_daily_limit",
			      rt->config->credits.daily_limit >= 0 &&
				      today.credits >
					      rt->config->credits.daily_limit);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	return tool_result_take_json(result, json);
}

static enum memory_query_type parse_memory_type(const char *s)
{
	if (!s || !*s || strcmp(s, "all") == 0)
		return MEMORY_QUERY_ALL;
	if (strcmp(s, "profile") == 0)
		return MEMORY_QUERY_PROFILE;
	if (strcmp(s, "facts") == 0)
		return MEMORY_QUERY_FACTS;
	if (strcmp(s, "procedures") == 0 || strcmp(s, "rules") == 0)
		return MEMORY_QUERY_PROCEDURES;
	if (strcmp(s, "episodes") == 0)
		return MEMORY_QUERY_EPISODES;
	if (strcmp(s, "changes") == 0)
		return MEMORY_QUERY_CHANGES;
	return MEMORY_QUERY_ALL;
}

static void parse_memory_query(const char *args_json,
			       struct memory_query_options *q)
{
	cJSON *root;
	cJSON *item;

	memset(q, 0, sizeof(*q));
	q->type = MEMORY_QUERY_ALL;
	q->scope_all = 1;
	q->max_episodes = 0;
	if (!args_json || !*args_json)
		return;
	root = cJSON_Parse(args_json);
	if (!root)
		return;
	item = cJSON_GetObjectItemCaseSensitive(root, "type");
	if (cJSON_IsString(item))
		q->type = parse_memory_type(cJSON_GetStringValue(item));
	item = cJSON_GetObjectItemCaseSensitive(root, "scope");
	if (cJSON_IsString(item) &&
	    strcmp(cJSON_GetStringValue(item), "session") == 0)
		q->scope_all = 0;
	item = cJSON_GetObjectItemCaseSensitive(root, "max_episodes");
	if (cJSON_IsNumber(item) && item->valueint > 0)
		q->max_episodes = item->valueint;
	cJSON_Delete(root);
}

static int memory_exec(const char *args_json, struct tool_result *result,
		       void *user_data)
{
	const struct tool_runtime_context *rt = tool_runtime_get_current();
	struct memory_query_options q;
	cJSON *root;
	char *json;
	char *text;

	(void)user_data;
	if (!rt || !rt->db)
		return tool_result_json_error(result,
					      "runtime context is unavailable");
	parse_memory_query(args_json, &q);
	if (rt->restrict_memory_to_user) {
		q.user_id = rt->user_id;
		q.visible_fn = rt->memory_visible_fn;
		q.visible_user_data = rt->memory_visible_user_data;
	}
	text = memory_query_render(rt->db, rt->memory_session_id, &q);
	if (!text)
		MORPH_RETURN(-ENOMEM);

	root = cJSON_CreateObject();
	if (!root) {
		free(text);
		MORPH_RETURN(-ENOMEM);
	}
	cJSON_AddStringToObject(root, "scope", q.scope_all ? "all" : "session");
	cJSON_AddStringToObject(root, "text", text);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	free(text);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	return tool_result_take_json(result, json);
}

int runtime_query_tools_init(struct tool_registry *reg)
{
	int rc;
	struct tool_entry *e;

	if (!reg)
		MORPH_RETURN(-EINVAL);
	rc = tool_register(reg, "credits",
			   "Query current credit usage, limits, and totals.",
			   "{}", credits_exec, NULL, NULL);
	if (rc != 0)
		return rc;
	e = tool_lookup(reg, "credits");
	if (e)
		e->flags |= TOOL_FLAG_READONLY;

	rc = tool_register(reg, "memory",
		"Query long-term memory by type and scope.",
		"{\"type\":\"all|profile|facts|procedures|episodes|changes\","
		"\"scope\":\"all|session\",\"max_episodes\":0}",
		memory_exec, NULL, NULL);
	if (rc != 0)
		return rc;
	e = tool_lookup(reg, "memory");
	if (e)
		e->flags |= TOOL_FLAG_READONLY;
	return 0;
}
