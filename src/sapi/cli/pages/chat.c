#include "sapi/cli/pages/chat.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/commands/media.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/models/jobs.h"
#include "config/config.h"
#include "credits.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/pages/resume.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/composer_footer.h"
#include "sapi/cli/presentation/work_status.h"
#include "sapi/cli/presentation/turn_details.h"
#include "sapi/cli/widgets/banner.h"
#include "sapi/cli/widgets/composer.h"
#include "sapi/cli/widgets/composer_panel.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/jobs.h"
#include "sapi/cli/widgets/notice.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/command_job.h"
#include "nb/map.h"
#include "nb/time.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/services/turn.h"
#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/services/transcript.h"
#include "sapi/cli/services/composer.h"
#include "sapi/cli/adapters/clipboard/clipboard.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/widgets/message.h"
#include "sapi/cli/widgets/rich_document.h"
#include "sapi/cli/widgets/media.h"
#include "sapi/cli/widgets/mcp_tree.h"
#include "sapi/cli/widgets/tool_call.h"
#include "sapi/cli/widgets/feed_block.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/plan.h"
#include "sapi/cli/application_internal.h"
#include "nb/cleanup.h"
#include <morph/tui/view_controller.h>
#include <morph/tui/terminal.h>
#include <morph/tui/widgets/stack.h>
#include <morph/tui/widgets/scroll.h>
#include <morph/tui/widgets/label.h>
#include <morph/tui/widgets/list.h>
#include <morph/tui/widgets/text_view.h>
#include <morph/tui/widgets/text_field.h>
#include <time.h>
#include <unistd.h>

struct cli_chat_page {
	struct cli_application *ctx;
	struct cli_command_job *job;
	struct morph_tui_view_controller *controller;
	struct morph_tui_builder *builder;
	struct morph_tui_view *scroll, *feed, *composer, *editor, *status, *footer;
	struct morph_tui_view *panel_host, *panel;
	struct morph_tui_view *stream_view, *output_view, *queue_title, *queue, *jobs, *agents;
	struct morph_tui_view *last_assistant, *command_group, *command_tail;
	char *last_assistant_text;
	char *footer_text;
	char *activity_text;
	nb_buf stream, output;
	nb_map *tools;
	nb_vec history;
	char *draft;
	size_t history_index;
	int stream_kind, ready, columns, feed_rows, agents_waiting, prompt_rows;
	int64_t turn_started;
};

static int64_t cli_chat_now(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? now / 1000000 : 0;
}

int cli_chat_active(const struct cli_application *ctx)
{
	return ctx && ctx->chat && ctx->chat->ready;
}

int cli_chat_flush(struct cli_application *ctx)
{
	return cli_chat_active(ctx) ? morph_tui_application_render(&ctx->base) : 0;
}

int cli_chat_panel_active(const struct cli_application *ctx)
{
	return cli_chat_active(ctx) && ctx->chat->panel;
}

static int cli_chat_panel_hide_activity(struct cli_chat_page *page)
{
	struct morph_tui_view *views[] = {page->jobs, page->status, page->agents, page->queue_title,
					  page->queue};
	for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
		int rc = morph_tui_view_set_hidden(views[i], 1);
		if (rc)
			return rc;
	}
	return 0;
}

int cli_chat_prompt_begin(struct cli_application *ctx, int rows)
{
	if (!cli_chat_active(ctx))
		return 0;
	struct cli_chat_page *page = ctx->chat;
	if (rows < 1 || page->panel || page->prompt_rows)
		return NB_ERR_INVAL;
	page->prompt_rows = rows;
	int rc = morph_tui_view_set_hidden(page->composer, 1);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->footer, 1);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->panel_host, 0);
	if (!rc)
		rc = cli_chat_panel_hide_activity(page);
	if (!rc)
		rc = cli_chat_flush(ctx);
	if (rc) {
		cli_chat_prompt_end(ctx);
		return rc;
	}
	struct morph_tui_view_rect frame = morph_tui_view_frame(page->panel_host);
	/* The synchronous widget owns this reserved region until prompt_end. */
	rc = morph_tui_terminal_clear_below(stdout, frame.y + 2, frame.x + 1);
	if (rc) {
		cli_chat_prompt_end(ctx);
		return rc;
	}
	return 0;
}

void cli_chat_prompt_end(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx) || !ctx->chat->prompt_rows)
		return;
	struct cli_chat_page *page = ctx->chat;
	page->prompt_rows = 0;
	(void)morph_tui_view_set_hidden(page->panel_host, 1);
	(void)morph_tui_view_set_hidden(page->composer, 0);
	(void)morph_tui_view_set_hidden(page->footer, 0);
	(void)morph_tui_view_focus(page->editor);
	(void)morph_tui_application_restore_terminal(&ctx->base);
	(void)cli_chat_flush(ctx);
}

int cli_chat_panel_show(struct cli_application *ctx,
	const struct cli_composer_panel_options *options, const struct cli_composer_panel_ops *ops,
	void *data)
{
	if (!cli_chat_active(ctx))
		return NB_ERR_INVAL;
	if (!ctx->running)
		return NB_ERR_CANCELLED;
	struct cli_chat_page *page = ctx->chat;
	if (page->panel)
		return NB_ERR_BUSY;
	page->panel = cli_composer_panel_add_borrow(page->builder, page->panel_host, ctx,
		options, ops, data);
	if (!page->panel) {
		(void)morph_tui_view_clear_children(page->panel_host);
		return morph_tui_builder_error(page->builder);
	}
	morph_tui_builder_fail(page->builder, morph_tui_view_set_hidden(page->composer, 1));
	morph_tui_builder_fail(page->builder, morph_tui_view_set_hidden(page->footer, 1));
	morph_tui_builder_fail(page->builder, morph_tui_view_set_hidden(page->panel_host, 0));
	morph_tui_builder_fail(page->builder, morph_tui_view_focus(page->panel));
	return cli_chat_panel_hide_activity(page);
}

