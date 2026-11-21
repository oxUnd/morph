#include "sapi/cli/pages/chat.h"

#include "config/config.h"
#include "credits.h"
#include "runtime/runtime.h"
#include "sapi/cli/adapters/clipboard/clipboard.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/commands/media.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/components/banner.h"
#include "sapi/cli/components/command.h"
#include "sapi/cli/components/composer.h"
#include "sapi/cli/components/status_bar.h"
#include "sapi/cli/components/file_browser.h"
#include "sapi/cli/components/list.h"
#include "sapi/cli/components/mcp_tree.h"
#include "sapi/cli/pages/mcp_status.h"
#include "sapi/cli/components/media.h"
#include "sapi/cli/components/message.h"
#include "sapi/cli/components/notice.h"
#include "sapi/cli/components/plan.h"
#include "sapi/cli/components/recap.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/components/rich_document.h"
#include "sapi/cli/components/skill_tree.h"
#include "sapi/cli/components/tool_call.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/models/files.h"
#include "sapi/cli/models/jobs.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/pages/resume.h"
#include "sapi/cli/pages/model.h"
#include "sapi/cli/pages/turn_details.h"
#include "sapi/cli/presentation/agents.h"
#include "sapi/cli/presentation/composer_footer.h"
#include "sapi/cli/presentation/feed.h"
#include "sapi/cli/presentation/feed_block.h"
#include "sapi/cli/presentation/jobs.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/panel.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/presentation/work_status.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/services/composer.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/services/sessions.h"
#include "sapi/cli/services/permissions.h"
#include "sapi/cli/services/transcript.h"
#include "sapi/cli/services/turn.h"
#include "terminal/text.h"

#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/map.h>
#include <nb/time.h>
#include <nothing/tui/animation.h>
#include <nothing/tui/choice_prompt.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/shortcut.h>
#include <nothing/tui/terminal.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/split_view.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/window.h>

#include <time.h>
#include <unistd.h>

#define CLI_CHAT_BOTTOM_LABEL "↓ Back to bottom · Esc"
#define CLI_CHAT_FILES_ANIMATION_CHANNEL 1u
#define CLI_CHAT_FILES_ANIMATION_MS 180

struct cli_chat_panel {
	struct nt_stack_view base;
	struct nt_view *focus;
	void *data;
	cli_chat_panel_complete_fn complete;
	int (*refresh)(void *, const struct nt_event *);
	int result, closed;
};

struct cli_chat_overlay {
	struct nt_view base;
	struct nt_view *content;
	void *data;
	cli_chat_panel_complete_fn complete;
	int (*refresh)(void *, const struct nt_event *);
};

struct cli_chat_page {
	struct nt_view_controller base;
	struct nt_builder *builder;
	struct nt_view *workspace, *primary, *files_view;
	struct cli_files_model files;
	char files_workdir[PATH_MAX];
	int64_t files_session;
	int files_open, files_focus;
	struct nt_component_state *modal;
	struct nt_view *scroll, *feed, *composer, *editor, *status, *footer;
	struct nt_view *bottom_button;
	struct nt_view *panel_host, *output_container, *overlay_host;
	struct cli_chat_panel *panel;
	struct cli_chat_overlay *overlay;
	int overlay_height;
	struct nt_view *stream_view, *output_view, *queue, *jobs, *agents;
	struct nt_view *last_assistant, *command_group, *command_tail;
	char *command_input;
	int command_running;
	char *last_assistant_text;
	char *footer_text;
	char *activity_text;
	nb_buf stream, output, agent_summary;
	int64_t queue_revision;
	uint64_t jobs_revision, agents_revision;
	int activity_columns, activity_styled, status_ultra;
	int64_t next_jobs_update, next_status_update;
	nb_map *tools;
	nb_vec history;
	char *draft;
	size_t history_index;
	int stream_kind, ready, columns, agents_waiting, prompt_rows;
	int64_t turn_started;
};

struct cli_chat_session_state {
	struct nt_view *scroll, *feed, *stream_view, *output_view, *output_container;
	struct nt_view *last_assistant, *command_group, *command_tail;
	char *last_assistant_text;
	char *input;
	size_t cursor;
	nb_buf stream, output;
	nb_map *tools;
	int stream_kind, agents_waiting;
	int64_t turn_started;
	struct cli_files_model files;
	char files_workdir[PATH_MAX];
	int64_t files_session;
};

static int cli_chat_page_footer(struct cli_chat_page *page, int columns);

static int64_t cli_chat_now(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? now / 1000000 : 0;
}

static void cli_chat_files_session(struct cli_chat_page *page)
{
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	int64_t session = 0;
	(void)runtime_session_current_id(ctx->runtime, &session);
	if (page->files_session == session)
		return;

	cli_files_cleanup(&page->files);
	cli_files_init(&page->files);
	page->files_session = session;
	struct session current = {0};
	const char *workdir = !runtime_session_current(ctx->runtime, &current) && current.workdir[0]
		? current.workdir : ctx->workdir;
	(void)nb_str_copy_to(nb_str_from_cstr(workdir), page->files_workdir,
		sizeof(page->files_workdir));
	if (page->files_view)
		cli_file_browser_reset(page->files_view);
}

static int cli_chat_files_markdown(struct cli_application *ctx, const char *text)
{
	cli_chat_files_session(ctx->chat);
	return cli_files_markdown(&ctx->chat->files, ctx->chat->files_workdir, text);
}

static int cli_chat_files_animate_ratio(struct nt_view *view, double value, void *data)
{
	(void)data;
	return nt_split_view_set_ratio(view, (int)(value + 0.5));
}

static int cli_chat_files_reveal(struct cli_chat_page *page)
{
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	struct nt_terminal_environment environment = {0};
	(void)nt_application_environment(&ctx->base, &environment);
	if (environment.columns < 100 || !cli_output_color_enabled(ctx))
		return NB_OK;

	int ratio = nt_split_view_ratio(page->workspace);
	if (ratio == 99)
		return NB_OK;

	int rc = nt_split_view_set_ratio(page->workspace, 99);
	if (!rc)
		rc = nt_view_animate(page->workspace, CLI_CHAT_FILES_ANIMATION_CHANNEL,
			&(struct nt_animation_options){.from = 99, .to = ratio,
				.duration_ms = CLI_CHAT_FILES_ANIMATION_MS,
				.easing = NT_EASE_OUT, .update = cli_chat_files_animate_ratio});

	if (rc)
		(void)nt_split_view_set_ratio(page->workspace, ratio);

	return rc;
}

static int cli_chat_files_pointer(void *data, const struct nt_view_event *event)
{
	struct cli_chat_page *page = data;
	if (!event->input || event->input->mouse.action != NT_MOUSE_PRESS ||
	    event->input->mouse.button != NT_MOUSE_LEFT ||
	    !nt_view_animation_running(page->workspace, CLI_CHAT_FILES_ANIMATION_CHANNEL))
		return 0;

	const struct nt_view_class *methods = nt_view_class_methods(page->workspace);
	if (methods->pointer_shape(page->workspace, event->input->mouse.x,
		event->input->mouse.y) != NT_POINTER_DEFAULT)
		return nt_view_cancel_animation(page->workspace,
			CLI_CHAT_FILES_ANIMATION_CHANNEL, 0);

	return 0;
}

static int cli_chat_files_close(struct cli_chat_page *page)
{
	int rc = nt_view_cancel_animation(page->workspace, CLI_CHAT_FILES_ANIMATION_CHANNEL, 1);
	if (rc)
		return rc;

	page->files_open = page->files_focus = 0;
	rc = nt_view_set_hidden(page->files_view, 1);
	if (!rc)
		rc = nt_view_set_hidden(page->primary, 0);

	return rc ? rc : nt_view_focus(page->editor);
}

static int cli_chat_files_action(void *data, const struct nt_view_event *event)
{
	struct cli_chat_page *page = data;
	if (event->target != page->files_view)
		return 0;

	if (event->type == NT_VIEW_EVENT_CANCEL) {
		int rc = cli_chat_files_close(page);
		return rc ? rc : NT_VIEW_EVENT_HANDLED;
	}

	if (event->type == NT_VIEW_EVENT_OPEN) {
		if (event->index == SIZE_MAX) {
			int rc = cli_chat_files_show(cli_application_from_controller(&page->base),
				event->text);
			return rc ? rc : NT_VIEW_EVENT_HANDLED;
		}

		const struct cli_files_entry *entry = nb_vec_at(&page->files.entries, event->index);
		int line = entry ? entry->line : 0;
		int rc = event->text ? cli_file_browser_open(page->files_view, event->text, line) : 0;
		if (!rc)
			rc = cli_file_browser_focus(page->files_view);

		return rc ? rc : NT_VIEW_EVENT_HANDLED;
	}

	return 0;
}

static int cli_chat_files_link(void *data, const struct nt_view_event *event)
{
	struct cli_chat_page *page = data;
	if (!event->text || strcmp(nt_view_kind(event->target), "cli_rich_document"))
		return 0;

	struct cli_application *ctx = cli_application_from_controller(&page->base);
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = cli_files_resolve(page->files_workdir, event->text, &path, NULL);
	if (rc)
		return 0;

	rc = cli_chat_files_show(ctx, event->text);
	return rc ? rc : NT_VIEW_EVENT_HANDLED;
}

int cli_chat_files_show(struct cli_application *ctx, const char *url)
{
	if (!cli_chat_active(ctx))
		return NB_ERR_UNSUPPORTED;

	struct cli_chat_page *page = ctx->chat;
	cli_chat_files_session(page);
	int opening = !page->files_open;
	page->files_open = page->files_focus = 1;
	int rc = cli_file_browser_sync(page->files_view);
	if (!rc)
		rc = nt_view_set_hidden(page->files_view, 0);

	if (!rc && url) {
		nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
		int line = 0;
		rc = cli_files_resolve(page->files_workdir, url, &path, &line);
		if (!rc)
			rc = cli_files_add(&page->files, page->files_workdir,
				url, CLI_FILES_REFERENCED);

		if (!rc)
			rc = cli_file_browser_open(page->files_view, nb_buf_cstr(&path), line);
	}

	if (!rc)
		rc = cli_file_browser_sync(page->files_view);

	if (!rc)
		rc = url ? cli_file_browser_focus(page->files_view)
			: cli_file_browser_list(page->files_view);

	if (!rc && opening)
		rc = cli_chat_files_reveal(page);

	return rc;
}

int cli_chat_files_event(struct cli_application *ctx, const struct morph_event *event)
{
	if (!cli_chat_active(ctx) || !event->name || strcmp(event->name, "output.created"))
		return NB_OK;

	const char *path = cJSON_GetStringValue(
		cJSON_GetObjectItemCaseSensitive(event->data, "path"));
	const cJSON *session = cJSON_GetObjectItemCaseSensitive(event->data, "session_id");
	cli_chat_files_session(ctx->chat);
	if (!path || (cJSON_IsNumber(session) &&
		(int64_t)session->valuedouble != ctx->chat->files_session))
		return NB_OK;

	return cli_files_add(&ctx->chat->files, ctx->chat->files_workdir,
		path, CLI_FILES_GENERATED);
}

int cli_chat_active(const struct cli_application *ctx)
{
	return ctx && ctx->chat && ctx->chat->ready;
}

int cli_chat_yield_input(struct cli_application *ctx)
{
	return cli_chat_active(ctx) ? nt_text_field_reset_input(ctx->chat->editor) : 0;
}

