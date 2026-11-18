#include "sapi/cli/presentation/agents.h"
#include "sapi/cli/models/agents.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/presentation/feed.h"
#include "sapi/cli/presentation/feed_block.h"
#include <nb/cleanup.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <nb/err.h>
#include "sapi/cli/presentation/style.h"
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include <stdio.h>

void cli_presentation_agents_flush(struct cli_application *ctx)
{
	struct cli_agents_receipt *receipt;
	int joined = 0;

	if (!ctx || !ctx->agents.receipts.len)
		return;

	cli_feed_history_begin(ctx);
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&ctx->agents.receipts) &&
	     (receipt = (struct cli_agents_receipt *)nb_vec_at(&ctx->agents.receipts, _i_));
	     _i_++) {
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_AGENT_RECEIPT,
			.join_previous = joined,
			.content.agent = {.id = receipt->id, .status = receipt->status},
		};

		(void)cli_feed_block_write(ctx, &block);
		joined = 1;
	}

	cli_agents_model_receipts_clear(&ctx->agents);
	cli_feed_history_end(ctx);
}

int cli_presentation_agents_event(struct cli_application *ctx,
				  const struct morph_event *event)
{
	const char *id;
	const char *status;
	int completed;
	int cancelled;
	enum agent_status outcome;

	if (!ctx || !event || !event->name ||
	    (strcmp(event->name, "agent.started") && strcmp(event->name, "agent.updated") &&
	     strcmp(event->name, "agent.completed")))
		return 0;

	if (cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
		    event->data, "parent_session_id")) != (double)ctx->agents.session_id)
		return 1;

	if (strcmp(event->name, "agent.completed"))
		return 1;

	id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event->data, "agent_id"));
	status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event->data, "status"));
	completed = status && !strcmp(status, "completed");
	cancelled = status && (!strcmp(status, "interrupted") || !strcmp(status, "cancelled"));
	outcome = completed ? AGENT_COMPLETED : cancelled ? AGENT_CANCELLED : AGENT_FAILED;
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && ctx->turn_active &&
	    ctx->presenter && !ctx->presenter->final_rendered &&
	    (ctx->presenter->markdown_stream || ctx->presenter->native_content_started ||
	     (ctx->presenter->event_stream_has_delta &&
	      !ctx->presenter->event_stream_complete))) {
		/* Keep parent answer streams contiguous. Snapshots still update live. */
		int rc = cli_agents_model_receipt_add(&ctx->agents, id, outcome);

		if (rc != 0)
			return rc;
	} else {
		struct cli_feed_block block = {
			.kind = CLI_FEED_BLOCK_AGENT_RECEIPT,
			.content.agent = {.id = id, .status = outcome},
		};

		cli_feed_history_begin(ctx);
		if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
			(void)cli_feed_block_write(ctx, &block);
		} else {
			nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};

			if ((nb_buf_init(&line), nb_buf_reserve(&line, 128) ? 0 : NB_ERR_NOMEM) ==
				    0 &&
			    cli_agents_receipt_append(&line, id, outcome, BUFSIZ,
						      cli_output_color_enabled(ctx)) == 0)
				cli_output_printf(ctx, "%s", nb_buf_cstr(&line));
		}

		cli_feed_history_end(ctx);
	}

	cli_feed_queue_changed(ctx);
	return 1;
}

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

static void cli_agents_append(nb_buf *out, const char *text, int columns, const char *style)
{
	if (columns < 1 || !nb_buf_ok(out))
		return;

	char *safe NB_CLEANUP(nb_charp_cleanup) = nt_sanitize_dup(
		text, strlen(text), NT_TEXT_SINGLE_LINE, NULL);
	char clipped[BUFSIZ];
	if (!safe) {
		nb_buf_set_error(out, NB_ERR_NOMEM);
		return;
	}

	(void)nt_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), safe,
							   (size_t)columns, 0);
	(void)nb_buf_append_cstr(out, style);
	(void)nb_buf_append_cstr(out, clipped);
	if (*style)
		(void)nb_buf_append_cstr(out, ANSI_RESET);
}