int cli_chat_panel_remove(struct cli_application *ctx, struct morph_tui_view *panel)
{
	if (!cli_chat_panel_active(ctx) || ctx->chat->panel != panel)
		return NB_ERR_INVAL;
	struct cli_chat_page *page = ctx->chat;
	page->panel = NULL;
	int rc = morph_tui_view_set_hidden(page->panel_host, 1);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->composer, 0);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->footer, 0);
	if (!rc)
		rc = morph_tui_view_focus(page->editor);
	if (!rc)
		rc = morph_tui_view_clear_children(page->panel_host);
	return rc;
}

int cli_chat_panel_cancel(struct cli_application *ctx, int result)
{
	return cli_chat_panel_active(ctx) ? cli_composer_panel_finish(ctx->chat->panel, result) : 0;
}

int cli_chat_reset(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return 0;
	struct cli_chat_page *page = ctx->chat;
	page->stream_view = NULL;
	page->output_view = NULL;
	page->stream_kind = 0;
	page->last_assistant = NULL;
	page->command_group = NULL;
	page->command_tail = NULL;
	free(page->last_assistant_text);
	page->last_assistant_text = NULL;
	nb_buf_clear(&page->stream);
	nb_buf_clear(&page->output);
	nb_map_clear(page->tools);
	page->feed_rows = 0;
	int rc = morph_tui_view_clear_children(page->feed);
	if (!rc)
		rc = morph_tui_widget_scroll_follow(page->scroll, 1);
	return rc;
}

static int cli_chat_page_error(struct cli_chat_page *page)
{
	return morph_tui_builder_error(page->builder);
}

static void cli_chat_page_boundary(struct cli_chat_page *page)
{
	page->stream_view = NULL;
	page->output_view = NULL;
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
	cli_chat_page_boundary(page);
	page->last_assistant = NULL;
	struct morph_tui_view *view = cli_message_add_borrow(
		page->builder, page->feed, (enum cli_message_kind)kind,
			       text ? text : "", cli_output_color_enabled(ctx));
	if (view && kind == CLI_MESSAGE_USER)
		morph_tui_builder_fail(page->builder,
			cli_message_set_ultra(view, ctx->composer.ultra));
	return cli_chat_page_error(page);
}

int cli_chat_media(struct cli_application *ctx, const char *type, const char *path)
{
	if (!cli_chat_active(ctx) || !type || !path)
		return NB_ERR_INVAL;
	struct cli_chat_page *page = ctx->chat;
	cli_chat_page_boundary(page);
	const struct morph_tui_view_options options = {
		.margin = {.top = page->command_group ? 0 : 1}};
	cli_media_add_borrow(page->builder,
		page->command_group ? page->command_group : page->feed, ctx, type, path, &options);
	return cli_chat_page_error(page);
}

static struct morph_tui_view *cli_chat_page_report_parent(struct cli_chat_page *page)
{
	cli_chat_page_boundary(page);
	if (page->command_group)
		return page->command_group;
	return morph_tui_widget_stack_add_borrow(page->builder, page->feed,
		MORPH_TUI_WIDGET_STACK_VERTICAL, 0,
		&(struct morph_tui_view_options){.margin = {.top = 1}});
}

int cli_chat_mcp_servers(struct cli_application *ctx, const struct runtime_mcp_status *servers,
	size_t count)
{
	if (!cli_chat_active(ctx))
		return NB_ERR_INVAL;
	struct cli_chat_page *page = ctx->chat;
	struct morph_tui_view *parent = cli_chat_page_report_parent(page);
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
	struct morph_tui_view *parent = cli_chat_page_report_parent(page);
	cli_mcp_tree_items_add_borrow(page->builder, parent, kind, server, items, count,
		cli_output_color_enabled(ctx));
	return cli_chat_page_error(page);
}

int cli_chat_command_input(struct cli_application *ctx, const char *text)
{
	if (!cli_chat_active(ctx) || !text)
		return NB_ERR_INVAL;
	struct cli_chat_page *page = ctx->chat;
	cli_chat_page_boundary(page);
	page->command_group = morph_tui_widget_stack_add_borrow(
		page->builder, page->feed, MORPH_TUI_WIDGET_STACK_VERTICAL, 1,
		&(struct morph_tui_view_options){.margin = {.bottom = 1}});
	page->command_tail = page->command_group;
	struct morph_tui_view_options style = {0};
	if (cli_output_color_enabled(ctx))
		style.style.foreground = (struct morph_tui_color){MORPH_TUI_COLOR_INDEXED, 141};
	morph_tui_widget_label_add_borrow(page->builder, page->command_group, text, &style);
	return cli_chat_page_error(page);
}

void cli_chat_command_end(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return;
	ctx->chat->command_group = NULL;
	cli_chat_page_boundary(ctx->chat);
}