int cli_chat_panel_active(const struct cli_application *ctx)
{
	return cli_chat_active(ctx) && ctx->chat->panel && ctx->chat->panel->complete;
}

static int cli_chat_switch_input(struct cli_chat_page *page, struct nt_view *focus)
{
	int rc = nt_component_state_set_bool(page->modal, page->panel || page->prompt_rows);
	if (!rc)
		rc = nt_view_flush_state(nt_view_controller_root_view(&page->base));

	return rc || !focus ? rc : nt_view_focus(focus);
}

static int cli_chat_activity_hidden(void *data, struct nt_value *value)
{
	struct nt_view *row = data;
	struct cli_chat_page *page = (struct cli_chat_page *)nt_view_controller(row);
	return nt_value_set_bool(value, nt_component_state_get_bool(page->modal) ||
		(row != page->status && !*nt_text_view_text(row)));
}

static int cli_chat_queue_hidden(void *data, struct nt_value *value)
{
	struct cli_chat_page *page = data;
	return nt_value_set_bool(value, nt_component_state_get_bool(page->modal) ||
						!nt_list_view_count(page->queue));
}

static int cli_chat_sync_bottom_button(struct cli_chat_page *page)
{
	const struct nt_scroll_state *scroll = nt_scroll_view_state(page->scroll);
	int hidden = !scroll->viewport || scroll->offset == nt_scroll_max(scroll) ||
		     page->panel || page->prompt_rows;
	return nt_view_set_hidden(page->bottom_button, hidden);
}

static int cli_chat_feed_scrolled(void *data, const struct nt_view_event *event)
{
	struct cli_chat_page *page = data;
	return event->target == page->scroll ? cli_chat_sync_bottom_button(page) : 0;
}

static int cli_chat_bottom_activate(void *data, const struct nt_view_event *event)
{
	struct cli_chat_page *page = data;
	(void)event;
	int rc = nt_scroll_view_handle_key(page->scroll, NT_KEY_END);
	if (rc < 0)
		return rc;

	rc = nt_view_focus(page->editor);
	return rc ? rc : NT_VIEW_EVENT_HANDLED;
}

static int cli_chat_bottom_pointer(void *data, const struct nt_view_event *event)
{
	(void)data;
	/* Let the button receive the synthesized click while the editor keeps focus. */
	return event->input->mouse.button == NT_MOUSE_LEFT;
}

int cli_chat_prompt_choice(struct cli_application *ctx, int rows, struct nt_selection_model *model,
			   struct nt_choice_list_state *state,
			   const struct nt_choice_prompt_options *options, FILE *input,
			   char **custom)
{
	if (!cli_chat_active(ctx))
		return nt_choice_prompt_run(model, state, options, input, stdout, custom);

	struct cli_chat_page *page = ctx->chat;
	if (rows < 1 || page->panel || page->prompt_rows)
		return NB_ERR_INVAL;

	page->prompt_rows = rows;
	int rc = cli_chat_switch_input(page, NULL);
	if (!rc)
		rc = nt_application_render(&ctx->base);

	if (!rc) {
		struct nt_view_rect frame = nt_view_frame(page->panel_host);
		rc = nt_terminal_clear_below(stdout, frame.y + 2, frame.x + 1);
	}

	if (!rc)
		rc = nt_choice_prompt_run(model, state, options, input, stdout, custom);

	page->prompt_rows = 0;
	int focus_rc = cli_chat_switch_input(page, page->editor);
	int terminal_rc = nt_application_restore_terminal(&ctx->base);
	int render_rc = nt_application_render(&ctx->base);
	if (rc)
		return rc;

	if (focus_rc)
		return focus_rc;

	return terminal_rc ? terminal_rc : render_rc;
}

static void cli_chat_panel_deinit(struct nt_view *view)
{
	struct cli_chat_panel *panel = (struct cli_chat_panel *)view;
	if (!panel->complete)
		return;

	struct cli_application *ctx = cli_application_from_view(view);
	panel->complete(ctx, panel->data, panel->closed ? panel->result : NB_ERR_CANCELLED);
	if (panel->closed)
		cli_application_panel_finished(ctx, panel->result);
}

int cli_chat_panel_mount(struct cli_application *ctx, cli_chat_panel_build_fn build, void *data,
			 cli_chat_panel_complete_fn complete)
{
	if (!cli_chat_active(ctx) || !build || !complete)
		return NB_ERR_INVAL;

	if (!ctx->running)
		return NB_ERR_CANCELLED;

	struct cli_chat_page *page = ctx->chat;
	int overlay_rc = cli_chat_overlay_close(ctx);
	if (overlay_rc < 0)
		return overlay_rc;

	if (nt_view_child_count(page->panel_host))
		return NB_ERR_BUSY;

	static const struct nt_view_class type = {
		.superclass = &nt_stack_view_class,
		.instance_size = sizeof(struct cli_chat_panel),
		.deinit = cli_chat_panel_deinit,
		.kind = "cli_chat_panel",
	};
	struct nt_view *container = nt_view_add(page->builder, page->panel_host, &type, NULL);
	if (!container)
		return nt_builder_error(page->builder);

	struct cli_chat_panel *panel = (struct cli_chat_panel *)container;
	panel->focus = build(page->builder, container, data);
	int rc = nt_builder_error(page->builder);
	if (!rc && !panel->focus)
		rc = NB_ERR_NOMEM;

	if (!rc) {
		page->panel = panel;
		rc = cli_chat_switch_input(page, panel->focus);
	}

	if (rc) {
		page->panel = NULL;
		(void)nt_view_clear_children(page->panel_host);
		(void)cli_chat_switch_input(page, page->editor);
		return rc;
	}

	panel->data = data;
	panel->complete = complete;
	return 0;
}

void cli_chat_panel_set_refresh(struct cli_application *ctx,
				int (*refresh)(void *, const struct nt_event *))
{
	if (cli_chat_panel_active(ctx))
		ctx->chat->panel->refresh = refresh;
}

int cli_chat_panel_close(struct cli_application *ctx, int result)
{
	if (!cli_chat_panel_active(ctx))
		return 0;

	struct cli_chat_page *page = ctx->chat;
	struct cli_chat_panel *panel = page->panel;
	page->panel = NULL;
	int rc = cli_chat_switch_input(page, page->editor);
	panel->closed = 1;
	panel->result = result ? result : rc;
	int clear_rc = nt_view_clear_children(page->panel_host);
	if (rc || clear_rc)
		return rc ? rc : clear_rc;

	return 1;
}

static void cli_chat_overlay_deinit(struct nt_view *view)
{
	struct cli_chat_overlay *overlay = (struct cli_chat_overlay *)view;
	if (overlay->complete)
		overlay->complete(cli_application_from_view(view), overlay->data, 0);
}

int cli_chat_overlay_mount(struct cli_application *ctx, cli_chat_panel_build_fn build,
	void *data, cli_chat_panel_complete_fn complete)
{
	if (!cli_chat_active(ctx) || !build || !complete)
		return NB_ERR_INVAL;

	struct cli_chat_page *page = ctx->chat;
	if (page->overlay || page->panel || !ctx->running)
		return NB_ERR_BUSY;

	static const struct nt_view_class type = {
		.superclass = &nt_view_base_class,
		.instance_size = sizeof(struct cli_chat_overlay),
		.deinit = cli_chat_overlay_deinit,
		.kind = "cli_chat_overlay",
	};
	struct nt_view *container = nt_view_add(page->builder, page->overlay_host, &type,
		&(struct nt_view_options){.width = NT_FILL, .height = NT_FILL});
	if (!container)
		return nt_builder_error(page->builder);

	struct cli_chat_overlay *overlay = (struct cli_chat_overlay *)container;
	overlay->content = build(page->builder, container, data);
	int rc = nt_builder_error(page->builder);
	if (!rc && !overlay->content)
		rc = NB_ERR_NOMEM;

	if (!rc)
		rc = nt_view_set_hidden(page->overlay_host, 0);

	if (rc) {
		(void)nt_view_clear_children(page->overlay_host);
		return rc;
	}

	overlay->data = data;
	overlay->complete = complete;
	page->overlay = overlay;
	return 0;
}

int cli_chat_overlay_close(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx) || !ctx->chat->overlay)
		return 0;

	struct cli_chat_page *page = ctx->chat;
	page->overlay = NULL;
	int rc = nt_view_set_hidden(page->overlay_host, 1);
	int clear_rc = nt_view_clear_children(page->overlay_host);
	return rc ? rc : clear_rc ? clear_rc : 1;
}

void cli_chat_overlay_set_refresh(struct cli_application *ctx,
	int (*refresh)(void *, const struct nt_event *))
{
	if (cli_chat_active(ctx) && ctx->chat->overlay)
		ctx->chat->overlay->refresh = refresh;
}

int cli_chat_reset(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return 0;

	struct cli_chat_page *page = ctx->chat;
	page->stream_view = NULL;
	page->output_view = NULL;
	page->output_container = NULL;
	page->stream_kind = 0;
	page->last_assistant = NULL;
	page->command_group = NULL;
	page->command_tail = NULL;
	free(page->last_assistant_text);
	page->last_assistant_text = NULL;
	nb_buf_clear(&page->stream);
	nb_buf_clear(&page->output);
	nb_map_clear(page->tools);
	int rc = nt_view_clear_children(page->feed);
	if (!rc)
		rc = nt_scroll_view_follow(page->scroll, 1);

	return rc;
}

void cli_chat_session_state_destroy(struct cli_chat_session_state *state)
{
	if (!state)
		return;

	nb_map_free(state->tools);
	nb_buf_cleanup(&state->stream);
	nb_buf_cleanup(&state->output);
	cli_files_cleanup(&state->files);
	free(state->input);
	free(state->last_assistant_text);
	free(state);
}

int cli_chat_session_save(struct cli_application *ctx, struct cli_chat_session_state **out)
{
	if (!out || *out)
		return NB_ERR_INVAL;

	if (!cli_chat_active(ctx))
		return 0;

	struct cli_chat_page *page = ctx->chat;
	struct cli_chat_session_state *state = calloc(1, sizeof(*state));
	if (!state)
		return NB_ERR_NOMEM;

	cli_files_init(&state->files);
	state->input = strdup(nt_text_field_text(page->editor));
	if (!state->input) {
		cli_chat_session_state_destroy(state);
		return NB_ERR_NOMEM;
	}

	int rc = nt_view_set_hidden(page->scroll, 1);
	if (rc) {
		cli_chat_session_state_destroy(state);
		return rc;
	}

	state->cursor = nt_text_field_cursor(page->editor);
	state->scroll = page->scroll;
	state->feed = page->feed;
	state->stream_view = page->stream_view;
	state->output_view = page->output_view;
	state->output_container = page->output_container;
	state->last_assistant = page->last_assistant;
	state->command_group = NULL;
	state->command_tail = page->command_tail;
	state->last_assistant_text = page->last_assistant_text;
	page->last_assistant_text = NULL;
	state->tools = page->tools;
	page->tools = NULL;
	nb_buf_move(&state->stream, &page->stream);
	nb_buf_move(&state->output, &page->output);
	state->stream_kind = page->stream_kind;
	state->agents_waiting = page->agents_waiting;
	state->turn_started = page->turn_started;
	cli_files_move(&state->files, &page->files);
	state->files_session = page->files_session;
	memcpy(state->files_workdir, page->files_workdir, sizeof(state->files_workdir));
	page->files_session = 0;
	cli_file_browser_reset(page->files_view);
	page->scroll = NULL;
	page->feed = NULL;
	page->stream_view = NULL;
	page->output_view = NULL;
	page->output_container = NULL;
	page->last_assistant = NULL;
	page->command_group = NULL;
	page->command_tail = NULL;
	page->stream_kind = 0;
	page->agents_waiting = 0;
	page->turn_started = 0;
	*out = state;
	return 0;
}

