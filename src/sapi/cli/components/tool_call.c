#include "sapi/cli/components/tool_call.h"
#include <nb/err.h>
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/agent_tools.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/transcript.h"
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/component_state.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cli_tool_call {
	struct cli_report report;
	struct nt_component_state *state, *expanded, *grouped;
	struct nt_view *count, *toggle;
	char count_text[64];
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
	int expanded = nt_component_state_get_int(widget->expanded) != 0;
	struct cli_transcript_tool_preview preview = {0};
	int rc = state->details
		? cli_transcript_tool_append(out, &state->snapshot->tool, columns, state->styled, 1)
		: cli_transcript_tool_output_append(out, &state->snapshot->tool, columns,
			state->styled, expanded, &preview);
	if (rc)
		return rc;

	while (out->len && out->ptr[out->len - 1] == '\n')
		out->ptr[--out->len] = '\0';

	widget->count_text[0] = '\0';
	if (!expanded && preview.hidden_lines)
		snprintf(widget->count_text, sizeof(widget->count_text), "+ %zu %s · ",
			preview.hidden_lines,
			preview.hidden_lines == 1 ? "line" : "lines");

	int visible = !state->details && (preview.hidden_lines || preview.clipped);
	report->footer_size = (struct nt_view_size){0};
	if (visible) {
		size_t count_width = nb_utf8_width_cstr(widget->count_text);
		report->footer_size = (struct nt_view_size){
			4 + (int)count_width + (int)strlen(expanded ? "show less" : "show more"), 1};
	}

	return 0;
}

static int cli_tool_call_publish(struct cli_report *report)
{
	struct cli_tool_call *widget = (struct cli_tool_call *)report;
	const struct cli_tool_call_state *state = nt_component_state_get(widget->state);
	int rc = nt_label_set_text(widget->count, widget->count_text);
	const struct nt_text_style link = state->styled
		? (struct nt_text_style){.attributes = NT_STYLE_UNDERLINE,
			.foreground = {NT_COLOR_ANSI, 6}} : (struct nt_text_style){0};
	if (!rc)
		rc = nt_view_set_style(widget->toggle, link);

	if (!rc)
		rc = nt_view_set_style(widget->count,
			(struct nt_text_style){.attributes = state->styled ? NT_STYLE_DIM : 0});

	return rc ? rc : nt_view_set_hidden(report->footer, !report->footer_size.height);
}

static int cli_tool_call_toggle_text(void *data, struct nt_value *result)
{
	return nt_value_set_string(result, nt_component_state_get_int(data) ? "show less"
		: "show more");
}

static int64_t cli_tool_call_invert(int64_t previous, int64_t argument)
{
	(void)argument;
	return !previous;
}

static int cli_tool_call_toggle(void *data, const struct nt_view_event *event)
{
	struct cli_tool_call *widget = data;
	if (event->type == NT_VIEW_EVENT_CLICK && event->input->mouse.button != NT_MOUSE_LEFT)
		return NT_VIEW_EVENT_PASS;

	if (event->type == NT_VIEW_EVENT_KEY &&
	    (event->input->pasting || event->input->action == NT_KEY_ACTION_RELEASE ||
	     (event->input->key != '\r' && event->input->key != '\n' &&
	      event->input->key != ' ')))
		return NT_VIEW_EVENT_PASS;

	int rc = nt_component_state_update_int(widget->expanded, cli_tool_call_invert, 0);
	if (!rc)
		cli_report_invalidate(&widget->report, widget->report.styled);

	return rc ? rc : NT_VIEW_EVENT_STOP;
}

static int cli_tool_call_pointer(void *data, const struct nt_view_event *event)
{
	(void)data;
	/* Keep the draft focused while NT synthesizes the link click. */
	return event->input->mouse.button == NT_MOUSE_LEFT;
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
	struct cli_tool_call *widget = data;
	const struct cli_tool_call_state *state = nt_component_state_get(widget->state);
	return nt_value_set_bool(result, !state->details &&
		(nt_component_state_get_bool(widget->grouped) ||
		 cli_agent_tools_analytics_only(state->snapshot->tool.name)));
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
		.width = NT_FILL,
		.margin = {.top = 1}, .padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_tool_call *widget = (struct cli_tool_call *)cli_report_add(
		builder, parent, &cli_tool_call_class, &options, styled, cli_tool_call_format);
	if (!widget)
		return NULL;

	struct nt_view *view = &widget->report.base;
	widget->report.publish = cli_tool_call_publish;
	const struct nt_view_options footer = {.height = 1, .padding = {.left = 4}};
	widget->report.footer = nt_row_add(builder, view, 0, &footer);
	widget->count = nt_label_add(builder, widget->report.footer, "", NULL);
	widget->toggle = nt_label_add(builder, widget->report.footer, "show more", NULL);
	if (nt_builder_error(builder))
		return NULL;

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
		rc = nt_view_use_state_int(view, 0, &widget->expanded);

	if (!rc)
		rc = nt_view_use_state_bool(view, false, &widget->grouped);

	if (!rc)
		rc = nt_view_bind_compute(view, NT_PROP_HIDDEN, cli_tool_call_hidden, widget);

	if (!rc)
		rc = nt_view_bind_compute(widget->toggle, NT_PROP_TEXT, cli_tool_call_toggle_text,
			widget->expanded);

	if (!rc)
		rc = nt_label_set_wrap_mode(widget->count, NT_TEXT_WRAP_NONE);

	if (!rc)
		rc = nt_label_set_wrap_mode(widget->toggle, NT_TEXT_WRAP_NONE);

	if (!rc)
		rc = nt_view_set_selectable(widget->toggle, 0);

	if (!rc)
		rc = nt_view_set_focusable(widget->toggle, 1);

	if (!rc && (!nt_view_on(widget->toggle, NT_VIEW_EVENT_CLICK,
				cli_tool_call_toggle, widget) ||
		    !nt_view_on(widget->toggle, NT_VIEW_EVENT_KEY, cli_tool_call_toggle, widget) ||
		    !nt_view_on_full(widget->toggle, NT_VIEW_EVENT_MOUSE,
			&(struct nt_view_listener_options){.flags = NT_VIEW_LISTEN_CAPTURE,
				NT_VIEW_LISTENER(cli_tool_call_pointer, widget)})))
		rc = NB_ERR_NOMEM;

	nt_builder_fail(builder, rc);
	return view;
}

int cli_tool_call_set_grouped(struct nt_view *view, int grouped)
{
	if (!view || view->type != &cli_tool_call_class)
		return NB_ERR_INVAL;

	return nt_component_state_set_bool(((struct cli_tool_call *)view)->grouped, grouped != 0);
}