int cli_chat_output(struct cli_application *ctx, const char *text)
{
	if (!cli_chat_active(ctx) || !text || !*text)
		return 0;
	struct cli_chat_page *page = ctx->chat;
	if (!page->output_view) {
		const struct morph_tui_view_options options = {
			.margin = {.top = page->command_group ? 0 : 1}};
		struct morph_tui_view *container = morph_tui_widget_stack_add_borrow(
			page->builder, page->command_group ? page->command_group : page->feed,
			MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &options);
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
		char *copy = text && *text ? strdup(text) : NULL;
		if (text && *text && !copy) {
			morph_tui_builder_fail(page->builder, NB_ERR_NOMEM);
			return;
		}
		free(page->activity_text);
		page->activity_text = copy;
		(void)cli_messages_set_rendered_view(page->status, text ? text : "",
						     cli_output_color_enabled(ctx));
		(void)morph_tui_view_set_hidden(page->status, !text || !*text);
	}
}

void cli_chat_turn_begin(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return;
	cli_chat_page_boundary(ctx->chat);
	ctx->chat->agents_waiting = 0;
	ctx->chat->turn_started = cli_chat_now();
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
	morph_tui_builder_fail(page->builder, rc);
	const struct morph_tui_view_options options = {.margin = {.top = 1, .bottom = 1},
						       .padding = {.left = 2}};
	if (!rc) {
		struct morph_tui_view *row = morph_tui_widget_text_view_add_borrow(
			page->builder, page->feed, "", &options);
		if (row) {
			morph_tui_widget_text_view_set_content_sizing(row, 1);
			morph_tui_view_set_focusable(row, 0);
			morph_tui_builder_fail(
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
	struct morph_tui_view *view = nb_map_get_cstr(page->tools, tool->id);
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

static int cli_chat_page_stream(struct cli_chat_page *page, int kind, const char *text, int delta)
{
	if (!page->stream_view || page->stream_kind != kind) {
		cli_chat_page_boundary(page);
		page->stream_kind = kind;
		page->stream_view = cli_message_add_borrow(page->builder, page->feed,
							   (enum cli_message_kind)kind, "",
							   cli_output_color_enabled(page->ctx));
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
			rc = cli_chat_page_stream(page, CLI_MESSAGE_REASONING, text, 1);
	} else if (!strcmp(name, "react.content.delta") || !strcmp(name, "react.thought.delta") ||
		   !strcmp(name, "react.final.delta")) {
		cli_chat_activity(ctx, "");
		rc = cli_chat_page_stream(page, CLI_MESSAGE_ASSISTANT, text, 1);
	} else if (!strcmp(name, "react.content.end") || !strcmp(name, "react.thought.end")) {
		if (text && *text)
			rc = cli_chat_page_stream(page, CLI_MESSAGE_ASSISTANT, text, 0);
		cli_chat_page_boundary(page);
	} else if (!strcmp(name, "react.final")) {
		if (text && *text &&
		    !(page->last_assistant && page->last_assistant_text &&
		      !strcmp(page->last_assistant_text, text)))
			rc = cli_chat_page_stream(page, CLI_MESSAGE_ASSISTANT, text, 0);
		ctx->presenter->final_rendered = 1;
		cli_chat_page_boundary(page);
		cli_chat_activity(ctx, "");
	} else if (!strcmp(name, "react.thinking")) {
		cli_chat_page_boundary(page);
		cli_chat_activity(ctx, "");
	} else if (!strcmp(name, "react.user.steer")) {
		const char *display = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(event->data, "display_text"));
		rc = cli_chat_message(ctx, CLI_MESSAGE_USER, display ? display : text);
	} else if (!strcmp(name, "react.agents.wait.begin") ||
		   !strcmp(name, "react.agents.wait.end")) {
		page->agents_waiting = !strcmp(name, "react.agents.wait.begin");
	} else if (!strcmp(name, "react.turn.begin") || !strcmp(name, "react.turn.end")) {
		/* The turn service owns lifecycle and the completion receipt. */
	} else
		return 0;
	return rc ? rc : 1;
}

static int cli_chat_page_submit(struct cli_chat_page *page, const char *text)
{
	struct cli_application *ctx = page->ctx;
	struct morph_tui_application *app = &ctx->base;
	if (!text || !*text)
		return 0;
	if (nb_str_is_empty(nb_str_trim(nb_str_from_cstr(text))))
		return morph_tui_widget_text_field_set_text_copy(page->editor, "");
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
	int rc = morph_tui_widget_text_field_set_text_copy(page->editor, "");
	if (rc)
		return rc;
	cli_chat_page_boundary(page);
	if (input[0] == '/') {
		cli_presentation_command_input(ctx, input);
		if (page->job->active && strcmp(input, "/recap") && strcmp(input, "/jobs") &&
		    strncmp(input, "/jobs ", 6) && strcmp(input, "/agents") &&
		    strncmp(input, "/agents ", 8) && strcmp(input, "/subagents") &&
		    strncmp(input, "/subagents ", 11)) {
			if (!cli_command_find_input_borrow(input))
				CMD_ERROR(ctx, "unknown command: %s. Try /help", input);
			else {
				CMD_ERROR(ctx,
					  "wait for the active turn before running this command");
				morph_tui_widget_text_field_set_text_copy(page->editor, input);
			}
		} else if (page->job->active && !strcmp(input, "/recap"))
			(void)cli_recap_start(ctx, 0);
		else
			(void)ctx->input_execute(ctx, input);
		(void)cli_event_queue_drain(ctx);
		if (page->panel || morph_tui_application_transition_pending(app))
			ctx->application_command = 1;
		else
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
		if (page->job->active)
			rc = cli_command_job_prompt(page->job, expanded, input);
		else {
			int handled = 0;
			rc = cli_media_handle_path(ctx, expanded, &handled);
			if (!handled && !rc) {
				cli_presentation_user_input(ctx, input);
				cli_turn_begin(ctx);
				rc = cli_command_job_start(page->job, ctx, expanded);
				if (rc)
					cli_turn_finish(ctx, rc);
			}
		}
	}
	cli_chat_page_boundary(page);
	morph_tui_widget_scroll_follow(page->scroll, 1);
	if (!ctx->running)
		morph_tui_application_stop(app, 0);
	return rc;
}

static int cli_chat_page_complete(struct cli_chat_page *page,
				  const struct morph_tui_widget_action *action)
{
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
			rc = morph_tui_widget_text_field_insert_text_copy(page->editor,
									  nb_buf_cstr(&suffix));
	} else {
		cli_chat_page_boundary(page);
		rc = cli_chat_output(page->ctx, nb_buf_cstr(&choices));
		cli_chat_page_boundary(page);
	}
	return rc;
}

static int cli_chat_page_action(struct morph_tui_view_controller *controller,
				const struct morph_tui_widget_action *action)
{
	struct cli_chat_page *page = controller->data;
	if (action->type == MORPH_TUI_WIDGET_ACTION_SUBMIT)
		return cli_chat_page_submit(page, action->text);
	if (action->type == MORPH_TUI_WIDGET_ACTION_COMPLETE)
		return cli_chat_page_complete(page, action);
	if (action->type == MORPH_TUI_WIDGET_ACTION_CHANGED && action->text &&
	    (strchr(action->text, '/') || strchr(action->text, '~')) &&
	    !(action->text[0] == '/' && cli_command_find_input_borrow(action->text))) {
		if (action->text[0] == '/') {
			const char *slash = strchr(action->text + 1, '/');
			if (!slash || slash >= action->text + strcspn(action->text, " \t\r\n"))
				return 0;
		}
		char *converted NB_CLEANUP(nb_charp_cleanup) = NULL;
		int rc = cli_composer_service_convert_paths_copy(&page->ctx->composer,
			action->text, &converted);
		if (rc < 0)
			return rc;
		if (rc > 0)
			return morph_tui_widget_text_field_replace_range_copy(page->editor,
				0, strlen(action->text), converted);
	}
	return 0;
}

static int cli_chat_page_key(struct morph_tui_view_controller *controller,
			     const struct morph_tui_event *event)
{
	struct cli_chat_page *page = controller->data;
	struct cli_application *ctx = page->ctx;
	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return 0;
	int key = event->key;
	if (key == MORPH_TUI_KEY_FOCUS_IN || key == MORPH_TUI_KEY_FOCUS_OUT) {
		cli_recap_focus(ctx, key == MORPH_TUI_KEY_FOCUS_IN, cli_recap_now());
		return 1;
	}
	if (key == 22 || key == MORPH_TUI_KEY_PASTE_CLIPBOARD) {
		char *path NB_CLEANUP(nb_charp_cleanup) = NULL;
		const char *label = NULL;
		int rc = cli_clipboard_save_image(ctx, &path);
		if (!rc)
			rc = cli_composer_service_add_image(&ctx->composer, path, &label);
		if (!rc)
			rc = morph_tui_widget_text_field_insert_text_copy(page->editor, label);
		if (rc) {
			if (path)
				(void)unlink(path);
			CMD_ERROR(ctx, "clipboard does not contain a supported image");
		}
		return 1;
	}
	if (key == MORPH_TUI_KEY_PAGE_UP || key == MORPH_TUI_KEY_PAGE_DOWN ||
	    key == MORPH_TUI_KEY_WHEEL_UP || key == MORPH_TUI_KEY_WHEEL_DOWN)
		return morph_tui_widget_scroll_handle_key(
			page->scroll, key == MORPH_TUI_KEY_WHEEL_UP	? MORPH_TUI_KEY_UP
				      : key == MORPH_TUI_KEY_WHEEL_DOWN ? MORPH_TUI_KEY_DOWN
									: key);
	if (key == MORPH_TUI_KEY_UP || key == MORPH_TUI_KEY_DOWN) {
		if (!page->history.len)
			return 1;
		if (page->history_index == page->history.len) {
			char *draft NB_CLEANUP(nb_charp_cleanup) =
				strdup(morph_tui_widget_text_field_text_borrow(page->editor));
			if (!draft)
				return NB_ERR_NOMEM;
			free(page->draft);
			page->draft = nb_charp_steal(&draft);
		}
		if (key == MORPH_TUI_KEY_UP && page->history_index)
			page->history_index--;
		if (key == MORPH_TUI_KEY_DOWN && page->history_index < page->history.len)
			page->history_index++;
		const char *value = page->draft ? page->draft : "";
		if (page->history_index < page->history.len)
			value = *(char **)nb_vec_at(&page->history, page->history_index);
		int rc = morph_tui_widget_text_field_set_text_copy(page->editor, value);
		return rc ? rc : 1;
	}
	if (key == 20) {
		cli_turn_details_toggle(ctx);
		return 1;
	}
	if (key == 3 || key == 27) {
		if (page->job->active) {
			if (key == 27 && cli_command_job_expedite_prompts(page->job))
				return 1;
			if (key == 27 && cli_jobs_model_waiting(ctx->jobs))
				runtime_interrupt_background_wait(ctx->runtime);
			else
				runtime_cancel_turn(ctx->runtime);
		}
		if (key == 3) {
			int rc = morph_tui_widget_text_field_set_text_copy(page->editor, "");
			if (rc)
				return rc;
		}
		return 1;
	}
	if (key == 4) {
		ctx->running = 0;
		morph_tui_application_stop(&ctx->base, 0);
		return 1;
	}
	return 0;
}

struct cli_chat_layout {
	struct morph_tui_widget base;
	struct cli_chat_page *page;
};

static int cli_chat_page_footer(struct cli_chat_page *page, int columns)
{
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, 128) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_composer_footer_render(page->ctx, columns, &text);
	if (rc || (page->footer_text && !strcmp(page->footer_text, nb_buf_cstr(&text))))
		return rc;
	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&text));
	if (!copy)
		return NB_ERR_NOMEM;
	rc = cli_messages_set_rendered_view(page->footer, nb_buf_cstr(&text),
					    cli_output_color_enabled(page->ctx));
	if (!rc) {
		free(page->footer_text);
		page->footer_text = nb_charp_steal(&copy);
	}
	return rc;
}