static void cli_chat_session_invalidate(struct cli_chat_page *page)
{
	free(page->footer_text);
	page->footer_text = NULL;
	free(page->activity_text);
	page->activity_text = NULL;
	page->next_status_update = 0;
	page->next_jobs_update = 0;
	page->jobs_revision = 0;
	page->agents_revision = 0;
	page->queue_revision = -1;
}

int cli_chat_session_restore(struct cli_application *ctx, struct cli_chat_session_state **owner)
{
	if (!cli_chat_active(ctx))
		return 0;

	if (!owner || !*owner)
		return NB_ERR_INVAL;

	struct cli_chat_page *page = ctx->chat;
	struct cli_chat_session_state *state = *owner;
	page->scroll = state->scroll;
	page->feed = state->feed;
	page->stream_view = state->stream_view;
	page->output_view = state->output_view;
	page->output_container = state->output_container;
	page->last_assistant = state->last_assistant;
	page->command_group = state->command_group;
	page->command_tail = state->command_tail;
	page->last_assistant_text = state->last_assistant_text;
	state->last_assistant_text = NULL;
	page->tools = state->tools;
	state->tools = NULL;
	nb_buf_move(&page->stream, &state->stream);
	nb_buf_move(&page->output, &state->output);
	page->stream_kind = state->stream_kind;
	page->agents_waiting = state->agents_waiting;
	page->turn_started = state->turn_started;
	cli_files_move(&page->files, &state->files);
	page->files_session = state->files_session;
	memcpy(page->files_workdir, state->files_workdir, sizeof(page->files_workdir));
	cli_file_browser_reset(page->files_view);
	int rc = nt_view_set_hidden(page->scroll, 0);
	if (!rc)
		rc = nt_text_field_set_text(page->editor, state->input);

	if (!rc)
		rc = nt_text_field_set_cursor(page->editor, state->cursor);

	cli_chat_session_invalidate(page);
	cli_chat_session_state_destroy(state);
	*owner = NULL;
	return rc;
}

int cli_chat_session_empty(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return 0;

	struct cli_chat_page *page = ctx->chat;
	if (page->scroll)
		return cli_chat_reset(ctx);

	page->tools = nb_map_new_with_cap(16);
	if (!page->tools)
		return NB_ERR_NOMEM;

	page->scroll = nt_scroll_view_add(page->builder, page->primary, NULL);
	if (!page->scroll)
		return nt_builder_error(page->builder);

	int rc = nt_view_set_focusable(page->scroll, 0);
	if (!rc)
		rc = nt_scroll_view_set_scrollbars(page->scroll, 0);

	if (!rc)
		rc = nt_scroll_view_follow(page->scroll, 1);

	page->feed = nt_column_add(page->builder, page->scroll, 0,
		&(struct nt_view_options){.width = NT_FILL});
	if (!nt_view_on(page->scroll, NT_VIEW_EVENT_SCROLLED, cli_chat_feed_scrolled, page))
		return NB_ERR_NOMEM;

	if (!rc)
		rc = nt_text_field_set_text(page->editor, "");

	cli_chat_session_invalidate(page);
	return rc ? rc : nt_builder_error(page->builder);
}

int cli_chat_session_refresh(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return 0;

	cli_chat_session_invalidate(ctx->chat);
	return cli_chat_page_footer(ctx->chat, ctx->chat->columns);
}

int cli_chat_session_command_copy(struct cli_application *ctx, char **out)
{
	if (!out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	if (!cli_chat_active(ctx) || !ctx->chat->command_input)
		return 0;

	*out = strdup(ctx->chat->command_input);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int cli_chat_page_error(struct cli_chat_page *page)
{
	return nt_builder_error(page->builder);
}

static void cli_chat_page_boundary(struct cli_chat_page *page)
{
	page->stream_view = NULL;
	page->output_view = NULL;
	page->output_container = NULL;
	page->stream_kind = 0;
	nb_buf_clear(&page->stream);
	nb_buf_clear(&page->output);
}

void cli_chat_transcript_reset(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return;

	cli_chat_page_boundary(ctx->chat);
	nb_map_clear(ctx->chat->tools);
	ctx->chat->last_assistant = NULL;
	free(ctx->chat->last_assistant_text);
	ctx->chat->last_assistant_text = NULL;
}

int cli_chat_message(struct cli_application *ctx, int kind, const char *text)
{
	if (!cli_chat_active(ctx))
		return 0;

	struct cli_chat_page *page = ctx->chat;
	if (kind != CLI_MESSAGE_REASONING) {
		int rc = cli_chat_files_markdown(ctx, text);
		if (rc)
			return rc;
	}

	cli_chat_page_boundary(page);
	page->last_assistant = NULL;
	struct nt_view *view = cli_message_add_borrow(
		page->builder, page->feed, (enum cli_message_kind)kind,
			       text ? text : "", cli_output_color_enabled(ctx));
	if (view && kind == CLI_MESSAGE_USER)
		nt_builder_fail(page->builder,
			cli_message_set_ultra(view, ctx->composer.ultra));

	return cli_chat_page_error(page);
}

int cli_chat_media(struct cli_application *ctx, const char *type, const char *path)
{
	if (!cli_chat_active(ctx) || !type || !path)
		return NB_ERR_INVAL;

	struct cli_chat_page *page = ctx->chat;
	cli_chat_files_session(page);
	int rc = cli_files_add(&page->files, page->files_workdir, path, CLI_FILES_GENERATED);
	if (rc)
		return rc;

	cli_chat_page_boundary(page);
	const struct nt_view_options options = {
		.margin = {.top = page->command_group ? 0 : 1}};
	cli_media_add(page->builder, page->command_group ? page->command_group : page->feed,
		      cli_output_color_enabled(ctx), type, path, &options);
	return cli_chat_page_error(page);
}

static struct nt_view *cli_chat_page_report_parent(struct cli_chat_page *page)
{
	cli_chat_page_boundary(page);
	if (page->command_group)
		return page->command_group;

	return nt_stack_view_add(page->builder, page->feed, NT_STACK_VIEW_VERTICAL, 0,
				 &(struct nt_view_options){.margin = {.top = 1}});
}

int cli_chat_skills(struct cli_application *ctx, const struct cli_skill_list_row *skills,
		    size_t count)
{
	struct cli_chat_page *page = ctx->chat;
	/* Keep the existing result heading and tree in the same output group. */
	struct nt_view *group = page->output_container;
	cli_chat_page_boundary(page);
	if (!group)
		group = cli_chat_page_report_parent(page);

	cli_skill_tree_add(page->builder, group, skills, count);
	return cli_chat_page_error(page);
}

int cli_chat_mcp_servers(struct cli_application *ctx, const struct runtime_mcp_status *servers,
	size_t count)
{
	if (!cli_chat_active(ctx))
		return NB_ERR_INVAL;

	struct cli_chat_page *page = ctx->chat;
	struct nt_view *parent = cli_chat_page_report_parent(page);
	cli_mcp_tree_servers_add_borrow(page->builder, parent, servers, count,
		cli_output_color_enabled(ctx));
	return cli_chat_page_error(page);
}

int cli_chat_mcp_items(struct cli_application *ctx, const char *kind, const char *server,
	const struct cli_mcp_tree_item *items, size_t count)
{
	if (!cli_chat_active(ctx))
		return NB_ERR_INVAL;

	struct cli_chat_page *page = ctx->chat;
	struct nt_view *parent = cli_chat_page_report_parent(page);
	cli_mcp_tree_items_add_borrow(page->builder, parent, kind, server, items, count,
		cli_output_color_enabled(ctx));
	return cli_chat_page_error(page);
}

int cli_chat_command_input(struct cli_application *ctx, const char *text)
{
	if (!cli_chat_active(ctx) || !text)
		return NB_ERR_INVAL;

	struct cli_chat_page *page = ctx->chat;
	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(text);
	if (!copy)
		return NB_ERR_NOMEM;

	if (!ctx->turn_active)
		cli_chat_page_boundary(page);
	page->command_group = cli_command_add(page->builder, page->feed, text,
					    cli_output_color_enabled(ctx));
	page->command_tail = page->command_group;
	int rc = cli_chat_page_error(page);
	if (!rc) {
		free(page->command_input);
		page->command_input = nb_charp_steal(&copy);
	}

	return rc;
}

void cli_chat_command_end(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return;

	ctx->chat->command_group = NULL;
	free(ctx->chat->command_input);
	ctx->chat->command_input = NULL;
	if (!ctx->turn_active)
		cli_chat_page_boundary(ctx->chat);
	else {
		ctx->chat->output_view = NULL;
		ctx->chat->output_container = NULL;
		nb_buf_clear(&ctx->chat->output);
	}
}

int cli_chat_output(struct cli_application *ctx, const char *text)
{
	if (!cli_chat_active(ctx) || !text || !*text)
		return 0;

	struct cli_chat_page *page = ctx->chat;
	if (!page->output_view) {
		const struct nt_view_options options = {
			.margin = {.top = page->command_group ? 0 : 1}};
		struct nt_view *container = nt_stack_view_add(
			page->builder, page->command_group ? page->command_group : page->feed,
			NT_STACK_VIEW_VERTICAL, 0, &options);
		page->output_container = container;
		page->output_view = cli_rich_document_add_borrow(page->builder, container);
		if (!page->output_view)
			return cli_chat_page_error(page);
	}

	int rc = (nb_buf_append_cstr(&page->output, text) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_rich_document_set_rendered_copy(page->output_view,
							 nb_buf_cstr(&page->output),
							 cli_output_color_enabled(ctx));

	return rc == NB_ERR_AGAIN ? 0 : rc;
}

void cli_chat_activity(struct cli_application *ctx, const char *text)
{
	if (cli_chat_active(ctx)) {
		struct cli_chat_page *page = ctx->chat;
		if (text && *text && page->activity_text && !strcmp(text, page->activity_text))
			return;

		if ((!text || !*text) && !page->activity_text &&
		    (ctx->turn_active || !*nt_text_view_text(page->status)))
			return;

		page->next_status_update = 0;
		char *copy = text && *text ? strdup(text) : NULL;
		if (text && *text && !copy) {
			nt_builder_fail(page->builder, NB_ERR_NOMEM);
			return;
		}

		int rc = cli_messages_set_rendered_view(page->status, text ? text : "",
							cli_output_color_enabled(ctx));
		if (rc) {
			free(copy);
			nt_builder_fail(page->builder, rc);
			return;
		}

		free(page->activity_text);
		page->activity_text = copy;
	}
}

void cli_chat_turn_begin(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return;

	cli_chat_page_boundary(ctx->chat);
	ctx->chat->agents_waiting = 0;
	ctx->chat->turn_started = cli_chat_now();
	ctx->chat->next_status_update = 0;
	cli_chat_activity(ctx, "");
}

void cli_chat_turn_end(struct cli_application *ctx, int result)
{
	if (!cli_chat_active(ctx))
		return;

	struct cli_chat_page *page = ctx->chat;
	page->agents_waiting = 0;
	cli_chat_page_boundary(page);
	nb_buf receipt NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = cli_work_status_completed(&receipt, page->columns,
		cli_output_color_enabled(ctx), result, cli_chat_now() - page->turn_started,
		time(NULL));
	nt_builder_fail(page->builder, rc);
	const struct nt_view_options options = {.margin = {.top = 1, .bottom = 1},
						       .padding = {.left = 2}};
	if (!rc) {
		struct nt_view *row = cli_report_text_add(page->builder, page->feed, &options);
		if (row) {
			nt_builder_fail(
				page->builder,
				cli_messages_set_rendered_view(row, nb_buf_cstr(&receipt),
							       cli_output_color_enabled(ctx)));
		}
	}

	cli_chat_activity(ctx, "");
}

int cli_chat_tool_changed(struct cli_application *ctx, const struct cli_transcript_tool *tool)
{
	if (!cli_chat_active(ctx))
		return 0;

	struct cli_chat_page *page = ctx->chat;
	cli_chat_files_session(page);
	int files_rc = cli_files_tool(&page->files, page->files_workdir, tool);
	if (files_rc)
		return files_rc;

	struct nt_view *view = nb_map_get_cstr(page->tools, tool->id);
	int rc;
	if (!view) {
		cli_chat_page_boundary(page);
		page->last_assistant = NULL;
		view = cli_tool_call_add_borrow(page->builder, page->feed, tool,
						cli_output_color_enabled(ctx), ctx->tool_details);
		if (!view || cli_chat_page_error(page))
			return cli_chat_page_error(page);

		rc = nb_map_put_cstr(page->tools, tool->id, view, NULL) ? 0 : NB_ERR_NOMEM;
	} else
		rc = cli_tool_call_update(view, tool, cli_output_color_enabled(ctx),
					  ctx->tool_details);

	return rc;
}

int cli_chat_block(struct cli_application *ctx, const struct cli_feed_block *block)
{
	if (!cli_chat_active(ctx) || !block)
		return 0;

	if (block->kind == CLI_FEED_BLOCK_TOOL)
		return cli_chat_tool_changed(ctx, block->content.tool);

	if (block->kind == CLI_FEED_BLOCK_REASONING)
		return cli_chat_message(ctx, CLI_MESSAGE_REASONING, block->content.reasoning);

	struct cli_chat_page *page = ctx->chat;
	cli_chat_page_boundary(page);
	if (block->kind == CLI_FEED_BLOCK_NOTICE) {
		cli_notice_add_borrow(page->builder, page->feed, block->content.notice,
				      cli_output_color_enabled(ctx));
		return cli_chat_page_error(page);
	}

	if (block->kind == CLI_FEED_BLOCK_RECAP) {
		cli_recap_add_borrow(page->builder, page->feed, block->content.recap.summary,
			block->content.recap.next, cli_output_color_enabled(ctx));
		return cli_chat_page_error(page);
	}

	if (block->kind == CLI_FEED_BLOCK_PLAN) {
		cli_plan_add_borrow(page->builder, page->feed, block->content.plan,
				    cli_output_color_enabled(ctx));
		return cli_chat_page_error(page);
	}

	if (block->kind == CLI_FEED_BLOCK_AGENT_RECEIPT) {
		nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = cli_agents_receipt_append(&text, block->content.agent.id,
			block->content.agent.status, page->columns, cli_output_color_enabled(ctx));
		if (!rc)
			rc = cli_chat_output(ctx, nb_buf_cstr(&text));

		cli_chat_page_boundary(page);
		return rc;
	}

	return 0;
}

static int cli_chat_page_stream(struct cli_application *ctx, int kind, const char *text, int delta)
{
	struct cli_chat_page *page = ctx->chat;
	if (!page->stream_view || page->stream_kind != kind) {
		cli_chat_page_boundary(page);
		page->stream_kind = kind;
		page->stream_view = cli_message_add_borrow(page->builder, page->feed,
							   (enum cli_message_kind)kind, "",
							   cli_output_color_enabled(ctx));
		if (!page->stream_view)
			return cli_chat_page_error(page);
	}

	if (!delta)
		nb_buf_clear(&page->stream);

	int rc = (nb_buf_append_cstr(&page->stream, text ? text : "") ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_message_set_text_copy(page->stream_view, nb_buf_cstr(&page->stream));

	if (!rc && kind == CLI_MESSAGE_ASSISTANT) {
		char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&page->stream));
		if (!copy)
			return NB_ERR_NOMEM;

		free(page->last_assistant_text);
		page->last_assistant_text = nb_charp_steal(&copy);
		page->last_assistant = page->stream_view;
	}

	return rc;
}

int cli_chat_react_event(struct cli_application *ctx, const struct morph_event *event)
{
	if (!cli_chat_active(ctx) || event->type != MORPH_EVENT_REACT || !event->name)
		return 0;

	struct cli_chat_page *page = ctx->chat;
	const char *name = event->name;
	const char *text =
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event->data, "text"));
	int rc = 0;
	if (!strcmp(name, "react.reasoning.delta")) {
		int begin = !page->stream_view || page->stream_kind != CLI_MESSAGE_REASONING;
		rc = cli_transcript_reasoning_append(ctx, text ? text : "", text ? strlen(text) : 0,
						     begin);
		if (!rc)
			rc = cli_chat_page_stream(ctx, CLI_MESSAGE_REASONING, text, 1);
	} else if (!strcmp(name, "cli.content.snapshot")) {
		/* Client recovery replaces the preview without closing its stream. */
		rc = cli_chat_page_stream(ctx, CLI_MESSAGE_ASSISTANT, text, 0);
	} else if (!strcmp(name, "react.content.delta") || !strcmp(name, "react.thought.delta") ||
		   !strcmp(name, "react.final.delta")) {
		cli_chat_activity(ctx, "");
		rc = cli_chat_page_stream(ctx, CLI_MESSAGE_ASSISTANT, text, 1);
	} else if (!strcmp(name, "react.content.end") || !strcmp(name, "react.thought.end")) {
		rc = cli_chat_files_markdown(ctx, text);
		if (text && *text)
			rc = cli_chat_page_stream(ctx, CLI_MESSAGE_ASSISTANT, text, 0);

		cli_chat_page_boundary(page);
	} else if (!strcmp(name, "react.final")) {
		rc = cli_chat_files_markdown(ctx, text);
		if (text && *text &&
		    !(page->last_assistant && page->last_assistant_text &&
		      !strcmp(page->last_assistant_text, text)))
			rc = cli_chat_page_stream(ctx, CLI_MESSAGE_ASSISTANT, text, 0);

		ctx->presenter->final_rendered = 1;
		cli_chat_page_boundary(page);
		cli_chat_activity(ctx, "");
	} else if (!strcmp(name, "react.thinking")) {
		cli_chat_page_boundary(page);
		cli_chat_activity(ctx, "");
	} else if (!strcmp(name, "react.context.usage")) {
		ctx->input_status.updated = 0;
	} else if (!strcmp(name, "react.user.steer")) {
		const char *display = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(event->data, "display_text"));
		rc = cli_chat_message(ctx, CLI_MESSAGE_USER, display ? display : text);
	} else if (!strcmp(name, "react.agents.wait.begin") ||
		   !strcmp(name, "react.agents.wait.end")) {
		page->agents_waiting = !strcmp(name, "react.agents.wait.begin");
		page->next_status_update = 0;
	} else if (!strcmp(name, "react.turn.begin") || !strcmp(name, "react.turn.end")) {
		/* The turn service owns lifecycle and the completion receipt. */
	} else
		return 0;

	return rc ? rc : 1;
}

