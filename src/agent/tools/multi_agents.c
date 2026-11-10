#include "multi_agents.h"

#include "util/cleanup.h"
#include "util/json.h"
#include "util/error.h"
#include <stdlib.h>
#include <string.h>

struct multi_agent_binding {
	struct agent_control *control;
	const char *operation;
};

static int multi_agent_exec(const char *arguments, struct tool_result *result, void *opaque)
{
	struct multi_agent_binding *binding = opaque;
	cJSON *args MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(arguments ? arguments : "{}");
	cJSON *out MORPH_CLEANUP(morph_json_cleanup) = NULL;
	int rc = agent_control_call(binding->control, binding->operation, args, result->runtime,
				    &out);
	if (rc != 0) {
		const char *code = rc == -EAGAIN		  ? "capacity_exceeded"
				   : rc == MORPH_ERR_TOKEN_BUDGET ? "token_budget_exhausted"
				   : rc == MORPH_ERR_AGENT_BUDGET ? "agent_budget_reserved"
				   : rc == -EINVAL		  ? "invalid_arguments"
								  : "invalid_operation";
		const char *message = rc == -EAGAIN ? "Agent execution capacity is full. Wait for "
						      "a running agent to finish "
						      "before spawning or resuming work."
				      : rc == -EINVAL && !strcmp(binding->operation, "wait_agent")
					      ? "timeout_ms must be an integer from 1 to 3600000."
				      : rc == -EINVAL && strcmp(binding->operation, "spawn_agent")
					      ? "Invalid collaboration arguments. Check this tool "
						"schema for required "
						"target/message fields, field types, and permitted "
						"optional fields."
				      : rc == -EINVAL ? "Invalid collaboration arguments. "
							"task_name accepts 1-64 lowercase "
							"letters, digits, '_' or '-'; fork_turns "
							"is 'all' or 'none'; model "
							"overrides require 'none'; write_paths "
							"must resolve inside the workspace. "
							"Check the tool schema before retrying."
				      : rc == -EBUSY ? "Write ownership conflicts with an active "
						       "agent or patch, or the target "
						       "is already running. Use disjoint "
						       "write_paths or wait before reassigning."
						     : morph_strerror(rc);
		int result_rc = tool_result_error_copy(result, code, message);
		return result_rc == 0 ? rc : result_rc;
	}
	char *json MORPH_CLEANUP(morph_json_text_cleanup) = cJSON_PrintUnformatted(out);
	return json ? tool_result_success_json_text_copy(result, json) : -ENOMEM;
}

static void binding_destroy(void *binding)
{
	free(binding);
}

int multi_agents_register(struct tool_registry *tools, struct agent_control *control)
{
	const struct {
		const char *name;
		const char *description;
		const char *schema;
	} specs[] = {
		{"spawn_agent",
		 "Spawn a collaborating agent and return its canonical path. "
		 "Assign optional write_paths (workspace files/directories) for apply_patch and "
		 "scoped shell writes. Each child also has private scratch space for builds/tests. "
		 "Without write_paths the workspace is read-only. "
		 "History and model are inherited by default. "
		 "All agents share the workspace. "
		 "Use fork_turns=none for a separate context or model/reasoning override.",
		 "{\"type\":\"object\",\"properties\":{\"task_name\":{\"type\":\"string\"},"
		 "\"message\":{\"type\":\"string\"},\"fork_turns\":{\"type\":\"string\","
		 "\"enum\":[\"all\",\"none\"]},\"model\":{\"type\":\"string\"},"
		 "\"reasoning_effort\":{\"type\":\"string\"},"
		 "\"write_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
		 "\"required\":[\"task_name\","
		 "\"message\"],"
		 "\"additionalProperties\":false}"},
		{"send_message",
		 "Send a message at the recipient's next safe point. "
		 "Does not start an idle agent. Use followup_task to request more work.",
		 "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\"},"
		 "\"message\":{\"type\":\"string\"}},\"required\":[\"target\",\"message\"],"
		 "\"additionalProperties\":false}"},
		{"followup_task",
		 "Give an existing agent more work. Starts a new turn if idle; "
		 "delivers steering at safe points if running.",
		 "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\"},"
		 "\"message\":{\"type\":\"string\"},"
		 "\"write_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
		 "\"required\":[\"target\",\"message\"],"
		 "\"additionalProperties\":false}"},
		{"interrupt_agent",
		 "Interrupt an agent's current turn; preserve it for followup_task.",
		 "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\"}},"
		 "\"required\":[\"target\"],\"additionalProperties\":false}"},
		{"list_agents", "List collaborating agents and their status in this conversation.",
		 "{\"type\":\"object\",\"properties\":{\"path_prefix\":{\"type\":\"string\"}},"
		 "\"additionalProperties\":false}"},
		{"wait_agent",
		 "Wait for mailbox activity, user steering, cancellation or timeout. "
		 "Messages and final answers are delivered at the next model boundary.",
		 "{\"type\":\"object\",\"properties\":{\"timeout_ms\":{\"type\":\"integer\","
		 "\"minimum\":1,\"maximum\":3600000}},\"additionalProperties\":false}"},
	};
	if (!tools || !control)
		return -EINVAL;
	for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
		struct multi_agent_binding *binding = calloc(1, sizeof(*binding));
		if (!binding)
			return -ENOMEM;
		binding->control = control;
		binding->operation = specs[i].name;
		struct tool_spec spec MORPH_CLEANUP(tool_spec_cleanup) = {
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = specs[i].name,
			.description = specs[i].description,
			.input_schema = specs[i].schema,
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.flags = TOOL_FLAG_READONLY,
			.exec = multi_agent_exec,
			.user_data = binding,
			.user_data_destroy = binding_destroy,
		};
		int rc = tool_register_replace_take(tools, &spec, 0);
		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}
