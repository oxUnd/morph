#include "agent/tool_runtime.h"
#include "skill_activate.h"
#include "util/log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/error.h"

static int skill_activate_exec(const char *args_json, struct tool_result *result,
			       void *user_data)
{
	struct skill_registry *skills = user_data;

	if (!result)
		return -EINVAL;
	if (!skills) {
		(void)tool_result_success_json_text(result, strdup("{\"error\":\"skill system not initialized\"}"));
		MORPH_RETURN(MORPH_ERR_NOT_INITIALIZED);
	}

	char name[SKILL_NAME_MAX] = {0};

	if (args_json && *args_json) {
		cJSON *root = cJSON_Parse(args_json);
		if (root) {
			cJSON *name_item = cJSON_GetObjectItem(root, "name");
			if (cJSON_IsString(name_item) && name_item->valuestring)
				strncpy(name, name_item->valuestring,
					sizeof(name) - 1);
			cJSON_Delete(root);
		}
	}

	if (name[0] == '\0') {
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing or empty 'name' parameter. "
			"Usage: activate_skill({\\\"name\\\": \\\"skill-name\\\"})\"}"));
		return -EINVAL;
	}

	struct skill_entry *skill = skill_lookup(skills, name);
	if (!skill) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"skill '%s' not found\"}", name);
		(void)tool_result_success_json_text(result, strdup(err));
		return -ENOENT;
	}

	if (!skill->enabled) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"skill '%s' is disabled\"}", name);
		(void)tool_result_success_json_text(result, strdup(err));
		return -EACCES;
	}

	if (skill->activated && skill->body_loaded) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			 "Skill '%s' is already active. Its instructions are in context.",
			 name);
		(void)tool_result_success_json_text(result, strdup(msg));
		return 0;
	}

	int rc = skill_activate(skill);
	if (rc < 0) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"failed to activate skill '%s' (code %d)\"}",
			 name, rc);
		(void)tool_result_success_json_text(result, strdup(err));
		return rc;
	}

	if (!skill->body || !skill->body[0]) {
		(void)tool_result_success_json_text(result, strdup(
			"Skill activated but contains no instructions."));
		return 0;
	}

	char *message = strdup(
		"Skill activated. Its full instructions are supplied in the "
		"active skill context on the next model request.");
	if (!message)
		MORPH_RETURN(-ENOMEM);
	(void)tool_result_success_json_text(result, message);
	log_info(result && result->runtime ? result->runtime->http.logger : NULL,
		 "skill_activate: '%s' activated", name);
	return 0;
}

static int skill_search_exec(const char *args, struct tool_result *result, void *user_data)
{
	struct skill_registry *skills = user_data;
	cJSON *input = args ? cJSON_ParseWithOpts(args, NULL, 1) : NULL;
	const char *query = cJSON_GetStringValue(cJSON_GetObjectItem(input, "query"));
	cJSON *offset_json = cJSON_GetObjectItem(input, "offset");
	int offset = cJSON_IsNumber(offset_json) ? offset_json->valueint : 0;

	if (!query || offset < 0 || (offset_json && (!cJSON_IsNumber(offset_json) ||
	    offset_json->valuedouble != (double)offset))) {
		cJSON_Delete(input);
		return tool_result_error(result, "invalid_arguments",
			"query is required and offset must be a non-negative integer");
	}
	const struct skill_entry *entries[SKILL_MAX_ENTRIES];
	int rc = skill_sorted_entries(skills, entries, SKILL_MAX_ENTRIES);
	cJSON *data = cJSON_CreateObject();
	cJSON *matches = cJSON_AddArrayToObject(data, "skills");
	int total = 0;
	int count = 0;

	if (rc >= 0 && (!data || !matches))
		rc = -ENOMEM;
	for (int i = 0; rc >= 0 && i < skills->count; i++) {
		const struct skill_entry *entry = entries[i];
		if (!entry->enabled || (query[0] && !strcasestr(entry->fm.name, query) &&
		    !strcasestr(entry->fm.description, query)))
			continue;
		if (total++ < offset || count >= 8)
			continue;
		cJSON *item = cJSON_CreateObject();
		if (!item || !cJSON_AddStringToObject(item, "name", entry->fm.name) ||
		    !cJSON_AddStringToObject(item, "description", entry->fm.description) ||
		    !cJSON_AddItemToArray(matches, item)) {
			cJSON_Delete(item);
			rc = -ENOMEM;
			break;
		}
		count++;
	}
	if (rc >= 0 && (!cJSON_AddNumberToObject(data, "total", total) ||
	    !cJSON_AddNumberToObject(data, "next_offset", total > offset + count ?
				    offset + count : -1)))
		rc = -ENOMEM;
	cJSON_Delete(input);
	if (rc < 0) {
		cJSON_Delete(data);
		MORPH_RETURN(rc);
	}
	return tool_result_success(result, data);
}

int skill_activate_init(struct tool_registry *reg, struct skill_registry *skills)
{
	if (!reg || !skills)
		return -EINVAL;
	int rc = tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "activate_skill",
		.description = "Activate a skill by name to load its specialized instructions "
			"into context. Use when a task matches a skill's description.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
			"\"name\":{\"type\":\"string\","
			"\"description\":\"The skill name to activate\"}"
			"},\"required\":[\"name\"]}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = skill_activate_exec,
		.user_data = skills,
	});
	if (rc != 0)
		return rc;
	return tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "skill_search",
		.description = "Find skills by a case-insensitive name or description substring. "
			"Use concise English keywords. Empty query lists skills; "
			"offset pages through eight matches. Read the full matching "
			"descriptions, then use activate_skill "
			"to load the required instructions.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
			"\"query\":{\"type\":\"string\"},"
			"\"offset\":{\"type\":\"integer\",\"minimum\":0}},"
			"\"required\":[\"query\"],\"additionalProperties\":false}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = skill_search_exec,
		.user_data = skills,
		.flags = TOOL_FLAG_READONLY,
	});
}
