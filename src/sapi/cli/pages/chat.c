#include "sapi/cli/pages/chat.h"
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
#include "sapi/cli/widgets/notice.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/command_job.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/services/recap.h"
#include "sapi/cli/services/turn.h"
#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/services/composer.h"
#include "sapi/cli/adapters/clipboard/clipboard.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/widgets/message.h"
#include "sapi/cli/widgets/tool_call.h"
#include "sapi/cli/widgets/feed_block.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/plan.h"
#include "sapi/cli/application_internal.h"
#include "util/cleanup.h"
#include "util/strmap.h"
#include <morph/tui/view_controller.h>
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
	struct morph_tui_view *scroll, *feed, *editor, *status, *footer;
	struct morph_tui_view *stream_view, *output_view, *queue_title, *queue, *jobs;
	struct morph_tui_view *last_assistant, *command_group;
	char *last_assistant_text;
	morph_buf_t stream, output;
	morph_strmap_t tools;
	morph_array_t history;
	char *draft;
	size_t history_index;
	int stream_kind, ready;
	int64_t turn_started;
};

static int64_t cli_chat_now(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int cli_chat_active(const struct cli_application *ctx)
{
	return ctx && ctx->chat && ctx->chat->ready;
}

int cli_chat_flush(struct cli_application *ctx)
{
	return cli_chat_active(ctx) ? morph_tui_application_render(&ctx->base) : 0;
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
	free(page->last_assistant_text);
	page->last_assistant_text = NULL;
	morph_buf_reset(&page->stream);
	morph_buf_reset(&page->output);
	morph_strmap_clear(&page->tools);
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
	morph_buf_reset(&page->stream);
	morph_buf_reset(&page->output);
}

void cli_chat_transcript_reset(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return;
	cli_chat_page_boundary(ctx->chat);
	morph_strmap_clear(&ctx->chat->tools);
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
	cli_message_add_borrow(page->builder, page->feed, (enum cli_message_kind)kind,
			       text ? text : "", cli_output_color_enabled(ctx));
	return cli_chat_page_error(page);
}

int cli_chat_command_input(struct cli_application *ctx, const char *text)
{
	if (!cli_chat_active(ctx) || !text)
		return -EINVAL;
	struct cli_chat_page *page = ctx->chat;
	cli_chat_page_boundary(page);
	page->command_group = morph_tui_widget_stack_add_borrow(
		page->builder, page->feed, MORPH_TUI_WIDGET_STACK_VERTICAL, 1, NULL);
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
		const struct morph_tui_view_options options = {0};
		page->output_view = morph_tui_widget_text_view_add_borrow(
			page->builder, page->command_group ? page->command_group : page->feed, "",
			&options);
		if (!page->output_view)
			return cli_chat_page_error(page);
		morph_tui_widget_text_view_set_content_sizing(page->output_view, 1);
		morph_tui_view_set_focusable(page->output_view, 0);
	}
	int rc = morph_buf_puts(&page->output, text);
	if (!rc)
		rc = cli_messages_set_rendered_view(page->output_view,
						    morph_buf_cstr_borrow(&page->output),
						    cli_output_color_enabled(ctx));
	return rc;
}

void cli_chat_activity(struct cli_application *ctx, const char *text)
{
	if (cli_chat_active(ctx))
		(void)morph_tui_widget_label_set_text_copy(ctx->chat->status, text ? text : "");
}

void cli_chat_turn_begin(struct cli_application *ctx)
{
	if (!cli_chat_active(ctx))
		return;
	cli_chat_page_boundary(ctx->chat);
	ctx->chat->turn_started = cli_chat_now();
	cli_chat_activity(ctx, CLI_FEED_RUNNING " Working · Esc to interrupt");
}

void cli_chat_turn_end(struct cli_application *ctx, int result)
{
	if (!cli_chat_active(ctx))
		return;
	struct cli_chat_page *page = ctx->chat;
	cli_chat_page_boundary(page);
	morph_buf_t receipt MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_printf(&receipt, "%s %s %.1fs",
				  result >= 0		 ? CLI_FEED_DONE
				  : result == -ECANCELED ? CLI_FEED_CANCELLED
							 : CLI_FEED_FAILED,
				  result >= 0		 ? "Worked for"
				  : result == -ECANCELED ? "Stopped after"
							 : "Failed after",
				  (double)(cli_chat_now() - page->turn_started) / 1000.0);
	morph_tui_builder_fail(page->builder, rc);
	const struct morph_tui_view_options options = {
		.style = {.attributes = cli_output_color_enabled(ctx) ? MORPH_TUI_STYLE_DIM : 0}};
	if (!rc)
		morph_tui_widget_label_add_borrow(page->builder, page->feed,
						  morph_buf_cstr_borrow(&receipt), &options);
	cli_chat_activity(ctx, "");
}

