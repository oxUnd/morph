#include "skill_activate.h"
#include "util/log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/error.h"

static struct skill_registry *g_skills;

static int skill_activate_exec(const char *args_json, char **result_json,
			       void *user_data)
{
	(void)user_data;
	if (!result_json)
		return -EINVAL;
	if (!g_skills) {
		*result_json = strdup("{\"error\":\"skill system not initialized\"}");
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
		*result_json = strdup(
			"{\"error\":\"missing or empty 'name' parameter. "
			"Usage: activate_skill({\\\"name\\\": \\\"skill-name\\\"})\"}");
		return -EINVAL;
	}

	struct skill_entry *skill = skill_lookup(g_skills, name);
	if (!skill) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"skill '%s' not found\"}", name);
		*result_json = strdup(err);
		return -ENOENT;
	}

	if (!skill->enabled) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"skill '%s' is disabled\"}", name);
		*result_json = strdup(err);
		return -EACCES;
	}

	if (skill->activated && skill->body_loaded) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			 "Skill '%s' is already active. Its instructions are in context.",
			 name);
		*result_json = strdup(msg);
		return 0;
	}

	int rc = skill_activate(skill);
	if (rc < 0) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"failed to activate skill '%s' (code %d)\"}",
			 name, rc);
		*result_json = strdup(err);
		return rc;
	}

	if (!skill->body || !skill->body[0]) {
		*result_json = strdup(
			"Skill activated but contains no instructions.");
		return 0;
	}

	size_t body_len = strlen(skill->body);
	size_t dir_len = strlen(skill->skill_dir);
	size_t result_len = 128 + strlen(name) + dir_len + body_len + 16;
	char *result = malloc(result_len);
	if (!result) {
		*result_json = strdup("{\"error\":\"out of memory\"}");
		return -ENOMEM;
	}
	snprintf(result, result_len,
		 "<skill name=\"%s\" dir=\"%s\">\n%s\n</skill>",
		 name, skill->skill_dir, skill->body);
	*result_json = result;

	log_info("skill_activate: '%s' activated (%zu bytes of instructions)",
		 name, body_len);
	return 0;
}

int skill_activate_init(struct tool_registry *reg, struct skill_registry *skills)
{
	if (!reg || !skills)
		return -EINVAL;
	g_skills = skills;
	return tool_register(reg, "activate_skill",
		"Activate a skill by name to load its specialized instructions into context. "
		"Use when a task matches a skill's description.",
		"{\"type\":\"object\",\"properties\":{"
		"\"name\":{\"type\":\"string\",\"description\":\"The skill name to activate\"}"
		"},\"required\":[\"name\"]}",
		skill_activate_exec, NULL, NULL);
}