static int cli_chat_page_layout(struct morph_tui_view *view)
{
	struct cli_chat_page *page = ((struct cli_chat_layout *)view)->page;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);
	if (page->columns != content.width) {
		int rc = cli_chat_page_footer(page, content.width);
		if (rc)
			return rc;
		page->columns = content.width;
	}
	struct morph_tui_view_size editor = {0}, footer = {0}, composer;
	int modal = page->panel || page->prompt_rows;
	struct morph_tui_view *input_view = modal ? page->panel_host : page->editor;
	int rc = 0;
	if (page->prompt_rows)
		editor.height = page->prompt_rows;
	else
		rc = morph_tui_view_measure(input_view, content.width, &editor);
	if (!rc && !modal)
		rc = morph_tui_view_measure(page->footer, content.width, &footer);
	if (rc)
		return rc;
	/* Input owns the scarce rows. Optional controls may use the space left
	 * after editable text and one component-painted bottom padding row. */
	int input = editor.height < content.height ? editor.height : content.height;
	int remaining = content.height - input - (!modal && input < content.height ? 1 : 0);
	int footer_rows = footer.height < remaining ? footer.height : remaining;
	remaining -= footer_rows;
	int auxiliary_rows = 0;
	for (size_t i = 1; i < morph_tui_view_child_count(view); i++) {
		struct morph_tui_view *child = morph_tui_view_child_borrow(view, i);
		if (morph_tui_view_hidden(child) || child == page->composer || child == page->panel_host ||
		    child == page->footer)
			continue;
		struct morph_tui_view_size size;
		rc = morph_tui_view_measure(child, content.width, &size);
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
		size_t count = morph_tui_view_child_count(page->feed);
		int separated = count && page->command_tail ==
			morph_tui_view_child_borrow(page->feed, count - 1);

		rc = cli_composer_fit(page->composer, content.width, composer_space, separated);
		if (!rc)
			rc = morph_tui_view_measure(page->composer, content.width, &composer);
	}
	if (rc)
		return rc;
	int composer_rows = composer.height < composer_space ? composer.height : composer_space;
	int controls = footer_rows + auxiliary_rows + composer_rows;
	struct morph_tui_view_size feed;
	rc = morph_tui_view_measure(page->feed, content.width, &feed);
	if (rc)
		return rc;
	/* Follow the original document flow until the composer reaches the
	 * bottom. Retain allocated feed space when a live preview shrinks. */
	int available = content.height > controls ? content.height - controls : 0;
	if (modal || feed.height > page->feed_rows)
		page->feed_rows = feed.height;
	int rows = page->feed_rows < available ? page->feed_rows : available;
	int offset = 0;
	for (size_t i = 0; i < morph_tui_view_child_count(view); i++) {
		struct morph_tui_view *child = morph_tui_view_child_borrow(view, i);
		if (morph_tui_view_hidden(child))
			continue;
		int height = rows;
		if (child == page->composer || child == page->panel_host)
			height = composer_rows;
		else if (child == page->footer)
			height = footer_rows;
		else if (i) {
			struct morph_tui_view_size size;
			rc = morph_tui_view_measure(child, content.width, &size);
			if (rc)
				return rc;
			height = size.height < auxiliary_rows ? size.height : auxiliary_rows;
			auxiliary_rows -= height;
		}
		if (height > content.height - offset)
			height = content.height - offset;
		rc = morph_tui_view_arrange(child, (struct morph_tui_view_rect){
			content.x, content.y + offset, content.width, height});
		if (rc)
			return rc;
		offset += height;
	}
	return 0;
}