static int cli_chat_page_submit(struct cli_chat_page *page, const char *text)
{
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	struct nt_application *app = &ctx->base;
	if (!text || !*text)
		return 0;

	if (nb_str_is_empty(nb_str_trim(nb_str_from_cstr(text))))
		return nt_text_field_set_text(page->editor, "");

	if (page->command_running)
		return 0;

	char *input NB_CLEANUP(nb_charp_cleanup) = strdup(text);
	if (!input)
		return NB_ERR_NOMEM;

	char **remembered = nb_vec_push(&page->history);
	if (!remembered)
		return NB_ERR_NOMEM;

	*remembered = strdup(text);
	if (!*remembered) {
		nb_vec_pop(&page->history);
		return NB_ERR_NOMEM;
	}

	page->history_index = page->history.len;
	free(page->draft);
	page->draft = NULL;
	int rc = nt_text_field_set_text(page->editor, "");
	if (rc)
		return rc;

	if (!ctx->turn_active)
		cli_chat_page_boundary(page);
	if (input[0] == '/') {
		cli_presentation_command_input(ctx, input);
		const struct cli_command *command = cli_command_find_input_borrow(input);
		int busy = ctx->input_job->active || cli_sessions_active(ctx);
		int navigation = !strcmp(input, "/new") || !strncmp(input, "/new ", 5) ||
			!strcmp(input, "/n") || !strncmp(input, "/n ", 3) ||
			!strcmp(input, "/sessions") || !strncmp(input, "/s ", 3) ||
			!strncmp(input, "/switch ", 8) || !strcmp(input, "/resume") ||
			!strncmp(input, "/resume ", 8) || !strncmp(input, "/r ", 3);
		if (busy && !navigation && strcmp(input, "/recap") && strcmp(input, "/jobs") &&
		    strcmp(input, "/side") && strncmp(input, "/side ", 6) &&
		    strcmp(input, "/sidebar") && strncmp(input, "/sidebar ", 9) &&
		    strncmp(input, "/jobs ", 6) && strcmp(input, "/agents") &&
		    strncmp(input, "/agents ", 8) && strcmp(input, "/subagents") &&
		    strncmp(input, "/subagents ", 11)) {
			if (!command)
				CMD_ERROR(ctx, "unknown command: %s. Try /help", input);
			else {
				CMD_ERROR(ctx,
					  "wait for the active turn before running this command");
				nt_text_field_set_text(page->editor, input);
			}
		} else if (ctx->input_job->active && !strcmp(input, "/recap"))
			(void)cli_recap_start(ctx, 0);
		else if (!ctx->input_job->active && command &&
		    !strcmp(command->name, "/compress")) {
			rc = cli_command_job_start(ctx->input_job, ctx, input);
			if (!rc)
				page->command_running = 1;
		} else
			(void)ctx->input_execute(ctx, input);

		(void)cli_event_queue_drain(ctx);
		if (page->panel || nt_application_transition_pending(app))
			ctx->application_command = 1;
		else if (!page->command_running)
			cli_presentation_command_end(ctx);
	} else {
		char *converted NB_CLEANUP(nb_charp_cleanup) = NULL;
		int converted_count =
			cli_composer_service_convert_paths_copy(&ctx->composer, input, &converted);
		if (converted_count < 0)
			return converted_count;

		if (converted_count > 0) {
			free(input);
			input = nb_charp_steal(&converted);
		}

		char *expanded NB_CLEANUP(nb_charp_cleanup) =
			cli_composer_model_expand_copy(&ctx->composer, input);
		if (!expanded)
			return NB_ERR_NOMEM;

		cli_turn_record_input_images(ctx, input);
		if (ctx->sessions && cli_chat_active(ctx)) {
			int handled = 0;
			if (!cli_sessions_active(ctx))
				rc = cli_media_handle_path(ctx, expanded, &handled);

			if (!handled && !rc)
				rc = cli_sessions_submit(ctx, expanded, input);
		} else if (ctx->input_job->active)
			rc = cli_command_job_prompt(ctx->input_job, expanded, input);
		else {
			int handled = 0;
			rc = cli_media_handle_path(ctx, expanded, &handled);
			if (!handled && !rc) {
				cli_presentation_user_input(ctx, input);
				cli_turn_begin(ctx);
				rc = cli_command_job_start(ctx->input_job, ctx, expanded);
				if (rc)
					cli_turn_finish(ctx, rc);
			}
		}
	}

	if (!ctx->turn_active)
		cli_chat_page_boundary(page);
	nt_scroll_view_follow(page->scroll, 1);
	if (!ctx->running)
		nt_application_stop(app, 0);

	return rc;
}

