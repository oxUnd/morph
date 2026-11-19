#include "sapi/cli/components/tool_call.h"
#include <nb/err.h>
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/agent_tools.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/transcript.h"
#include <nb/cleanup.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/component_state.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct cli_tool_call {
	struct cli_report report;
	struct nt_component_state *state;
};

struct cli_tool_call_snapshot {
	struct cli_transcript_tool tool;
	unsigned references;
};

struct cli_tool_call_state {
	struct cli_tool_call_snapshot *snapshot;
	int styled, details;
};

static void cli_tool_call_snapshot_cleanup(struct cli_tool_call_snapshot **owner)
{
	struct cli_tool_call_snapshot *snapshot = *owner;
	if (snapshot && --snapshot->references == 0) {
		cli_transcript_tool_cleanup(&snapshot->tool);
		free(snapshot);
	}

	*owner = NULL;
}

static char *cli_tool_call_string_copy(const char *text)
{
	return strdup(text ? text : "");
}

static int cli_tool_call_snapshot_copy(const struct cli_transcript_tool *source,
				       struct cli_tool_call_snapshot **out)
{
	struct cli_tool_call_snapshot *snapshot NB_CLEANUP(cli_tool_call_snapshot_cleanup) =
		calloc(1, sizeof(*snapshot));
	if (!snapshot)
		return NB_ERR_NOMEM;

	snapshot->references = 1;
	struct cli_transcript_tool *tool = &snapshot->tool;
	tool->id = cli_tool_call_string_copy(source->id);
	tool->name = cli_tool_call_string_copy(source->name);
	tool->agent_id = cli_tool_call_string_copy(source->agent_id);
	tool->args = cli_tool_call_string_copy(source->args);
	tool->subject = cli_tool_call_string_copy(source->subject);
	tool->result = cli_tool_call_string_copy(source->result);
	tool->error = cli_tool_call_string_copy(source->error);
	tool->summary = cli_tool_call_string_copy(source->summary);
	tool->patch_preview = cli_tool_call_string_copy(source->patch_preview);
	if (!tool->id || !tool->name || !tool->agent_id || !tool->args || !tool->subject ||
	    !tool->result || !tool->error || !tool->summary || !tool->patch_preview)
		return NB_ERR_NOMEM;

	int rc = (nb_buf_append(&tool->stream, nb_buf_cstr(&source->stream), source->stream.len)
			  ? 0
			  : NB_ERR_NOMEM);
	if (rc)
		return rc;

	tool->state = source->state;
	tool->timed_out = source->timed_out;
	tool->historical = source->historical;
	tool->interrupted = source->interrupted;
	tool->shell = source->shell;
	tool->process_poll = source->process_poll;
	tool->process_running = source->process_running;
	tool->started = source->started;
	tool->ended = source->ended;
	*out = snapshot;
	snapshot = NULL;
	return 0;
}

static int cli_tool_call_format(struct cli_report *report, nb_buf *out, int width)
{
	struct cli_tool_call *widget = (struct cli_tool_call *)report;
	int columns = width + (int)CLI_CONTENT_RIGHT_PADDING;
	const struct cli_tool_call_state *state = nt_component_state_get(widget->state);
	int rc = cli_transcript_tool_append(out, &state->snapshot->tool, columns, state->styled,
					    state->details);
	if (rc)
		return rc;

	while (out->len && out->ptr[out->len - 1] == '\n')
		out->ptr[--out->len] = '\0';

	if (out->len && !state->details && cli_transcript_tool_is_running(&state->snapshot->tool)) {
		char *newline = strchr(out->ptr, '\n');
		if (newline) {
			out->len = (size_t)(newline - out->ptr);
			*newline = '\0';
		}

		nb_buf live NB_CLEANUP(nb_buf_cleanup) = {0};
		(void)cli_transcript_tool_live_body(&state->snapshot->tool, &live, columns,
						    state->styled);
		rc = nb_buf_status(&live);
		if (!rc && live.len &&
		    (!nb_buf_append_char(out, '\n') || !nb_buf_append_str(out, nb_buf_str(&live))))
			rc = NB_ERR_NOMEM;
	}

	return rc;
}

static int cli_tool_call_state_copy(void *to, const void *from, void *data)
{
	struct cli_tool_call_state *copy = to;
	const struct cli_tool_call_state *source = from;
	(void)data;
	if (source->snapshot->references == UINT_MAX)
		return NB_ERR_RANGE;

	source->snapshot->references++;
	*copy = *source;
	return 0;
}

static void cli_tool_call_state_destroy(void *value, void *data)
{
	struct cli_tool_call_state *state = value;
	(void)data;
	cli_tool_call_snapshot_cleanup(&state->snapshot);
}

static int cli_tool_call_hidden(void *data, struct nt_value *result)
{
	const struct cli_tool_call_state *state = nt_component_state_get(data);
	return nt_value_set_bool(result, !state->details && cli_agent_tools_analytics_only(
								    state->snapshot->tool.name));
}

static const struct nt_view_class cli_tool_call_class = {
	.superclass = &cli_report_class,
	.instance_size = sizeof(struct cli_tool_call),
	.kind = "cli_tool_call",
};

int cli_tool_call_update(struct nt_view *view, const struct cli_transcript_tool *tool, int styled,
			 int details)
{
	if (!view || view->type != &cli_tool_call_class || !tool)
		return NB_ERR_INVAL;

	if (!nt_view_is_owner_thread(view))
		return NB_ERR_NOT_PERMITTED;

	struct cli_tool_call *widget = (struct cli_tool_call *)view;
	struct cli_tool_call_snapshot *snapshot NB_CLEANUP(cli_tool_call_snapshot_cleanup) = NULL;
	int rc = cli_tool_call_snapshot_copy(tool, &snapshot);
	if (rc)
		return rc;

	struct cli_tool_call_state next = {snapshot, styled, details};
	rc = nt_component_state_set(widget->state, &next);
	if (!rc)
		cli_report_invalidate(&widget->report, styled);

	return rc;
}

struct nt_view *cli_tool_call_add_borrow(struct nt_builder *builder, struct nt_view *parent,
					 const struct cli_transcript_tool *tool, int styled,
					 int details)
{
	const struct nt_view_options options = {
		.margin = {.top = 1}, .padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_tool_call *widget = (struct cli_tool_call *)cli_report_add(
		builder, parent, &cli_tool_call_class, &options, styled, cli_tool_call_format);
	if (!widget)
		return NULL;

	struct nt_view *view = &widget->report.base;
	struct cli_tool_call_snapshot *snapshot NB_CLEANUP(cli_tool_call_snapshot_cleanup) = NULL;
	int rc = tool ? cli_tool_call_snapshot_copy(tool, &snapshot) : NB_ERR_INVAL;
	const struct cli_tool_call_state initial = {snapshot, styled, details};
	const struct nt_component_state_options options_state = {
		.size = sizeof(initial),
		.initial = &initial,
		.copy = cli_tool_call_state_copy,
		.destroy = cli_tool_call_state_destroy,
	};
	if (!rc)
		rc = nt_view_use_state(view, &options_state, &widget->state);

	if (!rc)
		rc = nt_view_bind_compute(view, NT_PROP_HIDDEN, cli_tool_call_hidden,
					  widget->state);

	nt_builder_fail(builder, rc);
	return view;
}