static void cli_chat_page_build(struct morph_tui_view_controller *controller,
				struct morph_tui_builder *builder)
{
	struct cli_chat_page *page = controller->data;
	page->controller = controller;
	page->builder = builder;
	static const struct morph_tui_view_ops cli_chat_layout_ops = {
		.layout = cli_chat_page_layout};
	struct cli_chat_layout *layout = (struct cli_chat_layout *)morph_tui_widget_add_borrow(
		builder, NULL, sizeof(*layout), &cli_chat_layout_ops, NULL);
	if (!layout)
		return;
	layout->page = page;
	struct morph_tui_view *root = &layout->base.view;
	page->scroll = morph_tui_widget_scroll_add_borrow(builder, root, NULL);
	page->feed = morph_tui_widget_stack_add_borrow(builder, page->scroll,
						       MORPH_TUI_WIDGET_STACK_VERTICAL, 0, NULL);
	cli_banner_add_borrow(builder, page->feed, page->ctx);
	const struct morph_tui_view_options jobs = {.max_height = 3};
	page->jobs = morph_tui_widget_text_view_add_borrow(builder, root, "", &jobs);
	morph_tui_widget_text_view_set_content_sizing(page->jobs, 1);
	morph_tui_view_set_focusable(page->jobs, 0);
	page->status = morph_tui_widget_text_view_add_borrow(builder, root, "",
		&(struct morph_tui_view_options){.margin = {.top = 1}});
	morph_tui_widget_text_view_set_content_sizing(page->status, 1);
	morph_tui_view_set_focusable(page->status, 0);
	morph_tui_view_set_hidden(page->status, 1);
	page->agents = morph_tui_widget_text_view_add_borrow(builder, root, "", &jobs);
	morph_tui_widget_text_view_set_content_sizing(page->agents, 1);
	morph_tui_view_set_focusable(page->agents, 0);
	morph_tui_view_set_hidden(page->agents, 1);
	page->queue_title = morph_tui_widget_label_add_borrow(
		builder, root, CLI_FEED_PENDING " Messages queued · Esc to apply now", NULL);
	const struct morph_tui_view_options queue = {.max_height = 4};
	page->queue = morph_tui_widget_list_add_borrow(builder, root, NULL, 0, 0, &queue);
	morph_tui_view_set_focusable(page->queue, 0);
	morph_tui_view_set_hidden(page->queue, 1);
	morph_tui_view_set_hidden(page->queue_title, 1);
	morph_tui_view_set_hidden(page->jobs, 1);
	page->panel_host = morph_tui_widget_stack_add_borrow(builder, root,
		MORPH_TUI_WIDGET_STACK_VERTICAL, 0, NULL);
	morph_tui_view_set_hidden(page->panel_host, 1);
	page->composer = cli_composer_add_borrow(builder, root, page->ctx, &page->editor);
	page->footer = morph_tui_widget_text_view_add_borrow(builder, root, "",
		&(struct morph_tui_view_options){.padding = {.left = 2, .right = 2}});
	morph_tui_widget_text_view_set_content_sizing(page->footer, 1);
	morph_tui_view_set_focusable(page->footer, 0);
	if (!morph_tui_builder_error(builder)) {
		morph_tui_builder_fail(builder,
				       morph_tui_widget_text_field_set_multiline(page->editor, 1));
		morph_tui_builder_fail(builder,
			morph_tui_widget_text_field_set_prompt_hanging_indent(page->editor, 0));
		morph_tui_builder_fail(builder,
				       morph_tui_widget_text_field_set_completion(page->editor, 1));
		morph_tui_builder_fail(
			builder, morph_tui_widget_text_field_set_prompt_copy(page->editor, "› "));
		page->ready = 1;
		morph_tui_view_focus(page->editor);
	}
}