static int cli_chat_page_complete(struct cli_chat_page *page,
				  const struct nt_view_event *action)
{
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	const char *input = action->text;
	if (!input || input[0] != '/' || action->index != strlen(input) || strchr(input, ' '))
		return 0;

	char *candidate NB_CLEANUP(nb_charp_cleanup) = cli_command_completion_generator(input, 0);
	if (!candidate)
		return 0;

	nb_buf choices NB_CLEANUP(nb_buf_cleanup) = {0};
	char *common NB_CLEANUP(nb_charp_cleanup) = strdup(candidate);
	if (!common)
		return NB_ERR_NOMEM;

	int count = 0, rc = 0;
	while (candidate && !rc) {
		count++;
		if (choices.len)
			rc = (nb_buf_append_cstr(&choices, "  ") ? 0 : NB_ERR_NOMEM);

		if (!rc)
			rc = (nb_buf_append_cstr(&choices, candidate) ? 0 : NB_ERR_NOMEM);

		size_t length = 0;
		while (common[length] && common[length] == candidate[length])
			length++;

		common[length] = '\0';
		free(candidate);
		candidate = cli_command_completion_generator(input, count);
	}

	if (rc)
		return rc;

	if (count == 1 || strlen(common) > strlen(input)) {
		nb_buf suffix NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_append_cstr(&suffix, common + strlen(input)) ? 0 : NB_ERR_NOMEM);
		if (!rc && count == 1)
			rc = (nb_buf_append_char(&suffix, ' ') ? 0 : NB_ERR_NOMEM);

		if (!rc)
			rc = nt_text_field_insert_text(page->editor,
									  nb_buf_cstr(&suffix));
	} else {
		cli_chat_page_boundary(page);
		rc = cli_chat_output(ctx, nb_buf_cstr(&choices));
		cli_chat_page_boundary(page);
	}

	return rc;
}

static int cli_chat_page_action(void *data,
				const struct nt_view_event *action)
{
	struct cli_chat_page *page = data;
	if (action->type == NT_VIEW_EVENT_SUBMIT)
		return cli_chat_page_submit(page, action->text);

	if (action->type == NT_VIEW_EVENT_COMPLETE)
		return cli_chat_page_complete(page, action);

	if (action->type == NT_VIEW_EVENT_CHANGED && action->text &&
	    (strchr(action->text, '/') || strchr(action->text, '~')) &&
	    !(action->text[0] == '/' && cli_command_find_input_borrow(action->text))) {
		if (action->text[0] == '/') {
			const char *slash = strchr(action->text + 1, '/');
			if (!slash || slash >= action->text + strcspn(action->text, " \t\r\n"))
				return 0;
		}

		struct cli_application *ctx = cli_application_from_controller(&page->base);
		char *converted NB_CLEANUP(nb_charp_cleanup) = NULL;
		int rc = cli_composer_service_convert_paths_copy(&ctx->composer, action->text,
								 &converted);
		if (rc < 0)
			return rc;

		if (rc > 0)
			return nt_text_field_replace_range(page->editor,
				0, strlen(action->text), converted);
	}

	return 0;
}

static int cli_chat_page_key(struct nt_view_controller *controller,
			     const struct nt_event *event)
{
	struct cli_chat_page *page = (struct cli_chat_page *)controller;
	struct cli_application *ctx = cli_application_from_controller(controller);
	if (event->type != NT_EVENT_KEY || event->pasting || event->action == NT_KEY_ACTION_RELEASE)
		return 0;

	int key = event->key;
	unsigned mods = event->modifiers & ~(unsigned)(NT_MOD_CAPS_LOCK | NT_MOD_NUM_LOCK);
	if (page->panel)
		return key == 27 || key == NT_KEY_ESCAPE || key == 3 || key == 4
			       ? cli_chat_panel_close(ctx, NB_ERR_CANCELLED)
			       : 1;

	if (page->files_open && (key == 15 ||
	    ((key == 'o' || key == 'O') && mods == NT_MOD_CTRL))) {
		page->files_focus = !cli_file_browser_focused(page->files_view);
		(void)nt_view_set_hidden(page->primary, 0);
		int rc = page->files_focus ? cli_file_browser_focus(page->files_view)
			: nt_view_focus(page->editor);
		return rc ? rc : 1;
	}

	if (key == NT_KEY_FOCUS_IN || key == NT_KEY_FOCUS_OUT) {
		cli_recap_focus(ctx, key == NT_KEY_FOCUS_IN, cli_recap_now());
		return 1;
	}

	if (page->files_open && cli_file_browser_focused(page->files_view))
		return 0;

	if ((key == NT_KEY_PAGE_UP || key == NT_KEY_PAGE_DOWN) && mods == NT_MOD_CTRL) {
		if (page->command_running)
			return 1;

		int rc = cli_sessions_cycle(ctx, key == NT_KEY_PAGE_UP ? -1 : 1);
		return rc ? rc : 1;
	}

	if ((key == 27 || key == NT_KEY_ESCAPE) && !nt_view_is_hidden(page->bottom_button))
		return nt_button_activate(page->bottom_button);

	int paste_letter = key == 22 || key == 'v' || key == 'V';
	int image = paste_letter && mods == (NT_MOD_CTRL | NT_MOD_SUPER);
	if (image || key == 22 || nt_shortcut_resolve(event) == NT_SHORTCUT_PASTE ||
	    (paste_letter && (mods == NT_MOD_SUPER || mods == (NT_MOD_SHIFT | NT_MOD_CTRL))) ||
	    (key == NT_KEY_INSERT && mods == NT_MOD_SHIFT)) {
		int focus_rc = nt_view_focus(page->editor);
		if (focus_rc)
			return focus_rc;

		if (!image) {
			nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
			int rc = cli_clipboard_read_text(&text);
			if (!rc) {
				rc = nt_text_field_insert_text(
					page->editor, nb_buf_cstr(&text));
				if (!rc) {
					struct nt_view_event action = {
						.type = NT_VIEW_EVENT_CHANGED,
						.text = nt_text_field_text(
							page->editor)};
					rc = cli_chat_page_action(page, &action);
				}

				if (rc)
					CMD_ERROR(ctx, "Paste failed: %s", nb_err_str(
						morph_error_descriptions(), MORPH_ERROR_COUNT, rc));

				return 1;
			}

			if (rc == NB_ERR_TOO_BIG || rc == NB_ERR_FORMAT || rc == NB_ERR_TIMEOUT) {
				CMD_ERROR(ctx, "Paste failed: %s", nb_err_str(
					morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
				return 1;
			}
		}

		char *path NB_CLEANUP(nb_charp_cleanup) = NULL;
		const char *label = NULL;
		int rc = cli_clipboard_save_image(ctx, &path);
		if (!rc)
			rc = cli_composer_service_add_image(&ctx->composer, path, &label);

		if (!rc)
			rc = nt_text_field_insert_text(page->editor, label);

		if (rc) {
			if (path)
				(void)unlink(path);

			CMD_ERROR(ctx, "%s", image
				? "clipboard does not contain a supported image"
				: "clipboard does not contain supported text or an image");
		}

		return 1;
	}

	if (key == NT_KEY_PAGE_UP || key == NT_KEY_PAGE_DOWN || key == NT_KEY_WHEEL_UP ||
	    key == NT_KEY_WHEEL_DOWN) {
		int rc = nt_scroll_view_handle_key(page->scroll, key);
		return rc < 0 ? rc : 1;
	}

	if (key == NT_KEY_UP || key == NT_KEY_DOWN) {
		if (!page->history.len)
			return 1;

		if (page->history_index == page->history.len) {
			char *draft NB_CLEANUP(nb_charp_cleanup) =
				strdup(nt_text_field_text(page->editor));
			if (!draft)
				return NB_ERR_NOMEM;

			free(page->draft);
			page->draft = nb_charp_steal(&draft);
		}

		if (key == NT_KEY_UP && page->history_index)
			page->history_index--;

		if (key == NT_KEY_DOWN && page->history_index < page->history.len)
			page->history_index++;

		const char *value = page->draft ? page->draft : "";
		if (page->history_index < page->history.len)
			value = *(char **)nb_vec_at(&page->history, page->history_index);

		int rc = nt_text_field_set_text(page->editor, value);
		return rc ? rc : 1;
	}

	if (key == 20) {
		if (event->action == NT_KEY_ACTION_PRESS)
			cli_turn_details_toggle(ctx);

		return 1;
	}

	if (key == 3 || key == 27) {
		if (key == 27 && cli_sessions_active(ctx)) {
			int expedited = cli_sessions_expedite(ctx);
			if (expedited)
				return expedited < 0 ? expedited : 1;
		}

		if (cli_sessions_active(ctx))
			(void)cli_sessions_interrupt(ctx,
				key == 27 && cli_jobs_model_waiting(ctx->jobs));
		else if (ctx->input_job->active) {
			if (key == 27 && cli_command_job_expedite_prompts(ctx->input_job))
				return 1;

			if (key == 27 && cli_jobs_model_waiting(ctx->jobs))
				runtime_interrupt_background_wait(ctx->runtime);
			else
				runtime_cancel_turn(ctx->runtime);
		}

		if (key == 3) {
			int rc = nt_text_field_set_text(page->editor, "");
			if (rc)
				return rc;
		}

		return 1;
	}

	if (key == 4) {
		ctx->running = 0;
		nt_application_stop(&ctx->base, 0);
		return 1;
	}

	return 0;
}

static int cli_chat_update_queue(struct cli_application *ctx)
{
	struct cli_chat_page *page = ctx->chat;
	char *prompts[3] = {0};
	int64_t revision = page->queue_revision;
	size_t total = 0;
	int rc = ctx->sessions
		? cli_sessions_prompts_copy_since(ctx, prompts, 3, &total, &revision)
		: cli_command_job_prompts_copy_since(ctx->input_job, prompts, 3, &total, &revision);
	if (rc == NB_ERR_AGAIN)
		return 0;

	if (rc)
		return rc;

	size_t shown = total < 3 ? total : 3;
	nb_buf rows[4] = {0};
	const char *items[4] = {0};
	for (size_t i = 0; i < shown; i++) {
		if (!nb_buf_printf(&rows[i], "%s%s", i + 1 < total ? "├ " : "└ ", prompts[i]))
			rc = nb_buf_status(&rows[i]);

		items[i] = nb_buf_cstr(&rows[i]);
		free(prompts[i]);
	}

	if (total > shown) {
		if (!nb_buf_printf(&rows[shown], "└ +%zu more queued", total - shown))
			rc = nb_buf_status(&rows[shown]);

		items[shown] = nb_buf_cstr(&rows[shown]);
		shown++;
	}

	if (!rc)
		rc = nt_list_view_set_items(page->queue, items, shown);

	for (size_t i = 0; i < shown; i++)
		nb_buf_cleanup(&rows[i]);

	if (!rc)
		page->queue_revision = revision;

	return rc;
}

static int cli_chat_update_agents(struct cli_application *ctx, int columns, int styled, int resized)
{
	struct cli_chat_page *page = ctx->chat;
	const struct cli_agents_model *model = &ctx->agents;
	if (!resized && page->agents_revision == model->activity_revision)
		return 0;

	nb_buf summary NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf rows NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = cli_agents_summary(&summary, model->items, model->count, columns);
	if (!rc) {
		int count = cli_agents_rows(&rows, model->items, model->count, columns, 3, styled);
		if (count < 0)
			return count;
	}

	if (!rc && (rows.len || *nt_text_view_text(page->agents)))
		rc = cli_messages_set_rendered_view(page->agents, nb_buf_cstr(&rows), styled);

	if (!rc) {
		nb_buf_move(&page->agent_summary, &summary);
		page->agents_revision = model->activity_revision;
	}

	return rc;
}

static int cli_chat_update_jobs(struct cli_application *ctx, int columns, int styled, int resized,
				int64_t now)
{
	struct cli_chat_page *page = ctx->chat;
	const struct cli_jobs *model = ctx->jobs;
	uint64_t revision = model ? model->revision : 0;
	if (!resized && revision == page->jobs_revision && now < page->next_jobs_update)
		return 0;

	nb_buf rows NB_CLEANUP(nb_buf_cleanup) = {0};
	int count = cli_jobs_render(model, &rows, columns, styled, now);
	if (count < 0)
		return count;

	int rc = 0;
	if (rows.len || *nt_text_view_text(page->jobs))
		rc = cli_messages_set_rendered_view(page->jobs, nb_buf_cstr(&rows), styled);

	if (rc)
		return rc;

	page->jobs_revision = revision;
	page->next_jobs_update = INT64_MAX;
	if (rows.len && styled)
		page->next_jobs_update = now + cli_style_running_interval_ms(now);

	if (rows.len && cli_jobs_model_waiting(model)) {
		int64_t elapsed = cli_jobs_model_wait_elapsed(model, now);
		int64_t next_second = now + 1000 - elapsed % 1000;
		if (next_second < page->next_jobs_update)
			page->next_jobs_update = next_second;
	}

	return 0;
}

static int cli_chat_update_status(struct cli_application *ctx, int columns, int styled, int changed,
				  int64_t now)
{
	struct cli_chat_page *page = ctx->chat;
	if (changed)
		page->next_status_update = 0;

	if (cli_jobs_model_waiting(ctx->jobs)) {
		page->next_status_update = 0;
		return *nt_text_view_text(page->status) ? nt_text_view_set_text(page->status, "") : 0;
	}

	if (!ctx->turn_active || now < page->next_status_update)
		return 0;

	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *agents = ctx->agents.count || ctx->composer.ultra ?
		nb_buf_cstr(&page->agent_summary) : "";
	int64_t elapsed = now - page->turn_started;
	int64_t next = INT64_MAX;
	int rc;
	if (page->activity_text) {
		rc = nb_buf_append_cstr(&text, page->activity_text) ? 0 : nb_buf_status(&text);
	} else if (page->agents_waiting) {
		rc = nb_buf_printf(&text, "%s" CLI_FEED_RUNNING "%s Waiting for agents",
				   styled ? cli_style_running_color(now) : "",
				   styled ? ANSI_RESET : "") ? 0 : nb_buf_status(&text);
		if (!rc && *agents && !nb_buf_printf(&text, " · %s", agents))
			rc = nb_buf_status(&text);

		if (styled)
			next = now + cli_style_running_interval_ms(now);
	} else {
		rc = cli_work_status_running(&text, columns, styled, elapsed, now, agents);
		next = now + (styled ? cli_work_status_interval_ms(elapsed, now) :
			      1000 - elapsed % 1000);
	}

	if (!rc)
		rc = cli_messages_set_rendered_view(page->status, nb_buf_cstr(&text), styled);

	if (!rc)
		page->next_status_update = next;

	return rc;
}

static int cli_chat_update_activity(struct cli_chat_page *page, int columns, int64_t now)
{
	if (columns <= 0)
		return 0;

	struct cli_application *ctx = cli_application_from_controller(&page->base);
	int styled = cli_output_color_enabled(ctx);
	int resized = columns != page->activity_columns || styled != page->activity_styled;
	int status_changed = resized || page->agents_revision != ctx->agents.activity_revision ||
			     page->status_ultra != ctx->composer.ultra;
	int rc = cli_chat_update_agents(ctx, columns, styled, resized);
	if (!rc)
		rc = cli_chat_update_jobs(ctx, columns, styled, resized, now);

	if (!rc)
		rc = cli_chat_update_status(ctx, columns, styled, status_changed, now);

	if (!rc) {
		page->status_ultra = ctx->composer.ultra;
		page->activity_columns = columns;
		page->activity_styled = styled;
	}

	return rc;
}

static int cli_chat_page_footer(struct cli_chat_page *page, int columns)
{
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, 128) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_composer_footer_render(ctx, columns, &text);

	if (!rc && cli_sessions_open_count(ctx) > 1 && cli_sessions_current_id(ctx) > 0) {
		struct session session = {0};
		if (!runtime_session_current(ctx->runtime, &session)) {
			char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
				session.name, strlen(session.name),
				MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
			if (!safe)
				return NB_ERR_NOMEM;

			char name[128];
			morph_terminal_copy_ellipsized_display_width(name, sizeof(name),
				safe, 24, 0);
			if (!nb_buf_printf(&text, " · %s [%s]", name, session.display_id))
				rc = nb_buf_status(&text);
		}

		int other = cli_sessions_running_count(ctx) - cli_sessions_active(ctx);
		if (!rc && other > 0 && !nb_buf_printf(&text, " · %d other session%s running",
			other, other == 1 ? "" : "s"))
			rc = nb_buf_status(&text);
	}

	if (rc || (page->footer_text && !strcmp(page->footer_text, nb_buf_cstr(&text))))
		return rc;

	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&text));
	if (!copy)
		return NB_ERR_NOMEM;

	rc = cli_status_bar_set_text(page->footer, nb_buf_cstr(&text));
	if (!rc) {
		free(page->footer_text);
		page->footer_text = nb_charp_steal(&copy);
	}

	return rc;
}

