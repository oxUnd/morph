#include "sapi/cli/presentation/agent_tools.h"

#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/style.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/utf8.h"
#include <string.h>
#include <stdio.h>

int cli_agent_tools_matches(const char *name)
{
	return name && (!strcmp(name, "spawn_agent") || !strcmp(name, "followup_task") ||
			!strcmp(name, "send_message") || !strcmp(name, "wait_agent") ||
			!strcmp(name, "list_agents") || !strcmp(name, "interrupt_agent"));
}

/* MultiAgentV2 activity events own the visible lifecycle for these operations.
 * Keep calls in the transcript model for full details and JSON inspection. */
int cli_agent_tools_analytics_only(const char *name)
{
	return name && (!strcmp(name, "send_message") || !strcmp(name, "followup_task") ||
			!strcmp(name, "interrupt_agent") || !strcmp(name, "list_agents"));
}

static const char *cli_agent_tools_string(const cJSON *object, const char *key)
{
	const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
	return value ? value : "";
}

static const char *cli_agent_tools_label(const char *id)
{
	return !strncmp(id, "/root/", 6) ? id + 6 : id;
}

static void cli_agent_tools_clip(morph_buf_t *out, const char *text, int columns)
{
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char clipped[BUFSIZ];
	if (!safe || columns < 1)
		return;
	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), safe, (size_t)columns,
						 0);
	(void)morph_buf_puts(out, clipped);
}

int cli_agent_tools_row(const struct cli_transcript_tool *tool, morph_buf_t *out, int columns,
			int styled)
{
	if (!cli_agent_tools_matches(tool->name))
		return 0;
	cJSON *args MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(tool->args);
	int running = cli_transcript_tool_is_running(tool);
	int failed = tool->state < 0;
	const char *verb = "";
	const char *name = "";
	if (!strcmp(tool->name, "spawn_agent")) {
		verb = failed ? "Failed to spawn" : running ? "Spawning" : "Spawned";
		name = cli_agent_tools_string(args, "task_name");
	} else if (!strcmp(tool->name, "followup_task")) {
		verb = failed	 ? "Task assignment failed for"
		       : running ? "Assigning task to"
				 : "Assigned task to";
		name = cli_agent_tools_label(cli_agent_tools_string(args, "target"));
	} else if (!strcmp(tool->name, "send_message")) {
		verb = failed	 ? "Message failed for"
		       : running ? "Sending message to"
				 : "Sent message to";
		name = cli_agent_tools_label(cli_agent_tools_string(args, "target"));
	} else if (!strcmp(tool->name, "wait_agent")) {
		verb = failed	 ? "Agent wait failed"
		       : running ? "Waiting for agents"
				 : "Finished waiting";
	} else if (!strcmp(tool->name, "interrupt_agent")) {
		verb = failed ? "Failed to interrupt" : running ? "Interrupting" : "Interrupted";
		name = cli_agent_tools_label(cli_agent_tools_string(args, "target"));
	} else {
		verb = failed	 ? "Agent lookup failed"
		       : running ? "Checking agents"
				 : "Listed agents";
	}
	if (styled)
		(void)morph_buf_puts(out, failed ? ANSI_RED : ANSI_DIM);
	(void)morph_buf_puts(out, "• ");
	if (styled)
		(void)morph_buf_puts(out, ANSI_RESET ANSI_BOLD);
	int available = columns - (int)CLI_CONTENT_RIGHT_PADDING - 2;
	cli_agent_tools_clip(out, verb, available);
	if (*name && available > (int)utf8_display_width(verb) + 1) {
		(void)morph_buf_puts(out, " ");
		if (styled)
			(void)morph_buf_puts(out, ANSI_RESET CLI_COMMAND_COLOR);
		cli_agent_tools_clip(out, name, available - (int)utf8_display_width(verb) - 1);
	}
	if (styled)
		(void)morph_buf_puts(out, ANSI_RESET);
	return 1;
}

int cli_agent_tools_preview(const struct cli_transcript_tool *tool, morph_buf_t *out)
{
	if (!cli_agent_tools_matches(tool->name))
		return 0;
	if (tool->state < 0)
		return 0;
	cJSON *args MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(tool->args);
	const char *message = cli_agent_tools_string(args, "message");
	/* Full task prompts and raw results remain available in turn details. */
	if (*message)
		cli_agent_tools_clip(out, message, 160);
	return 1;
}
