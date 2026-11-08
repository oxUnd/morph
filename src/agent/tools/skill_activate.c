#include "skill_activate.h"
#include "agent/tool_runtime.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/utf8.h"
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
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = NULL;
	const cJSON *name_item;
	const char *name;
	struct skill_entry *skill;
	int rc;

	if (!result)
		return -EINVAL;
	if (!skills) {
		(void)tool_result_error_copy(result, "not_initialized",
		"Skill system is unavailable.");
		return MORPH_ERR_NOT_INITIALIZED;
	}
	root = args_json ? cJSON_Parse(args_json) : NULL;
	name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
	name = cJSON_GetStringValue(name_item);
	if (!name || !*name || utf8valid(name) || utf8len(name) > SKILL_NAME_MAX_CHARS) {
		(void)tool_result_error_format(result, "invalid_name",
			"Provide a valid UTF-8 skill name of at most %d characters.",
			SKILL_NAME_MAX_CHARS);
		return -EINVAL;
	}
	skill = skill_borrow(skills, name);
	if (!skill) {
		(void)tool_result_error_format(result, "skill_not_found",
		"Skill '%s' was not found.",
					       name);
		return -ENOENT;
	}
	if (!skill->enabled) {
		(void)tool_result_error_format(result, "skill_disabled", "Skill '%s' is disabled.",
					       name);
		return -EACCES;
	}
	if (skill->activated && skill->body_loaded)
		return tool_result_success_format(result,
			"Skill '%s' is already active. Its instructions are in context.", name);
	rc = skill_activate(skill);
	if (rc < 0) {
		(void)tool_result_error_format(result, "activation_failed",
			"Failed to activate skill '%s': %s", name, morph_strerror(rc));
		return rc;
	}
	if (!skill->body || !skill->body[0])
		return tool_result_success_text_copy(result,
			"Skill activated but contains no instructions.");
	log_info(result->runtime ? result->runtime->http.logger : NULL,
		 "skill_activate: '%s' activated", name);
	return tool_result_success_text_copy(result,
		"Skill activated. Its full instructions are supplied in the active skill context "
		"on the next model request.");
}

int skill_activate_init(struct tool_registry *reg, struct skill_registry *skills)
{
	if (!reg || !skills)
		return -EINVAL;
	return tool_register_borrow(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "activate_skill",
			.description =
				"Activate a skill by name to load its specialized instructions "
				"into context. Use when a task matches a skill's description.",
			.input_schema = "{\"type\":\"object\",\"properties\":{"
					"\"name\":{\"type\":\"string\","
					"\"description\":\"The skill name to activate\"}"
					"},\"required\":[\"name\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = skill_activate_exec,
			.user_data = skills,
		});
}