static int cli_chat_footer_action(void *data, const struct nt_view_event *event)
{
	(void)event;
	struct cli_chat_page *page = data;
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	int rc = page->overlay ? cli_chat_overlay_close(ctx) : cli_mcp_status_page_show(ctx);
	if (rc >= 0)
		rc = nt_view_focus(page->editor);

	return rc < 0 ? rc : NT_VIEW_EVENT_STOP;
}

static int cli_chat_page_layout(struct nt_view *view)
{
	struct cli_chat_page *page = (struct cli_chat_page *)nt_view_controller(view);
	struct nt_view_rect content = nt_view_content_rect(view);
	if (page->columns != content.width) {
		int rc = cli_chat_page_footer(page, content.width);
		if (!rc)
			rc = cli_chat_update_activity(page, content.width, cli_chat_now());

		if (rc)
			return rc;

		page->columns = content.width;
	}

	struct nt_view_size editor = {0}, footer = {0}, composer;
	int modal = page->panel || page->prompt_rows;
	struct nt_view *input_view = modal ? page->panel_host : page->editor;
	int rc = 0;
	if (page->prompt_rows)
		editor.height = page->prompt_rows;
	else
		rc = nt_view_measure(input_view, content.width, &editor);

	if (!rc && !modal)
		rc = nt_view_measure(page->footer, content.width, &footer);

	if (rc)
		return rc;

	/* Input owns the scarce rows. Optional controls may use the space left
	 * after editable text and one component-painted bottom padding row. */
	int input = editor.height < content.height ? editor.height : content.height;
	int remaining = content.height - input - (!modal && input < content.height ? 1 : 0);
	int footer_rows = footer.height < remaining ? footer.height : remaining;
	remaining -= footer_rows;
	int auxiliary_rows = 0;
	for (size_t i = 0; i < nt_view_child_count(view); i++) {
		struct nt_view *child = nt_view_child(view, i);
		if (nt_view_is_hidden(child) || child == page->scroll ||
		    child == page->composer || child == page->panel_host ||
		    child == page->footer || child == page->bottom_button ||
		    child == page->overlay_host)
			continue;

		struct nt_view_size size;
		rc = nt_view_measure(child, content.width, &size);
		if (rc)
			return rc;

		int rows = size.height < remaining ? size.height : remaining;
		auxiliary_rows += rows;
		remaining -= rows;
	}

	int composer_space = content.height - footer_rows - auxiliary_rows;
	if (modal)
		composer = editor;
	else {
		size_t count = nt_view_child_count(page->feed);
		int separated = count && page->command_tail ==
			nt_view_child(page->feed, count - 1);

		rc = cli_composer_fit(page->composer, content.width, composer_space, separated);
		if (!rc)
			rc = nt_view_measure(page->composer, content.width, &composer);
	}

	if (rc)
		return rc;

	int composer_rows = composer.height < composer_space ? composer.height : composer_space;
	int controls = footer_rows + auxiliary_rows + composer_rows;
	/* The conversation fills the viewport above the fixed bottom controls. */
	int rows = content.height > controls ? content.height - controls : 0;
	/* A wheel event can clear follow even at the boundary. Preserve the old
	 * tail before laying out appended content or resizing the viewport. */
	const struct nt_scroll_state *scroll = nt_scroll_view_state(page->scroll);
	if (!scroll->follow && scroll->offset == nt_scroll_max(scroll)) {
		rc = nt_scroll_view_follow(page->scroll, 1);
		if (rc)
			return rc;
	}

	struct nt_view_rect button_row = {0};
	struct nt_view_rect feed_slot = {content.x, content.y, content.width, rows};
	rc = nt_view_arrange(page->scroll, feed_slot);
	if (rc)
		return rc;

	int offset = rows;
	for (size_t i = 0; i < nt_view_child_count(view); i++) {
		struct nt_view *child = nt_view_child(view, i);
		if (nt_view_is_hidden(child) || child == page->scroll ||
		    child == page->bottom_button || child == page->overlay_host)
			continue;

		int height = rows;
		if (child == page->composer || child == page->panel_host)
			height = composer_rows;
		else if (child == page->footer)
			height = footer_rows;
		else {
			struct nt_view_size size;
			rc = nt_view_measure(child, content.width, &size);
			if (rc)
				return rc;

			height = size.height < auxiliary_rows ? size.height : auxiliary_rows;
			auxiliary_rows -= height;
		}

		if (height > content.height - offset)
			height = content.height - offset;

		struct nt_view_rect slot = {content.x, content.y + offset, content.width, height};
		rc = nt_view_arrange(child, slot);
		if (rc)
			return rc;

		if (child == page->composer) {
			/* Share the first row of the input slot without moving the editor. */
			button_row = slot;
			button_row.height = nt_view_frame(page->editor).y > slot.y ? 1 : 0;
		}

		offset += height;
	}

	int overlay_height = rows ? rows : NT_CELLS(0);
	if (page->overlay_height != overlay_height) {
		rc = nt_view_set_size(page->overlay_host, NT_FILL, overlay_height);
		if (!rc)
			page->overlay_height = overlay_height;
	}
	if (!rc)
		rc = nt_view_arrange(page->overlay_host, feed_slot);
	if (!rc)
		rc = cli_chat_sync_bottom_button(page);
	if (rc || nt_view_is_hidden(page->bottom_button))
		return rc;

	return nt_view_place(page->bottom_button, button_row, NT_ANCHOR_CENTER, NT_VIEW_PLACE_FIT);
}