static int cli_chat_page_refresh(struct cli_chat_page *page);

static int cli_chat_page_appear(struct morph_tui_view_controller *controller)
{
	struct cli_chat_page *page = controller->data;
	int delivery_rc = cli_event_queue_set_events_enabled(page->ctx, 1);
	if (delivery_rc)
		return delivery_rc;
	if (page->ctx->session_history_pending) {
		page->ctx->session_history_pending = 0;
		int rc = cli_session_history_restore(page->ctx);
		if (rc)
			return rc;
	}
	if (page->ctx->application_command && !page->panel) {
		page->ctx->application_command = 0;
		cli_presentation_command_end(page->ctx);
	}
	int rc = cli_chat_page_refresh(page);
	return rc ? rc : morph_tui_view_focus(page->panel ? page->panel : page->editor);
}

static void cli_chat_page_closed(struct morph_tui_view_controller *controller, int result)
{
	struct cli_chat_page *page = controller->data;
	(void)result;
	page->ready = 0;
	page->controller = NULL;
}

static void cli_chat_page_finish_job(struct cli_chat_page *page)
{
	struct cli_application *ctx = page->ctx;
	int result = cli_command_job_finish(page->job);
	(void)cli_event_queue_drain(ctx);
	cli_turn_finish(ctx, result);
	char *display NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *pending NB_CLEANUP(nb_charp_cleanup) =
		cli_command_job_take_prompt(page->job, &display);
	if (pending) {
		cli_presentation_user_input(ctx, display ? display : pending);
		cli_turn_begin(ctx);
		result = cli_command_job_start(page->job, ctx, pending);
		if (result)
			cli_turn_finish(ctx, result);
	}
}

