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
		(void)tool_result_take_text(result, strdup("{\"error\":\"skill system not initialized\"}"));
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
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing or empty 'name' parameter. "
			"Usage: activate_skill({\\\"name\\\": \\\"skill-name\\\"})\"}"));
		return -EINVAL;
	}

	struct skill_entry *skill = skill_lookup(skills, name);
	if (!skill) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"skill '%s' not found\"}", name);
		(void)tool_result_take_text(result, strdup(err));
		return -ENOENT;
	}

	if (!skill->enabled) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"skill '%s' is disabled\"}", name);
		(void)tool_result_take_text(result, strdup(err));
		return -EACCES;
	}

	if (skill->activated && skill->body_loaded) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			 "Skill '%s' is already active. Its instructions are in context.",
			 name);
		(void)tool_result_take_text(result, strdup(msg));
		return 0;
	}

	int rc = skill_activate(skill);
	if (rc < 0) {
		char err[256];
		snprintf(err, sizeof(err),
			 "{\"error\":\"failed to activate skill '%s' (code %d)\"}",
			 name, rc);
		(void)tool_result_take_text(result, strdup(err));
		return rc;
	}

	if (!skill->body || !skill->body[0]) {
		(void)tool_result_take_text(result, strdup(
			"Skill activated but contains no instructions."));
		return 0;
	}

	size_t body_len = strlen(skill->body);
	size_t dir_len = strlen(skill->skill_dir);
	size_t result_len = 128 + strlen(name) + dir_len + body_len + 16;
	char *msg = malloc(result_len);
	if (!msg) {
		(void)tool_result_take_text(result, strdup("{\"error\":\"out of memory\"}"));
		return -ENOMEM;
	}
	snprintf(msg, result_len,
		 "<skill name=\"%s\" dir=\"%s\">\n%s\n</skill>",
		 name, skill->skill_dir, skill->body);
	(void)tool_result_take_text(result, msg);

	log_info("skill_activate: '%s' activated (%zu bytes of instructions)",
		 name, body_len);
	return 0;
}

int skill_activate_init(struct tool_registry *reg, struct skill_registry *skills)
{
	if (!reg || !skills)
		return -EINVAL;
	return tool_register(reg, "activate_skill",
		"Activate a skill by name to load its specialized instructions into context. "
		"Use when a task matches a skill's description.",
		"{\"type\":\"object\",\"properties\":{"
		"\"name\":{\"type\":\"string\",\"description\":\"The skill name to activate\"}"
		"},\"required\":[\"name\"]}",
		skill_activate_exec, skills, NULL);
}