static int cli_chat_panel_cursor(struct nt_view *view, struct nt_canvas *canvas)
{
	struct cli_chat_page *page = (struct cli_chat_page *)nt_view_controller(view);
	if (page->panel && nt_view_is_focused(page->panel->focus)) {
		struct nt_view_rect panel = nt_view_content_rect(page->panel_host);
		struct nt_view_rect root = nt_view_content_rect(view);
		nt_canvas_cursor(canvas, panel.x - root.x, panel.y - root.y);
	}

	return 0;
}

static int cli_chat_editor_key(void *data, const struct nt_view_event *event)
{
	return cli_chat_page_key(data, event->input);
}

static int cli_chat_capture_input(struct nt_view *view, const struct nt_event *event)
{
	struct cli_chat_page *page = (struct cli_chat_page *)nt_view_controller(view);
	if (page->overlay && event->type == NT_EVENT_KEY &&
	    event->action != NT_KEY_ACTION_RELEASE && !event->pasting &&
	    (event->key == NT_KEY_ESCAPE || event->key == 27)) {
		int rc = cli_chat_overlay_close(cli_application_from_controller(&page->base));
		return rc < 0 ? rc : 1;
	}

	if (page->overlay && event->type == NT_EVENT_MOUSE &&
	    event->mouse.action == NT_MOUSE_PRESS && event->mouse.button == NT_MOUSE_LEFT) {
		struct nt_view_rect card = nt_view_frame(page->overlay->content);
		struct nt_view_rect footer = nt_view_frame(page->footer);
		int inside_card = event->mouse.x >= card.x && event->mouse.y >= card.y &&
			event->mouse.x < card.x + card.width && event->mouse.y < card.y + card.height;
		int inside_footer = event->mouse.x >= footer.x && event->mouse.y >= footer.y &&
			event->mouse.x < footer.x + footer.width &&
			event->mouse.y < footer.y + footer.height;
		if (!inside_card && !inside_footer) {
			int rc = cli_chat_overlay_close(
				cli_application_from_controller(&page->base));
			if (rc < 0)
				return rc;
		}
	}

	if (page->panel || page->prompt_rows || nt_view_is_focused(page->editor))
		return 0;

	struct nt_event input = *event;
	if (event->type != NT_EVENT_TEXT && event->type != NT_EVENT_PASTE) {
		unsigned shortcuts = NT_MOD_CTRL | NT_MOD_ALT | NT_MOD_SUPER | NT_MOD_HYPER |
				     NT_MOD_META;
		if (event->type != NT_EVENT_KEY || event->pasting ||
		    event->action == NT_KEY_ACTION_RELEASE || (event->modifiers & shortcuts))
			return 0;

		switch (event->key) {
		case ' ':
		case 8:
		case 0x7F:
		case NT_KEY_BACKSPACE:
		case NT_KEY_DELETE:
		case NT_KEY_LEFT:
		case NT_KEY_RIGHT:
		case NT_KEY_HOME:
		case NT_KEY_END:
			break;
		default:
			return 0;
		}

		/* Space must edit before a focused button can consume its KEY.
		 * Consuming that KEY suppresses the following TEXT event. */
		input.flags &= ~(unsigned)NT_EVENT_TEXT_FOLLOWS;
	}

	int rc = nt_view_focus(page->editor);
	if (rc)
		return rc;

	/* Preserve native selection, undo, paste sanitization and change actions. */
	rc = nt_view_class_methods(page->editor)->handle_event(page->editor, &input);
	return rc < 0 ? rc : 1;
}

static struct nt_text_style cli_chat_bottom_style(int hovered)
{
	return (struct nt_text_style){
		.foreground = {NT_COLOR_INDEXED, hovered ? 153u : 111u},
		.background = {NT_COLOR_INDEXED, hovered ? 24u : CLI_INPUT_BACKGROUND_INDEX},
	};
}

static int cli_chat_bottom_hover(void *data, const struct nt_view_event *event)
{
	(void)data;
	return nt_view_set_style(event->current,
				 cli_chat_bottom_style(event->type == NT_VIEW_EVENT_MOUSE_ENTER));
}

static void cli_chat_build_bottom_button(struct cli_chat_page *page, struct nt_builder *builder,
					struct nt_view *root, int styled)
{
	/* The native button owns measurement and activation. Its caption uses the
	 * composer's background and fills only the button's hit area. */
	page->bottom_button = nt_button_add(builder, root, CLI_CHAT_BOTTOM_LABEL, 0, NULL);
	struct nt_view_options options = {.width = NT_FILL, .height = 1};
	if (styled)
		options.style = cli_chat_bottom_style(0);

	struct nt_view *caption = nt_label_add(builder, page->bottom_button,
					       " " CLI_CHAT_BOTTOM_LABEL " ", &options);
	if (nt_builder_error(builder))
		return;

	nt_builder_fail(builder, nt_label_set_wrap_mode(caption, NT_TEXT_WRAP_NONE));
	nt_builder_fail(builder, nt_view_set_selectable(caption, 0));
	if (styled && (!nt_view_on(caption, NT_VIEW_EVENT_MOUSE_ENTER, cli_chat_bottom_hover, NULL) ||
		       !nt_view_on(caption, NT_VIEW_EVENT_MOUSE_LEAVE, cli_chat_bottom_hover, NULL)))
		nt_builder_fail(builder, NB_ERR_NOMEM);

	nt_builder_fail(builder, nt_view_set_hidden(page->bottom_button, 1));
	if (!nt_view_on(page->scroll, NT_VIEW_EVENT_SCROLLED, cli_chat_feed_scrolled, page) ||
	    !nt_view_on(page->bottom_button, NT_VIEW_EVENT_ACTIVATE, cli_chat_bottom_activate,
			page) ||
	    !nt_view_on_full(page->bottom_button, NT_VIEW_EVENT_MOUSE,
			     &(struct nt_view_listener_options){.flags = NT_VIEW_LISTEN_CAPTURE,
				NT_VIEW_LISTENER(cli_chat_bottom_pointer, page)}))
		nt_builder_fail(builder, NB_ERR_NOMEM);
}

static void cli_chat_page_build(struct nt_view_controller *controller, struct nt_builder *builder)
{
	struct cli_chat_page *page = (struct cli_chat_page *)controller;
	struct cli_application *ctx = cli_application_from_controller(controller);
	if (!ctx) {
		nt_builder_fail(builder, NB_ERR_STATE);
		return;
	}

	page->builder = builder;
	static const struct nt_view_class cli_chat_layout_class = {
		.superclass = &nt_view_base_class,
		.instance_size = sizeof(struct nt_view),
		.layout = cli_chat_page_layout,
		.capture_event = cli_chat_capture_input,
		.draw_overlay = cli_chat_panel_cursor};
	page->workspace = nt_split_view_add(builder, NULL, NT_SPLIT_VIEW_HORIZONTAL, 64, NULL);
	if (page->workspace &&
	    (!nt_view_on(page->workspace, NT_VIEW_EVENT_OPEN, cli_chat_files_link, page) ||
	     !nt_view_on_full(page->workspace, NT_VIEW_EVENT_MOUSE,
		&(struct nt_view_listener_options){.flags = NT_VIEW_LISTEN_CAPTURE,
			NT_VIEW_LISTENER(cli_chat_files_pointer, page)})))
		nt_builder_fail(builder, NB_ERR_NOMEM);

	struct nt_view *root = nt_view_add(builder, page->workspace, &cli_chat_layout_class, NULL);
	if (!root)
		return;

	page->primary = root;
	(void)nb_str_copy_to(nb_str_from_cstr(ctx->workdir), page->files_workdir,
		sizeof(page->files_workdir));
	page->files_view = cli_file_browser_add(builder, page->workspace, &page->files,
		page->files_workdir, cli_output_color_enabled(ctx));
	if (page->files_view) {
		nt_builder_fail(builder, nt_view_set_hidden(page->files_view, 1));
		if (!nt_view_on(page->files_view, NT_VIEW_EVENT_ACTION, cli_chat_files_action, page))
			nt_builder_fail(builder, NB_ERR_NOMEM);
	}

	page->scroll = nt_scroll_view_add(builder, root, NULL);
	nt_builder_fail(builder, nt_view_set_focusable(page->scroll, 0));
	nt_builder_fail(builder, nt_scroll_view_set_scrollbars(page->scroll, 0));
	nt_builder_fail(builder, nt_scroll_view_follow(page->scroll, 1));
	page->feed = nt_column_add(builder, page->scroll, 0,
				   &(struct nt_view_options){.width = NT_FILL});
	cli_banner_add(builder, page->feed);
	const struct nt_view_options jobs = {.max_height = 3};
	page->jobs = cli_report_text_add(builder, root, &jobs);

	page->agents = cli_report_text_add(builder, root, &jobs);

	struct nt_view *queue_title = nt_label_add(
		builder, root, CLI_FEED_PENDING " Messages queued · Esc to apply now", NULL);
	const struct nt_view_options queue = {.max_height = 4};
	page->queue = cli_list_add(builder, root, &queue);
	nt_view_set_focusable(page->queue, 0);
	page->status = cli_report_text_add(builder, root,
		&(struct nt_view_options){.height = 1, .margin = {.top = 1}});
	page->panel_host = nt_stack_view_add(builder, root, NT_STACK_VIEW_VERTICAL, 0, NULL);
	page->composer = cli_composer_add_borrow(builder, root, &ctx->composer,
						 cli_output_color_enabled(ctx), &page->editor);
	page->footer = cli_status_bar_add(builder, root, cli_output_color_enabled(ctx));
	if (!nt_view_on(page->footer, NT_VIEW_EVENT_ACTIVATE, cli_chat_footer_action, page))
		nt_builder_fail(builder, NB_ERR_NOMEM);

	cli_chat_build_bottom_button(page, builder, root, cli_output_color_enabled(ctx));
	page->overlay_host = nt_view_add(builder, root, &nt_view_base_class,
		&(struct nt_view_options){.position = NT_VIEW_POSITION_ABSOLUTE,
			.width = NT_FILL, .height = NT_FILL});
	nt_builder_fail(builder, nt_view_set_hidden(page->overlay_host, 1));

	if (nt_builder_error(builder))
		return;

	nt_builder_fail(builder, nt_view_use_state_bool(root, false, &page->modal));
	cli_panel_show_when(builder, page->panel_host, page->modal);
	cli_panel_hide_when(builder, page->composer, page->modal);
	cli_panel_hide_when(builder, page->footer, page->modal);
	struct nt_view *activity[] = {page->status, page->jobs, page->agents};
	for (size_t i = 0; i < sizeof(activity) / sizeof(activity[0]); i++)
		nt_builder_fail(builder,
				nt_view_bind_compute(activity[i], NT_PROP_HIDDEN,
						     cli_chat_activity_hidden, activity[i]));

	nt_builder_fail(builder, nt_view_bind_compute(page->queue, NT_PROP_HIDDEN,
						      cli_chat_queue_hidden, page));
	nt_builder_fail(builder, nt_view_bind_compute(queue_title, NT_PROP_HIDDEN,
						      cli_chat_queue_hidden, page));

	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_text_field_set_prompt_hanging_indent(page->editor, 0));
		nt_builder_fail(builder, nt_text_field_set_completion(page->editor, 1));
		nt_builder_fail(builder, nt_text_field_set_prompt(page->editor,
			cli_output_color_enabled(ctx) ? "› " : "> "));
		struct nt_text_style prompt = {0};
		if (cli_output_color_enabled(ctx))
			prompt.foreground = (struct nt_color){NT_COLOR_INDEXED,
				CLI_INPUT_MARKER_INDEX};

		nt_builder_fail(builder, nt_text_field_set_prompt_style(page->editor, prompt));
		if (!nt_view_on(page->editor, NT_VIEW_EVENT_ACTION, cli_chat_page_action, page) ||
		    !nt_view_on_full(page->editor, NT_VIEW_EVENT_KEY,
				     &(struct nt_view_listener_options){
					     .flags = NT_VIEW_LISTEN_CAPTURE,
					     NT_VIEW_LISTENER(cli_chat_editor_key, controller)})) {
			nt_builder_fail(builder, NB_ERR_NOMEM);
			return;
		}

		page->ready = 1;
		nt_view_focus(page->editor);
	}
}