static int cli_chat_page_refresh(struct cli_chat_page *page)
{
	struct cli_application *ctx = page->ctx;
	if (!page->ready)
		return 0;
	int64_t now = cli_chat_now();
	if (ctx->jobs) {
		int64_t session = 0;

		if (!runtime_session_current_id(ctx->runtime, &session))
			cli_jobs_model_set_session(ctx->jobs, session);
	}
	if (ctx->turn_active && !cli_jobs_model_waiting(ctx->jobs)) {
		nb_buf activity NB_CLEANUP(nb_buf_cleanup) = {0};
		nb_buf agents NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = (ctx->agents.count || ctx->composer.ultra) ?
			cli_agents_summary(&agents, ctx->agents.items, ctx->agents.count,
					   page->columns) : 0;
		if (rc)
			return rc;
		if (page->activity_text)
			rc = nb_buf_append_cstr(&activity, page->activity_text) ? 0 : NB_ERR_NOMEM;
		else if (page->agents_waiting) {
			int styled = cli_output_color_enabled(ctx);

			rc = (nb_buf_printf(&activity,
					    "%s" CLI_FEED_RUNNING "%s Waiting for agents",
					    styled ? cli_style_running_color(now) : "",
					    styled ? ANSI_RESET : "")
				      ? 0
				      : NB_ERR_NOMEM);
			if (!rc && agents.len)
				rc = (nb_buf_printf(&activity, " · %s", agents.ptr) ? 0
										    : NB_ERR_NOMEM);
		} else
			rc = cli_work_status_running(
				&activity, page->columns, cli_output_color_enabled(ctx),
				now - page->turn_started, now, nb_buf_cstr(&agents));
		if (!rc)
			rc = cli_messages_set_rendered_view(page->status, nb_buf_cstr(&activity),
							    cli_output_color_enabled(ctx));
		if (!rc)
			rc = morph_tui_view_set_hidden(page->status, 0);
		if (rc)
			return rc;
	}
	if (now / 1000 != ctx->input_status.updated) {
		struct permission_policy policy;
		if (!runtime_permission_get_policy(ctx->runtime, &policy)) {
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
		if (!runtime_model_get_ultra(ctx->runtime, &ultra)) {
			cli_composer_model_set_ultra(&ctx->composer, ultra,
				cli_output_color_enabled(ctx));
			morph_tui_widget_text_field_set_prompt_copy(page->editor,
				ultra ? "» " : cli_output_color_enabled(ctx) ? "› " : "> ");
			struct morph_tui_text_style prompt = {0};
			if (cli_output_color_enabled(ctx)) {
				prompt.foreground = (struct morph_tui_color){
					MORPH_TUI_COLOR_INDEXED,
					ultra ? CLI_INPUT_ULTRA_INDEX : CLI_INPUT_MARKER_INDEX};
				prompt.attributes = ultra ? MORPH_TUI_STYLE_BOLD : 0;
			}
			morph_tui_widget_text_field_set_prompt_style(page->editor, prompt);
		}
		int rc = cli_chat_page_footer(page, page->columns);
		if (rc)
			return rc;
	}
	int animation_rc = cli_composer_update(page->composer, now);
	if (animation_rc)
		return animation_rc;
	int timeout = ctx->composer.mode_transition ? 33 :
		ctx->turn_active ? cli_work_status_interval_ms(now - page->turn_started, now) : 100;
	morph_tui_application_schedule(&ctx->base, timeout);
	char *items[4] = {0};
	size_t total = 0;
	int rc = cli_command_job_prompts_copy(page->job, items, 3, &total);
	if (rc)
		return rc;
	size_t shown = total < 3 ? total : 3;
	for (size_t i = 0; !rc && i < shown; i++) {
		nb_buf row NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_printf(&row, "%s%s", i + 1 < total ? "├ " : "└ ", items[i])
			      ? 0
			      : NB_ERR_NOMEM);
		if (!rc) {
			free(items[i]);
			items[i] = (nb_buf_ok(&row) ? strdup(nb_buf_cstr(&row)) : NULL);
			if (!items[i])
				rc = NB_ERR_NOMEM;
		}
	}
	if (!rc && total > shown) {
		nb_buf row NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_printf(&row, "└ +%zu more queued", total - shown) ? 0 : NB_ERR_NOMEM);
		if (!rc) {
			items[shown] = strdup(nb_buf_cstr(&row));
			if (!items[shown])
				rc = NB_ERR_NOMEM;
			else
				shown++;
		}
	}
	if (!rc)
		rc = morph_tui_widget_list_set_items_copy(page->queue, (const char *const *)items,
							  shown);
	for (size_t i = 0; i < shown; i++)
		free(items[i]);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->queue, !shown);
	if (!rc)
		rc = morph_tui_view_set_hidden(page->queue_title, !shown);
	nb_buf jobs NB_CLEANUP(nb_buf_cleanup) = {0};
	if (ctx->jobs)
		(void)cli_jobs_render(ctx->jobs, &jobs, page->columns,
				      cli_output_color_enabled(ctx));
	if (!rc)
		rc = nb_buf_status(&jobs);
	if (!rc)
		rc = cli_messages_set_rendered_view(page->jobs, jobs.ptr ? jobs.ptr : "",
						    cli_output_color_enabled(ctx));
	if (!rc)
		rc = morph_tui_view_set_hidden(page->jobs, !jobs.len);
	if (!rc && cli_jobs_model_waiting(ctx->jobs))
		rc = morph_tui_view_set_hidden(page->status, 1);
	nb_buf agents NB_CLEANUP(nb_buf_cleanup) = {0};
	(void)cli_agents_rows(&agents, ctx->agents.items, ctx->agents.count,
		page->columns, 3, cli_output_color_enabled(ctx));
	if (!rc)
		rc = nb_buf_status(&agents);
	if (!rc)
		rc = cli_messages_set_rendered_view(page->agents, agents.ptr ? agents.ptr : "",
						    cli_output_color_enabled(ctx));
	if (!rc)
		rc = morph_tui_view_set_hidden(page->agents, !agents.len);
	if (!rc)
		rc = cli_chat_page_error(page);
	if (!rc && (page->panel || page->prompt_rows))
		rc = cli_chat_panel_hide_activity(page);
	return rc;
}