int cli_chat_tool_changed(struct cli_application *ctx, const struct cli_transcript_tool *tool)
{
	if (!cli_chat_active(ctx))
		return 0;
	struct cli_chat_page *page = ctx->chat;
	struct morph_tui_view *view = morph_strmap_borrow(&page->tools, tool->id);
	int rc;
	if (!view) {
		cli_chat_page_boundary(page);
		page->last_assistant = NULL;
		view = cli_tool_call_add_borrow(page->builder, page->feed, tool,
						cli_output_color_enabled(ctx), ctx->tool_details);
		if (!view || cli_chat_page_error(page))
			return cli_chat_page_error(page);
		rc = morph_strmap_set_borrow(&page->tools, tool->id, view);
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
		morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
		int rc = morph_buf_printf(
			&text, "%s Agent %s",
			block->content.agent.status == AGENT_COMPLETED	 ? CLI_FEED_DONE
			: block->content.agent.status == AGENT_CANCELLED ? CLI_FEED_CANCELLED
									 : CLI_FEED_FAILED,
			block->content.agent.id);
		if (!rc)
			rc = cli_chat_output(ctx, morph_buf_cstr_borrow(&text));
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
		morph_buf_reset(&page->stream);
	int rc = morph_buf_puts(&page->stream, text ? text : "");
	if (!rc)
		rc = cli_message_set_text_copy(page->stream_view,
					       morph_buf_cstr_borrow(&page->stream));
	if (!rc && kind == CLI_MESSAGE_ASSISTANT) {
		char *copy MORPH_CLEANUP(morph_charp_cleanup) =
			strdup(morph_buf_cstr_borrow(&page->stream));
		if (!copy)
			return -ENOMEM;
		free(page->last_assistant_text);
		page->last_assistant_text = morph_charp_steal(&copy);
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
	if (!strcmp(name, "react.reasoning.delta"))
		rc = cli_chat_page_stream(page, CLI_MESSAGE_REASONING, text, 1);
	else if (!strcmp(name, "react.content.delta") || !strcmp(name, "react.thought.delta") ||
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
		cli_chat_activity(ctx, CLI_FEED_RUNNING " Thinking");
	} else if (!strcmp(name, "react.user.steer")) {
		const char *display = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(event->data, "display_text"));
		rc = cli_chat_message(ctx, CLI_MESSAGE_USER, display ? display : text);
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
	char *input MORPH_CLEANUP(morph_charp_cleanup) = strdup(text);
	if (!input)
		return -ENOMEM;
	char **remembered = morph_array_push_borrow(&page->history);
	if (!remembered)
		return -ENOMEM;
	*remembered = strdup(text);
	if (!*remembered) {
		morph_array_pop(&page->history);
		return -ENOMEM;
	}
	page->history_index = page->history.nelts;
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
		if (morph_tui_application_transition_pending(app))
			ctx->application_command = 1;
		else
			cli_presentation_command_end(ctx);
	} else {
		char *converted MORPH_CLEANUP(morph_charp_cleanup) = NULL;
		int converted_count =
			cli_composer_service_convert_paths_copy(&ctx->composer, input, &converted);
		if (converted_count < 0)
			return converted_count;
		if (converted_count > 0) {
			free(input);
			input = morph_charp_steal(&converted);
		}
		char *expanded MORPH_CLEANUP(morph_charp_cleanup) =
			cli_composer_model_expand_copy(&ctx->composer, input);
		if (!expanded)
			return -ENOMEM;
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
	char *candidate MORPH_CLEANUP(morph_charp_cleanup) =
		cli_command_completion_generator(input, 0);
	if (!candidate)
		return 0;
	morph_buf_t choices MORPH_CLEANUP(morph_buf_cleanup) = {0};
	char *common MORPH_CLEANUP(morph_charp_cleanup) = strdup(candidate);
	if (!common)
		return -ENOMEM;
	int count = 0, rc = 0;
	while (candidate && !rc) {
		count++;
		if (choices.len)
			rc = morph_buf_puts(&choices, "  ");
		if (!rc)
			rc = morph_buf_puts(&choices, candidate);
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
		morph_buf_t suffix MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_puts(&suffix, common + strlen(input));
		if (!rc && count == 1)
			rc = morph_buf_putc(&suffix, ' ');
		if (!rc)
			rc = morph_tui_widget_text_field_insert_text_copy(
				page->editor, morph_buf_cstr_borrow(&suffix));
	} else {
		cli_chat_page_boundary(page);
		rc = cli_chat_output(page->ctx, morph_buf_cstr_borrow(&choices));
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
		char *path MORPH_CLEANUP(morph_charp_cleanup) = NULL;
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
		if (!page->history.nelts)
			return 1;
		if (page->history_index == page->history.nelts) {
			char *draft MORPH_CLEANUP(morph_charp_cleanup) =
				strdup(morph_tui_widget_text_field_text_borrow(page->editor));
			if (!draft)
				return -ENOMEM;
			free(page->draft);
			page->draft = morph_charp_steal(&draft);
		}
		if (key == MORPH_TUI_KEY_UP && page->history_index)
			page->history_index--;
		if (key == MORPH_TUI_KEY_DOWN && page->history_index < page->history.nelts)
			page->history_index++;
		const char *value = page->draft ? page->draft : "";
		if (page->history_index < page->history.nelts)
			value = *(char **)morph_array_borrow(&page->history, page->history_index);
		int rc = morph_tui_widget_text_field_set_text_copy(page->editor, value);
		return rc ? rc : 1;
	}
	if (key == 20) {
		ctx->tool_details = !ctx->tool_details;
		if (ctx->transcript) {
			const struct cli_transcript_tool *tool;
			morph_array_foreach (tool, &ctx->transcript->tools,
					     struct cli_transcript_tool) {
				int rc = cli_chat_tool_changed(ctx, tool);
				if (rc)
					return rc;
			}
		}
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
		} else if (key == 3)
			morph_tui_widget_text_field_set_text_copy(page->editor, "");
		return 1;
	}
	if (key == 4) {
		ctx->running = 0;
		morph_tui_application_stop(&ctx->base, 0);
		return 1;
	}
	return 0;
}

static void cli_chat_page_build(struct morph_tui_view_controller *controller,
				struct morph_tui_builder *builder)
{
	struct cli_chat_page *page = controller->data;
	page->controller = controller;
	page->builder = builder;
	const struct morph_tui_view_options inset = {.padding = {.right = 1}};
	const struct morph_tui_view_options fill = {.grow = 1};
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(
		builder, NULL, MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &inset);
	morph_tui_widget_label_add_borrow(builder, root, "morph · conversation", NULL);
	page->scroll = morph_tui_widget_scroll_add_borrow(builder, root, &fill);
	page->feed = morph_tui_widget_stack_add_borrow(builder, page->scroll,
						       MORPH_TUI_WIDGET_STACK_VERTICAL, 1, NULL);
	const struct morph_tui_view_options jobs = {.max_height = 3};
	page->jobs = morph_tui_widget_text_view_add_borrow(builder, root, "", &jobs);
	morph_tui_widget_text_view_set_content_sizing(page->jobs, 1);
	morph_tui_view_set_focusable(page->jobs, 0);
	page->status = morph_tui_widget_label_add_borrow(builder, root, "", NULL);
	page->queue_title = morph_tui_widget_label_add_borrow(
		builder, root, CLI_FEED_PENDING " Messages queued · Esc to apply now", NULL);
	const struct morph_tui_view_options queue = {.max_height = 4};
	page->queue = morph_tui_widget_list_add_borrow(builder, root, NULL, 0, 0, &queue);
	morph_tui_view_set_focusable(page->queue, 0);
	morph_tui_view_set_hidden(page->queue, 1);
	morph_tui_view_set_hidden(page->queue_title, 1);
	morph_tui_view_set_hidden(page->jobs, 1);
	struct morph_tui_view_options composer = {
		.margin = {.top = 1}, .padding = {1, 1, 1, 0}, .max_height = 8};
	if (cli_output_color_enabled(page->ctx))
		composer.style.background = (struct morph_tui_color){MORPH_TUI_COLOR_RGB, 0x3a3a3a};
	page->editor = morph_tui_widget_text_field_add_borrow(builder, root, "", 1024u * 1024u, 0,
							      &composer);
	page->footer = morph_tui_widget_label_add_borrow(builder, root, "", NULL);
	morph_tui_widget_label_add_borrow(
		builder, root,
		"Enter send · Ctrl+J newline · PgUp/PgDn scroll · Ctrl+T tools · Ctrl+D quit",
		NULL);
	if (!morph_tui_builder_error(builder)) {
		morph_tui_builder_fail(builder,
				       morph_tui_widget_text_field_set_multiline(page->editor, 1));
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
	if (page->ctx->session_history_pending) {
		page->ctx->session_history_pending = 0;
		int rc = cli_session_history_restore(page->ctx);
		if (rc)
			return rc;
	}
	if (page->ctx->application_command) {
		page->ctx->application_command = 0;
		cli_presentation_command_end(page->ctx);
	}
	int rc = cli_chat_page_refresh(page);
	return rc ? rc : morph_tui_view_focus(page->editor);
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
	char *display MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *pending MORPH_CLEANUP(morph_charp_cleanup) =
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
	if (ctx->turn_active) {
		morph_buf_t activity MORPH_CLEANUP(morph_buf_cleanup) = {0};
		int rc = morph_buf_printf(&activity,
					  CLI_FEED_RUNNING " Working · %llds · Esc to interrupt",
					  (long long)((now - page->turn_started) / 1000));
		if (!rc)
			rc = morph_tui_widget_label_set_text_copy(page->status,
								  morph_buf_cstr_borrow(&activity));
		struct morph_tui_text_style style = {0};
		if (cli_output_color_enabled(ctx)) {
			style.foreground = (struct morph_tui_color){MORPH_TUI_COLOR_INDEXED, 6};
			style.attributes =
				(now / CLI_STYLE_RUNNING_PULSE_MS) % 2 ? MORPH_TUI_STYLE_DIM : 0;
		}
		if (!rc)
			rc = morph_tui_view_set_style(page->status, style);
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
		struct credit_summary credits = {0};
		if (!runtime_credit_summary_current_get(ctx->runtime, &credits))
			ctx->input_status.credits = credits.credits;
		ctx->input_status.updated = now / 1000;
		int ultra = 0;
		if (!runtime_model_get_ultra(ctx->runtime, &ultra)) {
			cli_composer_model_set_ultra(&ctx->composer, ultra, 0);
			morph_tui_widget_text_field_set_prompt_copy(page->editor,
								    ultra ? "» " : "› ");
			struct morph_tui_text_style prompt = {0};
			if (cli_output_color_enabled(ctx)) {
				prompt.foreground = (struct morph_tui_color){
					MORPH_TUI_COLOR_RGB, ultra ? 0xaf87ffu : 0x8a8a8au};
				prompt.attributes = ultra ? MORPH_TUI_STYLE_BOLD : 0;
			}
			morph_tui_widget_text_field_set_prompt_style(page->editor, prompt);
		}
		morph_buf_t footer MORPH_CLEANUP(morph_buf_cleanup) = {0};
		const char *permission =
			ctx->input_status.permissions_ready
				? runtime_permission_preset_name(&ctx->input_status.permissions)
				: "?";
		int rc = morph_buf_printf(
			&footer, "morph · %s · perm: %s · context %d / %d · %lld credits%s",
			runtime_config_borrow(ctx->runtime)->models.text.model, permission,
			ctx->input_status.tokens, ctx->input_status.limit,
			(long long)ctx->input_status.credits,
			ctx->input_status.session_titles_pending ? " · Naming session..." : "");
		if (!rc)
			rc = morph_tui_widget_label_set_text_copy(page->footer,
								  morph_buf_cstr_borrow(&footer));
		if (rc)
			return rc;
	}
	char *items[4] = {0};
	size_t total = 0;
	int rc = cli_command_job_prompts_copy(page->job, items, 3, &total);
	if (rc)
		return rc;
	size_t shown = total < 3 ? total : 3;
	for (size_t i = 0; !rc && i < shown; i++) {
		morph_buf_t row MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_printf(&row, "%s%s", i + 1 < total ? "├ " : "└ ", items[i]);
		if (!rc) {
			free(items[i]);
			items[i] = morph_buf_detach(&row);
		}
	}
	if (!rc && total > shown) {
		morph_buf_t row MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_printf(&row, "└ +%zu more queued", total - shown);
		if (!rc)
			items[shown++] = morph_buf_detach(&row);
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
	morph_buf_t jobs MORPH_CLEANUP(morph_buf_cleanup) = {0};
	if (ctx->jobs) {
		int count = cli_jobs_model_count(ctx->jobs);
		if (count > 0)
			(void)morph_buf_printf(&jobs,
					       CLI_FEED_RUNNING " %d background job%s · /jobs",
					       count, count == 1 ? "" : "s");
		int shown_jobs = 0;
		const struct cli_jobs_job *job;
		morph_array_foreach (job, &ctx->jobs->entries, struct cli_jobs_job) {
			if (!cli_jobs_job_running(job->data) ||
			    cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
				    job->data, "owner_session_id")) !=
				    (double)ctx->jobs->session_id)
				continue;
			if (jobs.len)
				(void)morph_buf_putc(&jobs, '\n');
			(void)morph_buf_printf(&jobs, "%s%s · /jobs %.8s",
					       shown_jobs + 1 < count ? "├ " : "└ ",
					       cli_jobs_job_string(job->data, "description"),
					       cli_jobs_job_short_id(job->data));
			if (++shown_jobs == 2)
				break;
		}
	}
	if (!rc)
		rc = jobs.failed;
	if (!rc)
		rc = morph_tui_widget_text_view_set_text_copy(
			page->jobs, jobs.data ? morph_buf_cstr_borrow(&jobs) : "");
	if (!rc)
		rc = morph_tui_view_set_hidden(page->jobs, !jobs.len);
	struct morph_tui_text_style jobs_style = {0};
	if (jobs.len && cli_output_color_enabled(ctx)) {
		jobs_style.foreground = (struct morph_tui_color){MORPH_TUI_COLOR_INDEXED, 6};
		jobs_style.attributes =
			(now / CLI_STYLE_RUNNING_PULSE_MS) % 2 ? MORPH_TUI_STYLE_DIM : 0;
	}
	if (!rc)
		rc = morph_tui_view_set_style(page->jobs, jobs_style);
	if (!rc)
		rc = cli_chat_page_error(page);
	return rc;
}

static int cli_chat_page_event(void *data, struct morph_tui_application *app,
			       const struct morph_tui_event *event)
{
	struct cli_chat_page *page = data;
	struct cli_application *ctx = page->ctx;
	cli_recap_tick(ctx, cli_recap_now());
	if (morph_signal_flag_get(&ctx->terminate_requested)) {
		ctx->running = 0;
		morph_tui_application_stop(app, 0);
		return 1;
	}
	int modal = morph_tui_application_depth(app) > 1;
	if (modal && cli_event_queue_owner_call_pending(ctx)) {
		if (!morph_tui_application_transition_pending(app))
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
	if (morph_signal_flag_take(&ctx->interrupted)) {
		if (modal)
			(void)morph_tui_application_dismiss_controller(app, 0);
		else if (page->job->active)
			runtime_cancel_turn(ctx->runtime);
	}
	if (morph_signal_flag_take(&ctx->resized))
		morph_tui_application_invalidate(app, MORPH_TUI_INVALIDATE_LAYOUT);
	if (!ctx->running)
		morph_tui_application_stop(app, 0);
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
		return -EINVAL;
	struct cli_chat_page *page MORPH_CLEANUP(cli_chat_page_cleanup) = calloc(1, sizeof(*page));
	if (!page)
		return -ENOMEM;
	page->ctx = ctx;
	page->job = job;
	int rc = morph_buf_init(&page->stream, 128);
	if (!rc)
		rc = morph_buf_init(&page->output, 128);
	if (!rc)
		rc = morph_strmap_init(&page->tools, 16);
	char *entry = NULL;
	if (!rc)
		rc = morph_array_init(&page->history, 16, sizeof(entry));
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
	morph_array_foreach (entry, &page->history, char *)
		free(*entry);
	morph_array_cleanup(&page->history);
	morph_strmap_cleanup(&page->tools);
	morph_buf_cleanup(&page->stream);
	morph_buf_cleanup(&page->output);
	free(page->draft);
	free(page->last_assistant_text);
	if (page->ctx->chat == page)
		page->ctx->chat = NULL;
	free(page);
	if (result < 0)
		CMD_ERROR(ctx, "Application failed: %s", morph_strerror(result));
}