int cli_agents_receipt_append(nb_buf *out, const char *id, enum agent_status status, int columns,
			      int styled)
{
	if (!out)
		return NB_ERR_INVAL;

	int completed = status == AGENT_COMPLETED;
	int cancelled = status == AGENT_CANCELLED;
	const char *action = completed ? "Finished" : cancelled ? "Interrupted" : "Failed";
	const char *marker = completed ? CLI_FEED_DONE
		: cancelled ? CLI_FEED_CANCELLED : CLI_FEED_FAILED;
	const char *color = completed ? ANSI_GREEN
		: cancelled ? ANSI_DIM : CLI_FEED_FAILED_STYLE;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING - 2;
	int rc = (nb_buf_printf(out, "%s%s%s ", styled ? color : "", marker,
				styled ? ANSI_RESET : "")
			  ? 0
			  : NB_ERR_NOMEM);
	if (rc == 0)
		cli_agents_append(out, action, width, "");

	if (rc == 0 && width > (int)strlen(action) + 1) {
		rc = (nb_buf_append_char(out, ' ') ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			cli_agents_append(out, id ? id : "agent", width - (int)strlen(action) - 1,
				styled ? CLI_COMMAND_COLOR : "");
	}

	if (rc == 0)
		rc = (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);

	return rc != 0 ? rc : nb_buf_status(out);
}

int cli_agents_detail(nb_buf *out, const char *label, const char *value, int columns, int styled)
{
	if (!out || !label || !value)
		return NB_ERR_INVAL;

	char *safe NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(value, strlen(value), NT_TEXT_MULTILINE, NULL);
	char *heading NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(label, strlen(label), NT_TEXT_SINGLE_LINE, NULL);
	if (!safe || !heading)
		return NB_ERR_NOMEM;

	int rc = (nb_buf_printf(out, "  %s%s%s\n", styled ? ANSI_DIM : "", heading,
				styled ? ANSI_RESET : "")
			  ? 0
			  : NB_ERR_NOMEM);
	int width = columns > 5 ? columns - 5 : 1;
	const char *line = safe;
	while (rc == 0 && *line) {
		const char *newline = strchr(line, '\n');
		const char *end = nt_terminal_advance_display_width(line, (size_t)width);
		if (newline && newline < end)
			end = newline;

		if (end == line && *line != '\n')
			end += nb_utf8_next_codepoint_len(line, strnlen(line, 4));

		rc = (nb_buf_printf(out, "    %.*s\n", (int)(end - line), line) ? 0 : NB_ERR_NOMEM);
		line = *end == '\n' ? end + 1 : end;
	}

	return rc;
}

int cli_agents_summary(nb_buf *out, const struct agent_info *agents, int count, int columns)
{
	int active = 0, completed = 0, failed = 0, interrupted = 0;
	for (int i = 0; i < count; i++) {
		active += agents[i].status == AGENT_RUNNING || agents[i].status == AGENT_PENDING;
		completed += agents[i].status == AGENT_COMPLETED;
		failed += agents[i].status == AGENT_FAILED;
		interrupted += agents[i].status == AGENT_CANCELLED;
	}

	if (columns < 76 && (failed || interrupted))
		return (nb_buf_printf(out, "Agents: %d run · %d %s", active,
				      failed ? failed : interrupted,
				      failed ? "failed" : "interrupted")
				? 0
				: NB_ERR_NOMEM);

	int rc = (nb_buf_printf(out,
				columns < 76 ? "Agents: %d running · %d done"
					     : "Agents: %d running · %d completed",
				active, completed)
			  ? 0
			  : NB_ERR_NOMEM);
	if (rc == 0 && failed)
		rc = (nb_buf_printf(out, " · %d failed", failed) ? 0 : NB_ERR_NOMEM);

	if (rc == 0 && interrupted)
		rc = (nb_buf_printf(out, " · %d interrupted", interrupted) ? 0 : NB_ERR_NOMEM);

	if (rc == 0 && columns >= 100 && !failed && !interrupted)
		rc = (nb_buf_append_cstr(out, " · /agents") ? 0 : NB_ERR_NOMEM);

	return rc;
}

int cli_agents_rows(nb_buf *out, const struct agent_info *agents, int count, int columns,
		    int max_agents, int styled)
{
	int rc = nb_buf_status(out);
	if (rc)
		return rc;

	if ((!agents && count > 0) || count < 0)
		return NB_ERR_INVAL;

	if (columns < 20 || max_agents < 1)
		return 0;

	int active = 0, shown = 0;
	for (int i = 0; i < count; i++)
		active += agents[i].status == AGENT_RUNNING || agents[i].status == AGENT_PENDING;

	int visible = active < max_agents ? active : max_agents;
	nb_buf activity NB_CLEANUP(nb_buf_cleanup) = {0};
	for (int i = 0; i < count && shown < visible; i++) {
		const struct agent_info *agent = &agents[i];
		if (agent->status != AGENT_RUNNING && agent->status != AGENT_PENDING)
			continue;

		const char *name = agent->id;
		const char *status =
			agent->activity ? agent->activity : cli_agents_status(agent->status);
		nb_buf_clear(&activity);
		if (!strncmp(status, "Tool: ", 6))
			(void)nb_buf_printf(&activity, "Running %s", status + 6);
		else
			(void)nb_buf_append_cstr(&activity, status);

		if (!nb_buf_ok(&activity))
			return nb_buf_status(&activity);

		if (out->len)
			(void)nb_buf_append_char(out, '\n');

		cli_agents_append(out, shown + 1 == active ? "  └ " : "  ├ ", 4,
				  styled ? ANSI_DIM : "");
		cli_agents_append(out, agent->status == AGENT_RUNNING ? CLI_FEED_RUNNING
							    : CLI_FEED_PENDING, 1,
				  styled ? (agent->status == AGENT_RUNNING ? ANSI_CYAN : ANSI_DIM)
					 : "");
		(void)nb_buf_append_cstr(out, " ");
		int name_width = columns / 2;
		if (name_width > 28)
			name_width = 28;

		int actual = (int)nb_utf8_width_cstr(name);
		if (actual < name_width)
			name_width = actual;

		cli_agents_append(out, name, name_width, styled ? CLI_COMMAND_COLOR : "");
		(void)nb_buf_append_cstr(out, "  ");
		cli_agents_append(out, nb_buf_cstr(&activity), columns - name_width - 9,
				  styled ? ANSI_DIM : "");
		if (!nb_buf_ok(out))
			return nb_buf_status(out);

		shown++;
	}

	if (active > shown) {
		if (out->len)
			(void)nb_buf_append_char(out, '\n');

		nb_buf_clear(&activity);
		if (!nb_buf_printf(&activity, "  └ +%d more · /agents", active - shown))
			return nb_buf_status(&activity);

		cli_agents_append(out, nb_buf_cstr(&activity), columns - 1, styled ? ANSI_DIM : "");
		shown++;
	}

	return nb_buf_ok(out) ? shown : nb_buf_status(out);
}

int cli_agents_model_render(const struct cli_agents_model *model, nb_buf *out, nb_buf *summary,
			    int columns, int maximum, int styled)
{
	if (!model || !out || !summary)
		return NB_ERR_INVAL;

	if (!model->count)
		return 0;

	int rc = cli_agents_summary(summary, model->items, model->count, columns);
	if (rc)
		return rc;

	return cli_agents_rows(out, model->items, model->count, columns, maximum, styled);
}
