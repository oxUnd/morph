#include "sapi/cli/presentation/agent_tools.h"

#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/style.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
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

static void cli_agent_tools_clip(nb_buf *out, const char *text, int columns)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, strlen(text), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char clipped[BUFSIZ];
	if (!safe || columns < 1)
		return;
	(void)morph_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), safe,
							   (size_t)columns, 0);
	(void)nb_buf_append_cstr(out, clipped);
}

int cli_agent_tools_row(const struct cli_transcript_tool *tool, nb_buf *out, int columns,
			int styled)
{
	if (!cli_agent_tools_matches(tool->name))
		return 0;
	cJSON *args NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(tool->args);
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) =
		tool->result ? cJSON_Parse(tool->result) : NULL;
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(result, "data");
	if (!cJSON_IsObject(data))
		data = result;
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	int running = cli_transcript_tool_is_running(tool);
	int failed = !tool->interrupted && (tool->state < 0 || tool->timed_out);
	const char *verb = "";
	const char *name = "";
	if (!strcmp(tool->name, "spawn_agent")) {
		verb = failed ? "Failed to spawn" : running ? "Spawning" : "Spawned";
		name = cli_agent_tools_string(args, "task_name");
		const char *id = cli_agent_tools_string(data, "agent_id");
		if (*id)
			name = id;
	} else if (!strcmp(tool->name, "followup_task")) {
		verb = failed	 ? "Task assignment failed for"
		       : running ? "Assigning task to"
				 : "Assigned task to";
		name = cli_agent_tools_string(args, "target");
	} else if (!strcmp(tool->name, "send_message")) {
		verb = failed	 ? "Message failed for"
		       : running ? "Sending message to"
				 : "Sent message to";
		name = cli_agent_tools_string(args, "target");
	} else if (!strcmp(tool->name, "wait_agent")) {
		verb = failed	 ? "Agent wait failed"
		       : running ? "Waiting for agents"
				 : "Finished waiting";
	} else if (!strcmp(tool->name, "interrupt_agent")) {
		verb = failed ? "Failed to interrupt" : running ? "Interrupting" : "Interrupted";
		name = cli_agent_tools_string(args, "target");
	} else {
		verb = failed	 ? "Agent lookup failed"
		       : running ? "Checking agents"
				 : "Listed agents";
	}
	if (*name && *name != '/' &&
	    (nb_buf_init(&path), nb_buf_reserve(&path, 64) ? 0 : NB_ERR_NOMEM) == 0 &&
	    (nb_buf_printf(&path, "%s/%s",
			   tool->agent_id && *tool->agent_id ? tool->agent_id : "/root", name)
		     ? 0
		     : NB_ERR_NOMEM) == 0)
		name = nb_buf_cstr(&path);
	const char *marker = cli_transcript_tool_is_pending(tool) ? CLI_FEED_PENDING
		: tool->interrupted ? CLI_FEED_CANCELLED
		: failed ? CLI_FEED_FAILED : running ? CLI_FEED_RUNNING : CLI_FEED_DONE;
	const char *color = tool->interrupted || cli_transcript_tool_is_pending(tool) ? ANSI_DIM
		: failed ? CLI_FEED_FAILED_STYLE : running ? ANSI_CYAN : ANSI_GREEN;
	if (styled)
		(void)nb_buf_append_cstr(out, color);
	(void)nb_buf_printf(out, "%s ", marker);
	if (styled)
		(void)nb_buf_append_cstr(out, ANSI_RESET ANSI_BOLD);
	int available = columns - (int)CLI_CONTENT_RIGHT_PADDING - 2;
	cli_agent_tools_clip(out, verb, available);
	if (*name && available > (int)nb_utf8_width_cstr(verb) + 1) {
		(void)nb_buf_append_cstr(out, " ");
		if (styled)
			(void)nb_buf_append_cstr(out, ANSI_RESET CLI_COMMAND_COLOR);
		cli_agent_tools_clip(out, name, available - (int)nb_utf8_width_cstr(verb) - 1);
	}
	if (styled)
		(void)nb_buf_append_cstr(out, ANSI_RESET);
	return 1;
}

int cli_agent_tools_preview(const struct cli_transcript_tool *tool, nb_buf *out)
{
	if (!cli_agent_tools_matches(tool->name))
		return 0;
	if (tool->state < 0)
		return 0;
	/* Delegation prompts belong to the agent panel and explicit details,
	 * including restored calls. Successful lifecycle rows need no body. */
	(void)out;
	return 1;
}
