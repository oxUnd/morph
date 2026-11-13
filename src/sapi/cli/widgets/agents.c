#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/models/agents.h"

#include "sapi/cli/presentation/style.h"
#include "util/utf8.h"
#include "util/cleanup.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

const char *cli_agents_status(enum agent_status status)
{
	switch (status) {
	case AGENT_PENDING:
		return "pending";
	case AGENT_RUNNING:
		return "running";
	case AGENT_COMPLETED:
		return "completed";
	case AGENT_FAILED:
		return "failed";
	case AGENT_CANCELLED:
		return "interrupted";
	}
	return "unknown";
}

static void cli_agents_append(morph_buf_t *out, const char *text, int columns, const char *style)
{
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char clipped[BUFSIZ];
	if (!safe || columns < 1)
		return;
	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), safe, (size_t)columns,
						 0);
	(void)morph_buf_puts(out, style);
	(void)morph_buf_puts(out, clipped);
	if (*style)
		(void)morph_buf_puts(out, ANSI_RESET);
}

int cli_agents_receipt_append(morph_buf_t *out, const char *id, enum agent_status status,
			     int columns, int styled)
{
	if (!out)
		return -EINVAL;
	int completed = status == AGENT_COMPLETED;
	int cancelled = status == AGENT_CANCELLED;
	const char *action = completed ? "Finished" : cancelled ? "Interrupted" : "Failed";
	const char *marker = completed ? CLI_FEED_DONE
		: cancelled ? CLI_FEED_CANCELLED : CLI_FEED_FAILED;
	const char *color = completed ? ANSI_GREEN
		: cancelled ? ANSI_DIM : CLI_FEED_FAILED_STYLE;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING - 2;
	int rc = morph_buf_printf(out, "%s%s%s ", styled ? color : "", marker,
		styled ? ANSI_RESET : "");
	if (rc == 0)
		cli_agents_append(out, action, width, "");
	if (rc == 0 && width > (int)strlen(action) + 1) {
		rc = morph_buf_putc(out, ' ');
		if (rc == 0)
			cli_agents_append(out, id ? id : "agent", width - (int)strlen(action) - 1,
				styled ? CLI_COMMAND_COLOR : "");
	}
	if (rc == 0)
		rc = morph_buf_putc(out, '\n');
	return rc != 0 ? rc : out->failed;
}

int cli_agents_detail(morph_buf_t *out, const char *label, const char *value, int columns,
		      int styled)
{
	if (!out || !label || !value)
		return -EINVAL;
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		value, strlen(value), UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	char *heading MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		label, strlen(label), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe || !heading)
		return -ENOMEM;
	int rc = morph_buf_printf(out, "  %s%s%s\n", styled ? ANSI_DIM : "", heading,
				  styled ? ANSI_RESET : "");
	int width = columns > 5 ? columns - 5 : 1;
	const char *line = safe;
	while (rc == 0 && *line) {
		const char *newline = strchr(line, '\n');
		const char *end = utf8_advance_display_width_borrow(line, (size_t)width);
		if (newline && newline < end)
			end = newline;
		if (end == line && *line != '\n')
			end += utf8_next_codepoint_len(line, strnlen(line, 4));
		rc = morph_buf_printf(out, "    %.*s\n", (int)(end - line), line);
		line = *end == '\n' ? end + 1 : end;
	}
	return rc;
}

int cli_agents_summary(morph_buf_t *out, const struct agent_info *agents, int count, int columns)
{
	int active = 0, completed = 0, failed = 0, interrupted = 0;
	for (int i = 0; i < count; i++) {
		active += agents[i].status == AGENT_RUNNING || agents[i].status == AGENT_PENDING;
		completed += agents[i].status == AGENT_COMPLETED;
		failed += agents[i].status == AGENT_FAILED;
		interrupted += agents[i].status == AGENT_CANCELLED;
	}
	if (columns < 76 && (failed || interrupted))
		return morph_buf_printf(out, "Agents: %d run · %d %s", active,
			failed ? failed : interrupted, failed ? "failed" : "interrupted");
	int rc = morph_buf_printf(out, columns < 76 ? "Agents: %d running · %d done" :
		"Agents: %d running · %d completed", active, completed);
	if (rc == 0 && failed)
		rc = morph_buf_printf(out, " · %d failed", failed);
	if (rc == 0 && interrupted)
		rc = morph_buf_printf(out, " · %d interrupted", interrupted);
	if (rc == 0 && columns >= 100 && !failed && !interrupted)
		rc = morph_buf_puts(out, " · /agents");
	return rc;
}

int cli_agents_rows(morph_buf_t *out, const struct agent_info *agents, int count, int columns,
		    int max_agents, int styled)
{
	if (columns < 20 || max_agents < 1)
		return 0;
	int active = 0, shown = 0;
	for (int i = 0; i < count; i++)
		active += agents[i].status == AGENT_RUNNING || agents[i].status == AGENT_PENDING;
	int visible = active < max_agents ? active : max_agents;
	morph_buf_t activity MORPH_CLEANUP(morph_buf_cleanup) = {0};
	if (morph_buf_init(&activity, 128) != 0)
		return 0;
	for (int i = 0; i < count && shown < visible; i++) {
		const struct agent_info *agent = &agents[i];
		if (agent->status != AGENT_RUNNING && agent->status != AGENT_PENDING)
			continue;
		const char *name = agent->id;
		const char *status =
			agent->activity ? agent->activity : cli_agents_status(agent->status);
		morph_buf_reset(&activity);
		if (!strncmp(status, "Tool: ", 6))
			(void)morph_buf_printf(&activity, "Running %s", status + 6);
		else
			(void)morph_buf_puts(&activity, status);
		if (out->len)
			(void)morph_buf_putc(out, '\n');
		cli_agents_append(out, shown + 1 == active ? "  └ " : "  ├ ", 4,
				  styled ? ANSI_DIM : "");
		cli_agents_append(out, agent->status == AGENT_RUNNING ? CLI_FEED_RUNNING
							    : CLI_FEED_PENDING, 1,
				  styled ? (agent->status == AGENT_RUNNING ? ANSI_CYAN : ANSI_DIM)
					 : "");
		(void)morph_buf_puts(out, " ");
		int name_width = columns / 2;
		if (name_width > 28)
			name_width = 28;
		int actual = (int)utf8_display_width(name);
		if (actual < name_width)
			name_width = actual;
		cli_agents_append(out, name, name_width, styled ? CLI_COMMAND_COLOR : "");
		(void)morph_buf_puts(out, "  ");
		cli_agents_append(out, morph_buf_cstr_borrow(&activity), columns - name_width - 9,
				  styled ? ANSI_DIM : "");
		shown++;
	}
	if (active > shown) {
		if (out->len)
			(void)morph_buf_putc(out, '\n');
		morph_buf_reset(&activity);
		(void)morph_buf_printf(&activity, "  └ +%d more · /agents", active - shown);
		cli_agents_append(out, morph_buf_cstr_borrow(&activity), columns - 1,
				  styled ? ANSI_DIM : "");
		shown++;
	}
	return shown;
}

int cli_agents_model_visible(const struct cli_agents_model *model)
{
	return cli_agents_model_active(model);
}

int cli_agents_model_render(const struct cli_agents_model *model, morph_buf_t *out,
			    morph_buf_t *summary, int columns, int maximum, int styled)
{
	if (!model || !out || !summary)
		return -EINVAL;
	if (!model->count)
		return 0;
	(void)cli_agents_summary(summary, model->items, model->count, columns);
	return cli_agents_rows(out, model->items, model->count, columns, maximum, styled);
}