static int cli_chat_page_event(void *data, struct morph_tui_application *app,
			       const struct morph_tui_event *event)
{
	struct cli_chat_page *page = data;
	struct cli_application *ctx = page->ctx;
	cli_recap_tick(ctx, cli_recap_now());
	if (nb_atomic_int_load(&ctx->terminate_requested)) {
		ctx->running = 0;
		morph_tui_application_stop(app, 0);
		return 1;
	}
	int modal = page->panel || morph_tui_application_depth(app) > 1;
	if (modal && cli_event_queue_owner_call_pending(ctx)) {
		if (page->panel)
			(void)cli_chat_panel_cancel(ctx, NB_ERR_CANCELLED);
		else if (!morph_tui_application_transition_pending(app))
			(void)morph_tui_application_dismiss_controller(app, 0);
		(void)morph_tui_application_wake(app);
		return 0;
	}
	if (event->type == MORPH_TUI_EVENT_WAKE || event->type == MORPH_TUI_EVENT_TICK) {
		int rc = cli_event_queue_drain_batch(ctx);
		if (rc)
			return rc;
	}
	if (!modal && cli_command_job_done(page->job))
		cli_chat_page_finish_job(page);
	if (!modal && ctx->resume_requested && !morph_tui_application_transition_pending(app)) {
		ctx->resume_requested = 0;
		(void)cli_resume_show(ctx);
	}
	if (nb_atomic_int_exchange(&ctx->interrupted, 0)) {
		if (page->panel)
			(void)cli_chat_panel_cancel(ctx, NB_ERR_CANCELLED);
		else if (modal)
			(void)morph_tui_application_dismiss_controller(app, 0);
		else if (page->job->active)
			runtime_cancel_turn(ctx->runtime);
	}
	if (nb_atomic_int_exchange(&ctx->resized, 0))
		morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_LAYOUT);
	if (!ctx->running)
		morph_tui_application_stop(app, 0);
	if (page->panel && (event->type == MORPH_TUI_EVENT_TICK ||
	    event->type == MORPH_TUI_EVENT_WAKE || event->type == MORPH_TUI_EVENT_RESIZE)) {
		int rc = cli_composer_panel_update(page->panel, event);
		if (rc < 0)
			return rc;
	}
	return cli_chat_page_refresh(page);
}

static void cli_chat_page_cleanup(struct cli_chat_page **owner)
{
	if (owner && *owner) {
		cli_chat_destroy(*owner, 0);
		*owner = NULL;
	}
}

static struct cli_chat_page *cli_chat_page_steal(struct cli_chat_page **owner)
{
	struct cli_chat_page *page = *owner;
	*owner = NULL;
	return page;
}

int cli_chat_create(struct cli_application *ctx, struct cli_command_job *job,
		    struct cli_chat_page **out)
{
	if (!ctx || !job || !out || *out || ctx->chat)
		return NB_ERR_INVAL;
	struct cli_chat_page *page NB_CLEANUP(cli_chat_page_cleanup) = calloc(1, sizeof(*page));
	if (!page)
		return NB_ERR_NOMEM;
	page->ctx = ctx;
	page->job = job;
	int rc =
		(nb_buf_init(&page->stream), nb_buf_reserve(&page->stream, 128) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_init(&page->output),
		      nb_buf_reserve(&page->output, 128) ? 0 : NB_ERR_NOMEM);
	if (!rc) {
		page->tools = nb_map_new_with_cap(16);
		if (!page->tools)
			rc = NB_ERR_NOMEM;
	}
	char *entry = NULL;
	if (!rc)
		rc = (nb_vec_init(&page->history, sizeof(entry)),
		      nb_vec_reserve(&page->history, 16) ? 0 : NB_ERR_NOMEM);
	if (rc)
		return rc;
	ctx->chat = page;
	cli_feed_set_hidden(ctx, 1);
	*out = cli_chat_page_steal(&page);
	return 0;
}

struct morph_tui_application_options cli_chat_options(struct cli_chat_page *page)
{
	return (struct morph_tui_application_options){.external_signals = 1,
						      .input = stdin,
						      .output = stdout,
						      .event = cli_chat_page_event,
						      .data = page};
}

int cli_chat_present(struct cli_chat_page *page)
{
	static const struct morph_tui_view_controller_ops ops = {.build = cli_chat_page_build,
								 .action = cli_chat_page_action,
								 .event = cli_chat_page_key,
								 .appear = cli_chat_page_appear,
								 .destroy = cli_chat_page_closed};
	const struct morph_tui_view_controller_spec spec = {
		.ops = &ops,
		.data = page,
		.tick_ms = 100,
		.terminal_flags =
			MORPH_TUI_TERMINAL_SESSION_MOUSE | MORPH_TUI_TERMINAL_SESSION_FOCUS};
	return morph_tui_view_controller_present_borrow(&page->ctx->base, &spec);
}

void cli_chat_job_finished(struct cli_chat_page *page, int result)
{
	if (page && page->ctx->turn_active)
		cli_turn_finish(page->ctx, result);
}

void cli_chat_destroy(struct cli_chat_page *page, int result)
{
	if (!page)
		return;
	struct cli_application *ctx = page->ctx;
	char **entry;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&page->history) && (entry = (char **)nb_vec_at(&page->history, _i_));
	     _i_++)
		free(*entry);
	nb_vec_cleanup(&page->history);
	nb_map_free(page->tools);
	page->tools = NULL;
	nb_buf_cleanup(&page->stream);
	nb_buf_cleanup(&page->output);
	free(page->draft);
	free(page->last_assistant_text);
	free(page->footer_text);
	free(page->activity_text);
	if (page->ctx->chat == page)
		page->ctx->chat = NULL;
	free(page);
	if (result < 0)
		CMD_ERROR(ctx, "Application failed: %s",
			  nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, result));
}