static int cli_chat_page_refresh(struct cli_chat_page *page);

static int cli_chat_page_resume(struct nt_view_controller *controller)
{
	struct cli_chat_page *page = (struct cli_chat_page *)controller;
	struct cli_application *ctx = cli_application_from_controller(controller);
	int delivery_rc = cli_event_queue_set_events_enabled(ctx, 1);
	if (delivery_rc)
		return delivery_rc;

	if (ctx->session_history_pending) {
		ctx->session_history_pending = 0;
		int rc = cli_session_history_restore(ctx);
		if (rc)
			return rc;
	}

	int rc = cli_chat_page_refresh(page);
	return rc ? rc : nt_view_focus(page->panel ? page->panel->focus : page->editor);
}

static void cli_chat_page_appear(struct nt_view_controller *controller)
{
	int rc = cli_chat_page_resume(controller);
	if (rc)
		nt_application_stop(nt_view_controller_application(controller), rc);
}

static void cli_chat_page_prepare_deinit(struct nt_view_controller *controller)
{
	struct cli_chat_page *page = (struct cli_chat_page *)controller;
	struct cli_application *ctx = cli_application_from_controller(controller);
	page->ready = 0;
	if (ctx && ctx->chat == page)
		ctx->chat = NULL;
}

static void cli_chat_page_finish_job(struct cli_application *ctx)
{
	int result = cli_command_job_finish(ctx->input_job);
	(void)cli_event_queue_drain(ctx);
	if (ctx->chat->command_running) {
		ctx->chat->command_running = 0;
		cli_presentation_command_end(ctx);
		return;
	}

	cli_turn_finish(ctx, result);
	char *display NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *pending NB_CLEANUP(nb_charp_cleanup) =
		cli_command_job_take_prompt(ctx->input_job, &display);
	if (pending) {
		cli_presentation_user_input(ctx, display ? display : pending);
		cli_turn_begin(ctx);
		result = cli_command_job_start(ctx->input_job, ctx, pending);
		if (result)
			cli_turn_finish(ctx, result);
	}
}

static int cli_chat_page_refresh(struct cli_chat_page *page)
{
	struct cli_application *ctx = cli_application_from_controller(&page->base);
	if (!page->ready)
		return 0;

	int64_t now = cli_chat_now();
	cli_chat_files_session(page);
	int files_rc = cli_file_browser_sync(page->files_view);
	if (files_rc)
		return files_rc;

	struct nt_terminal_environment environment = {0};
	(void)nt_application_environment(&ctx->base, &environment);
	int showing = page->files_open && !page->panel && !page->prompt_rows;
	int narrow = environment.columns < 100;
	if (narrow) {
		int rc = nt_view_cancel_animation(page->workspace,
			CLI_CHAT_FILES_ANIMATION_CHANNEL, 1);
		if (rc)
			return rc;
	}

	if (cli_file_browser_focused(page->files_view))
		page->files_focus = 1;
	else if (nt_view_is_focused(page->editor))
		page->files_focus = 0;

	(void)nt_view_set_hidden(page->files_view, !showing || (narrow && !page->files_focus));
	(void)nt_view_set_hidden(page->primary, showing && narrow && page->files_focus);

	if (ctx->jobs) {
		int64_t session = 0;

		if (!runtime_session_current_id(ctx->runtime, &session))
			cli_jobs_model_set_session(ctx->jobs, session);
	}

	int rc = cli_chat_update_activity(page, page->columns, now);
	if (rc)
		return rc;

	if (now / 1000 != ctx->input_status.updated) {
		struct permission_policy policy;
		if (!cli_permissions_policy_get(ctx, &policy)) {
			ctx->input_status.permissions = policy;
			ctx->input_status.permissions_ready = 1;
		}

		(void)runtime_session_model_context_stats(ctx->runtime, NULL,
							  &ctx->input_status.tokens, NULL, NULL,
							  &ctx->input_status.limit);
		if (ctx->input_status.limit <= 0)
			ctx->input_status.limit =
				runtime_config_borrow(ctx->runtime)->models.text.context_limit;

		struct credit_summary credits = {0};
		if (!runtime_credit_summary_current_get(ctx->runtime, &credits))
			ctx->input_status.credits = credits.credits;

		ctx->input_status.updated = now / 1000;
		int ultra = 0;
		if (!cli_model_get_mode(ctx, &ultra)) {
			cli_composer_model_set_ultra(&ctx->composer, ultra,
				cli_output_color_enabled(ctx));
			nt_text_field_set_prompt(page->editor,
				ultra ? "» " : cli_output_color_enabled(ctx) ? "› " : "> ");
			struct nt_text_style prompt = {0};
			if (cli_output_color_enabled(ctx)) {
				prompt.foreground = (struct nt_color){
					NT_COLOR_INDEXED,
					ultra ? CLI_INPUT_ULTRA_INDEX : CLI_INPUT_MARKER_INDEX};
				prompt.attributes = ultra ? NT_STYLE_BOLD : 0;
			}

			nt_text_field_set_prompt_style(page->editor, prompt);
		}

		rc = cli_chat_page_footer(page, page->columns);
		if (rc)
			return rc;
	}

	int animation_rc = cli_composer_update(page->composer, now);
	if (animation_rc)
		return animation_rc;

	int timeout = ctx->composer.mode_transition ? 33 : 100;
	int64_t next = page->next_jobs_update;
	if (ctx->turn_active && page->next_status_update > now && page->next_status_update < next)
		next = page->next_status_update;

	if (next > now && next - now < timeout)
		timeout = (int)(next - now);

	nt_application_schedule(&ctx->base, timeout);
	rc = cli_chat_update_queue(ctx);

	return rc ? rc : cli_chat_page_error(page);
}

static int cli_chat_page_event(void *data, struct nt_application *app,
			       const struct nt_event *event)
{
	(void)data;
	struct cli_application *ctx = (struct cli_application *)app;
	if (!cli_chat_active(ctx))
		return 0;

	struct cli_chat_page *page = ctx->chat;
	cli_recap_tick(ctx, cli_recap_now());
	if (nb_atomic_int_load(&ctx->terminate_requested)) {
		ctx->running = 0;
		nt_application_stop(app, 0);
		return 1;
	}

	int modal = page->panel || nt_application_depth(app) > 1;
	if (cli_application_prompt_deferred(ctx) && cli_event_queue_owner_call_pending(ctx)) {
		if (page->panel)
			(void)cli_chat_panel_close(ctx, NB_ERR_CANCELLED);
		else if (!nt_application_transition_pending(app))
			(void)nt_view_controller_dismiss(
				nt_view_controller(nt_window_root_view(nt_application_window(app))),
				0);

		(void)nt_application_wake(app);
		return 0;
	}

	if (event->type == NT_EVENT_WAKE || event->type == NT_EVENT_TICK) {
		int rc = cli_event_queue_drain_batch(ctx);
		if (rc)
			return rc;
	}

	if (!modal && cli_command_job_done(ctx->input_job))
		cli_chat_page_finish_job(ctx);

	if (!modal && ctx->resume_requested && !nt_application_transition_pending(app)) {
		ctx->resume_requested = 0;
		(void)cli_resume_show(ctx);
	}

	if (nb_atomic_int_exchange(&ctx->interrupted, 0)) {
		if (page->panel)
			(void)cli_chat_panel_close(ctx, NB_ERR_CANCELLED);
		else if (modal)
			(void)nt_view_controller_dismiss(
				nt_view_controller(nt_window_root_view(nt_application_window(app))), 0);
		else if (cli_sessions_active(ctx))
			(void)cli_sessions_interrupt(ctx, 0);
		else if (ctx->input_job->active)
			runtime_cancel_turn(ctx->runtime);
	}

	if (nb_atomic_int_exchange(&ctx->resized, 0))
		nt_application_invalidate(app, NT_INVALIDATE_LAYOUT);

	if (!ctx->running)
		nt_application_stop(app, 0);

	if (page->panel && page->panel->refresh &&
	    (event->type == NT_EVENT_TICK || event->type == NT_EVENT_WAKE ||
	     event->type == NT_EVENT_RESIZE)) {
		int rc = page->panel->refresh(page->panel->data, event);
		if (rc < 0)
			return rc;
	}

	if (page->overlay && page->overlay->refresh &&
	    (event->type == NT_EVENT_TICK || event->type == NT_EVENT_WAKE ||
	     event->type == NT_EVENT_RESIZE)) {
		int rc = page->overlay->refresh(page->overlay->data, event);
		if (rc < 0)
			return rc;
	}

	return cli_chat_page_refresh(page);
}

static int cli_chat_page_init(struct nt_view_controller *controller)
{
	struct cli_chat_page *page = (struct cli_chat_page *)controller;
	char *entry = NULL;
	cli_files_init(&page->files);
	nb_vec_init(&page->history, sizeof(entry));
	page->tools = nb_map_new_with_cap(16);
	return page->tools ? 0 : NB_ERR_NOMEM;
}

static void cli_chat_page_deinit(struct nt_view_controller *controller)
{
	struct cli_chat_page *page = (struct cli_chat_page *)controller;
	cli_files_cleanup(&page->files);
	for (size_t i = 0; i < page->history.len; i++)
		free(*(char **)nb_vec_at(&page->history, i));

	nb_vec_cleanup(&page->history);
	nb_map_free(page->tools);
	nb_buf_cleanup(&page->stream);
	nb_buf_cleanup(&page->output);
	nb_buf_cleanup(&page->agent_summary);
	free(page->draft);
	free(page->last_assistant_text);
	free(page->footer_text);
	free(page->activity_text);
	free(page->command_input);
}

struct nt_application_options cli_chat_options(void)
{
	return (struct nt_application_options){
		.external_signals = 1,
		.terminal_flags = NT_TERMINAL_MOUSE | NT_TERMINAL_PASTE | NT_TERMINAL_FOCUS,
		.input = stdin,
		.output = stdout,
		.event = cli_chat_page_event,
	};
}

int cli_chat_present(struct cli_application *ctx)
{
	if (!ctx || !ctx->input_job || ctx->chat)
		return NB_ERR_INVAL;

	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct cli_chat_page),
		.init = cli_chat_page_init,
		.prepare_deinit = cli_chat_page_prepare_deinit,
		.deinit = cli_chat_page_deinit,
		.load_view = cli_chat_page_build,
		.handle_event = cli_chat_page_key,
		.view_will_appear = cli_chat_page_appear,
	};
	struct nt_view_controller *controller NB_CLEANUP(cli_application_controller_cleanup) = NULL;
	int rc = nt_view_controller_create(&type, &controller);
	if (rc)
		return rc;

	struct cli_chat_page *page = (struct cli_chat_page *)controller;
	rc = nt_view_controller_set_tick_interval(controller, 100);
	if (!rc)
		rc = nt_application_present_view_controller(&ctx->base, &controller);

	if (!rc) {
		ctx->chat = page;
		cli_feed_set_hidden(ctx, 1);
	}

	return rc;
}
