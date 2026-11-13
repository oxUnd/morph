#include "sapi/cli/pages/subagents.h"
#include "nb/time.h"
#include "nb/err.h"

#include "sapi/cli/application.h"
#include "config/config.h"
#include "runtime/runtime.h"
#include "sapi/cli/models/subagents.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/agents_browser.h"
#include "sapi/cli/widgets/composer_panel.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/widgets/subagents.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/frame.h>
#include <morph/tui/input.h>
#include <morph/tui/widgets/list.h>
#include <morph/tui/widgets/text_view.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "sapi/cli/services/agents.h"

struct cli_subagents_feed {
	struct morph_tui_frame frame;
	struct morph_tui_widget_text_layout lines;
	struct agent_info *detail;
	struct cli_subagents_thread *thread;
	nb_buf document;
	nb_buf draft;
	nb_buf message;
	size_t scroll;
	int editing;
	int details;
	int follow;
	int dirty;
	int columns;
	int rows;
	int64_t rendered;
};

struct cli_subagents_page {
	struct cli_agents_browser_state browser;
	struct cli_application *ctx;
	struct morph_tui_widget_list_state selection;
	struct agent_info *items;
	int count;
	nb_vec feeds;
	struct cli_subagents_feed *feed;
	struct morph_tui_frame picker_frame;
	int fullscreen;
	int open;
	int viewing;
	int64_t session;
	int64_t refreshed;
};

static void cli_subagents_detail_cleanup(struct agent_info **detail)
{
	cli_agents_list_free(*detail, *detail ? 1 : 0);
}

static struct agent_info *cli_subagents_detail_steal(struct agent_info **detail)
{
	struct agent_info *owned = *detail;
	*detail = NULL;
	return owned;
}

static void cli_subagents_feed_destroy(struct cli_subagents_feed *feed)
{
	if (!feed)
		return;
	cli_agents_list_free(feed->detail, feed->detail ? 1 : 0);
	cli_subagents_thread_destroy(feed->thread);
	nb_buf_cleanup(&feed->document);
	nb_buf_cleanup(&feed->draft);
	nb_buf_cleanup(&feed->message);
	morph_tui_widget_text_layout_cleanup(&feed->lines);
	morph_tui_frame_cleanup(&feed->frame);
	free(feed);
}

static void cli_subagents_feed_cleanup(struct cli_subagents_feed **feed)
{
	cli_subagents_feed_destroy(*feed);
}

static struct cli_subagents_feed *cli_subagents_feed_steal(struct cli_subagents_feed **feed)
{
	struct cli_subagents_feed *owned = *feed;
	*feed = NULL;
	return owned;
}

static void cli_subagents_feeds_clear(struct cli_subagents_page *page)
{
	struct cli_subagents_feed **feed;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&page->feeds) &&
			     (feed = (struct cli_subagents_feed **)nb_vec_at(&page->feeds, _i_));
	     _i_++)
		cli_subagents_feed_destroy(*feed);
	nb_vec_clear(&page->feeds);
	page->feed = NULL;
}

static const char *cli_subagents_label(const char *id)
{
	return !strncmp(id, "/root/", 6) ? id + 6 : id;
}

static const char *cli_subagents_activity(const struct agent_info *agent)
{
	if (agent->status == AGENT_RUNNING || agent->status == AGENT_PENDING)
		return agent->activity ? agent->activity : "Starting";
	if (agent->error_code < 0)
		return nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, agent->error_code);
	return agent->status == AGENT_COMPLETED ? "Result available" : "";
}

static const char *cli_subagents_selected(const struct cli_subagents_page *page)
{
	if (!page->selection.index || page->selection.index > (size_t)page->count)
		return "/root";
	return page->items[page->selection.index - 1].id;
}

static struct cli_agents_browser_data cli_subagents_browser_data(struct cli_subagents_page *page)
{
	const struct config *config = runtime_config_borrow(page->ctx->runtime);
	struct cli_agents_browser_data data = {
		.agents = page->items,
		.count = page->count,
		.detail = NULL,
		.project = runtime_workdir_borrow(page->ctx->runtime),
		.model = config ? config->models.text.model : "",
		.root_running = page->ctx->turn_active,
		.now_ms = nb_time_monotonic_ms(),
	};
	return data;
}

static int cli_subagents_refresh(struct cli_subagents_page *page)
{
	struct agent_info *items = NULL;
	int count = 0;
	int64_t session = 0;
	(void)runtime_session_current_id(page->ctx->runtime, &session);
	int rc = page->fullscreen && !page->viewing
			 ? cli_agents_list(page->ctx, session, &items, &count)
			 : cli_agents_activity(page->ctx, session, &items, &count);
	if (rc != 0)
		return rc;
	char *selected NB_CLEANUP(nb_charp_cleanup) = strdup(cli_subagents_selected(page));
	if (!selected) {
		cli_agents_list_free(items, count);
		return NB_ERR_NOMEM;
	}
	if (page->session != session) {
		page->viewing = 0;
		cli_subagents_feeds_clear(page);
		page->session = session;
	}
	cli_agents_list_free(page->items, page->count);
	page->items = items;
	page->count = count;
	page->selection.index = 0;
	for (int i = 0; i < count; i++) {
		if (!strcmp(items[i].id, selected)) {
			page->selection.index = (size_t)i + 1;
			break;
		}
	}
	if (page->fullscreen && !page->viewing) {
		struct cli_agents_browser_data data = cli_subagents_browser_data(page);
		cli_agents_browser_select(&page->browser, &data, page->selection.index);
		size_t selected_index = cli_agents_browser_selected(&page->browser, &data);
		page->selection.index = selected_index == SIZE_MAX ? 0 : selected_index;
	}
	struct cli_subagents_feed *feed = page->feed;
	if (feed && feed->detail) {
		for (int i = 0; i < count; i++) {
			if (!strcmp(items[i].id, feed->thread->id) &&
			    (items[i].status != feed->detail->status ||
			     items[i].started_at_ms != feed->detail->started_at_ms)) {
				struct agent_info *detail = NULL;
				if (cli_agents_read(page->ctx, feed->thread->id,
						       &detail) == 0) {
					cli_agents_list_free(feed->detail, 1);
					feed->detail = detail;
					feed->dirty = 1;
				}
				break;
			}
		}
	}
	page->refreshed = nb_time_monotonic_ms();
	return 0;
}

static int cli_subagents_feed_prepare(struct cli_subagents_page *page, const char *id)
{
	struct cli_subagents_feed **existing;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&page->feeds) &&
	     (existing = (struct cli_subagents_feed **)nb_vec_at(&page->feeds, _i_));
	     _i_++) {
		if (!strcmp((*existing)->thread->id, id)) {
			page->feed = *existing;
			return 0;
		}
	}
	struct cli_subagents_feed *feed NB_CLEANUP(cli_subagents_feed_cleanup) =
		calloc(1, sizeof(*feed));
	if (!feed)
		return NB_ERR_NOMEM;
	int rc = cli_subagents_thread_create(id, &feed->thread);
	if (rc == 0)
		rc = (nb_buf_init(&feed->document),
		      nb_buf_reserve(&feed->document, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_init(&feed->draft),
		      nb_buf_reserve(&feed->draft, 256) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_init(&feed->message),
		      nb_buf_reserve(&feed->message, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	struct cli_subagents_feed **slot = nb_vec_push(&page->feeds);
	if (!slot)
		return NB_ERR_NOMEM;
	feed->follow = 1;
	*slot = cli_subagents_feed_steal(&feed);
	page->feed = *slot;
	return 0;
}

struct cli_subagents_events {
	char **items;
	int count;
};

static void cli_subagents_events_cleanup(struct cli_subagents_events *events)
{
	cli_agents_events_free(events->items, events->count);
}

static int cli_subagents_select(struct cli_subagents_page *page)
{
	const char *id = cli_subagents_selected(page);
	(void)cli_agents_select(page->ctx, NULL);
	page->viewing = 0;
	if (!strcmp(id, "/root"))
		return 0;
	int rc = cli_subagents_feed_prepare(page, id);
	if (rc != 0)
		return rc;
	struct cli_subagents_feed *feed = page->feed;
	struct agent_info *detail NB_CLEANUP(cli_subagents_detail_cleanup) = NULL;
	rc = cli_agents_read(page->ctx, id, &detail);
	if (rc != 0)
		return rc;
	cli_agents_list_free(feed->detail, feed->detail ? 1 : 0);
	feed->detail = cli_subagents_detail_steal(&detail);
	struct cli_subagents_events events NB_CLEANUP(cli_subagents_events_cleanup) = {0};
	int64_t sequence = 0;
	rc = cli_agents_subscribe(page->ctx, id, &events.items, &events.count,
				     &sequence);
	for (int i = 0; i < events.count && rc == 0; i++)
		rc = cli_subagents_thread_replay(feed->thread, events.items[i]);
	if (rc != 0) {
		(void)cli_agents_select(page->ctx, NULL);
		return rc;
	}
	feed->thread->sequence = sequence;
	feed->editing = 0;
	feed->dirty = 1;
	feed->rendered = 0;
	morph_tui_frame_invalidate(&feed->frame);
	page->viewing = 1;
	return 0;
}

static int cli_subagents_document(struct cli_subagents_page *page)
{
	struct cli_subagents_feed *feed = page->feed;
	if (!page->viewing || !feed || !feed->thread || !feed->detail)
		return 0;
	nb_buf_clear(&feed->document);
	int rc = cli_subagents_transcript(page->ctx, feed->thread, &feed->document, feed->columns,
					  feed->details);
	struct cli_message_style style = {
		.columns = feed->columns,
		.styled = cli_output_color_enabled(page->ctx),
		.ultra = page->ctx->composer.ultra,
	};
	cli_output_track_tail(&style.trailing_newlines, nb_buf_cstr(&feed->document),
			      feed->document.len);
	if (rc == 0 && !feed->thread->segments.len)
		rc = cli_messages_append(&feed->document, CLI_MESSAGE_USER,
					 feed->detail->description ? feed->detail->description : "",
					 &style);
	if (rc == 0 && !feed->thread->has_content && feed->detail->result)
		rc = cli_messages_append(&feed->document, CLI_MESSAGE_ASSISTANT,
					 feed->detail->result, &style);
	if (rc != 0)
		return rc;
	if (!((void *)feed->lines.lines.data)) {
		struct morph_tui_widget_text_line line;
		rc = (nb_vec_init(&feed->lines.lines, sizeof(line)),
		      nb_vec_reserve(&feed->lines.lines, 32) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	nb_vec_clear(&feed->lines.lines);
	feed->lines.text = nb_buf_cstr(&feed->document);
	const char *start = feed->lines.text;
	while (*start) {
		const char *end = strchr(start, '\n');
		if (!end)
			end = start + strlen(start);
		struct morph_tui_widget_text_line *line = nb_vec_push(&feed->lines.lines);
		if (!line)
			return NB_ERR_NOMEM;
		*line = (struct morph_tui_widget_text_line){
			.offset = (size_t)(start - feed->lines.text),
			.length = (size_t)(end - start),
		};
		start = *end ? end + 1 : end;
	}
	feed->dirty = 0;
	feed->rendered = nb_time_monotonic_ms();
	return rc;
}

static int cli_subagents_picker_row(struct cli_subagents_page *page, size_t index, nb_buf *line)
{
	const char *marker = page->selection.index == index ? ">" : " ";
	if (!index)
		return (nb_buf_printf(line, "%s /root  Main conversation", marker) ? 0
										   : NB_ERR_NOMEM);
	if (index > (size_t)page->count)
		return NB_ERR_INVAL;
	const struct agent_info *agent = &page->items[index - 1];
	return (nb_buf_printf(line, "%s %s  · %s  · %s", marker, agent->id,
			      cli_agents_status(agent->status), cli_subagents_activity(agent))
			? 0
			: NB_ERR_NOMEM);
}

static int cli_subagents_picker_draw(void *data, struct cli_composer_panel_frame *frame)
{
	struct cli_subagents_page *page = data;
	int rows = page->count < frame->rows - 3 ? page->count + 3 : frame->rows;
	int height = rows > 2 ? rows - 2 : 1;
	morph_tui_widget_list_state_layout(&page->selection, (size_t)page->count + 1,
					   (size_t)height);
	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&line), nb_buf_reserve(&line, 256) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && rows > 1)
		rc = cli_composer_panel_line(frame, "  Subagents", ANSI_BOLD CLI_COMMAND_COLOR);
	for (size_t row = 0; rc == 0 && row < page->selection.page; row++) {
		size_t index = page->selection.first + row;
		if (index > (size_t)page->count)
			break;
		nb_buf_clear(&line);
		rc = cli_subagents_picker_row(page, index, &line);
		if (rc == 0)
			rc = cli_composer_panel_line(frame, nb_buf_cstr(&line),
						     index == page->selection.index ? "\033[44;97m"
										    : "");
	}
	if (rc == 0 && rows > 2)
		rc = cli_composer_panel_line(frame, "  ↑/↓ select · Enter view · Esc cancel",
					     ANSI_DIM);
	return rc;
}

static void cli_subagents_frame_cleanup(struct cli_composer_panel_frame *frame)
{
	nb_buf_cleanup(&frame->content);
}

static int cli_subagents_fullscreen_draw(struct cli_subagents_page *page,
					 const struct morph_tui_screen *screen)
{
	struct cli_agents_browser_data data = cli_subagents_browser_data(page);
	size_t selected = cli_agents_browser_selected(&page->browser, &data);
	page->selection.index = selected == SIZE_MAX ? 0 : selected;
	nb_buf document NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&document), nb_buf_reserve(&document, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = cli_agents_browser_append(&document, &page->browser, &data,
					       screen->area.width > 1 ? screen->area.width - 1 : 1,
					       screen->area.height,
					       cli_output_color_enabled(page->ctx));
	if (rc == 0)
		rc = morph_tui_frame_present(&page->picker_frame, screen->session.output,
					     screen->area.width, screen->area.height,
					     nb_buf_cstr(&document));
	return rc;
}

static int cli_subagents_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_subagents_page *page = data;
	struct cli_subagents_feed *feed = page->feed;
	if (!page->viewing)
		return cli_subagents_fullscreen_draw(page, screen);
	if (!feed)
		return NB_ERR_INVAL;
	struct cli_composer_panel_frame frame NB_CLEANUP(cli_subagents_frame_cleanup) = {
		.columns = screen->area.width > 1 ? screen->area.width - 1 : 0,
		.rows = screen->area.height,
		.color = cli_output_color_enabled(page->ctx),
	};
	nb_buf line NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&frame.content),
		  nb_buf_reserve(&frame.content, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_init(&line), nb_buf_reserve(&line, 256) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	int changed = feed->columns != frame.columns;
	feed->columns = frame.columns;
	feed->rows = frame.rows;
	if (changed)
		feed->dirty = 1;
	int header = feed->rows >= 6 ? 3 : 1;
	int footer = feed->rows >= 4 ? 2 : 1;
	int height = feed->rows - header - footer;
	if (height < 1)
		height = 1;
	if (feed->dirty &&
	    (changed || !feed->rendered || nb_time_monotonic_ms() - feed->rendered >= 200)) {
		rc = cli_subagents_document(page);
		if (rc != 0)
			return rc;
	}
	if (feed->follow)
		feed->scroll = feed->lines.lines.len > (size_t)height
				       ? feed->lines.lines.len - (size_t)height
				       : 0;
	feed->scroll = morph_tui_widget_scroll_update(feed->scroll, feed->lines.lines.len,
						      (size_t)height, 0);
	size_t bottom =
		feed->lines.lines.len > (size_t)height ? feed->lines.lines.len - (size_t)height : 0;
	feed->follow = feed->scroll == bottom;
	/* Line helpers sanitize/clamp renderer output; the child owns the whole frame. */
	for (int row = 0; row < feed->rows && rc == 0; row++) {
		nb_buf_clear(&line);
		const char *style = "";
		if (row == 0) {
			rc = (nb_buf_printf(&line, "  %s · %s",
					    page->fullscreen ? "Agents" : "Subagents",
					    feed->thread->id)
				      ? 0
				      : NB_ERR_NOMEM);
			style = ANSI_BOLD CLI_COMMAND_COLOR;
		} else if (header > 1 && row == 1) {
			const struct agent_info *current =
				page->selection.index &&
						page->selection.index <= (size_t)page->count
					? &page->items[page->selection.index - 1]
					: feed->detail;
			rc = (nb_buf_printf(&line, "  %s · %s%s",
					    cli_agents_status(current->status),
					    cli_subagents_activity(current),
					    feed->details ? " · Tool details" : "")
				      ? 0
				      : NB_ERR_NOMEM);
			style = ANSI_DIM;
		} else if (row >= header && row < height + header && row < feed->rows - footer) {
			struct morph_tui_widget_text_line *text = nb_vec_at(
				&feed->lines.lines, feed->scroll + (size_t)(row - header));
			if (text)
				rc = (nb_buf_append(&line, feed->document.ptr + text->offset,
						    text->length)
					      ? 0
					      : NB_ERR_NOMEM);
			if (rc == 0)
				rc = cli_composer_panel_styled_line(&frame, nb_buf_cstr(&line));
			continue;
		} else if (footer > 1 && row == feed->rows - 2) {
			rc = (nb_buf_printf(
				      &line, feed->editing ? "  Follow up › %s" : "  %s",
				      nb_buf_cstr(feed->editing ? &feed->draft : &feed->message))
				      ? 0
				      : NB_ERR_NOMEM);
		} else if (row == feed->rows - 1) {
			rc = (nb_buf_append_cstr(
				      &line, feed->editing ? "  Enter send · Esc cancel"
							   : "  f follow · k stop · d details · "
							     "Tab switch · Esc main conversation")
				      ? 0
				      : NB_ERR_NOMEM);
			style = ANSI_DIM;
		}
		if (rc == 0)
			rc = cli_composer_panel_line(&frame, nb_buf_cstr(&line), style);
	}
	if (rc == 0)
		rc = morph_tui_frame_present(&feed->frame, screen->session.output,
					     screen->area.width, screen->area.height,
					     nb_buf_cstr(&frame.content));
	return rc;
}

static int cli_subagents_picker_update(void *data, const struct morph_tui_event *event)
{
	struct cli_subagents_page *page = data;
	if (event->type == MORPH_TUI_EVENT_TICK || event->type == MORPH_TUI_EVENT_WAKE) {
		if (cli_event_queue_owner_call_pending(page->ctx))
			return MORPH_TUI_SCREEN_CLOSE;
		if (nb_time_monotonic_ms() - page->refreshed >= 500) {
			int rc = cli_subagents_refresh(page);
			return rc != 0 ? rc : MORPH_TUI_SCREEN_REDRAW;
		}
		return MORPH_TUI_SCREEN_CONTINUE;
	}
	if (event->type == MORPH_TUI_EVENT_RESIZE)
		return MORPH_TUI_SCREEN_REDRAW;
	if (event->type != MORPH_TUI_EVENT_KEY || event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	int key = event->key;
	if (page->fullscreen) {
		struct cli_agents_browser_data snapshot = cli_subagents_browser_data(page);
		int action = cli_agents_browser_update(&page->browser, &snapshot, key);
		size_t selected = cli_agents_browser_selected(&page->browser, &snapshot);
		page->selection.index = selected == SIZE_MAX ? 0 : selected;
		if (action == MORPH_TUI_WIDGET_CANCEL)
			return MORPH_TUI_SCREEN_CLOSE;
		if (action == MORPH_TUI_WIDGET_SUBMIT) {
			int rc = cli_subagents_select(page);
			return rc != 0 ? rc : MORPH_TUI_SCREEN_CLOSE;
		}
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (key == 27 || key == 3 || key == 4 || key == 'q')
		return MORPH_TUI_SCREEN_CLOSE;
	if (key == '\r' || key == '\n') {
		int rc = cli_subagents_select(page);
		return rc != 0 ? rc : MORPH_TUI_SCREEN_CLOSE;
	}
	if (key == '\t' || key == MORPH_TUI_KEY_RIGHT || key == MORPH_TUI_KEY_LEFT ||
	    key == MORPH_TUI_KEY_BACK_TAB) {
		size_t count = (size_t)page->count + 1;
		int backward = key == MORPH_TUI_KEY_LEFT || key == MORPH_TUI_KEY_BACK_TAB;
		page->selection.index = backward ? (page->selection.index + count - 1) % count
						 : (page->selection.index + 1) % count;
	} else
		(void)morph_tui_widget_list_state_update(&page->selection, key);
	return MORPH_TUI_SCREEN_REDRAW;
}

static int cli_subagents_update(void *data, const struct morph_tui_event *event)
{
	struct cli_subagents_page *page = data;
	if (!page->viewing) {
		int result = cli_subagents_picker_update(data, event);
		return result == MORPH_TUI_SCREEN_CLOSE && page->viewing ? MORPH_TUI_SCREEN_REDRAW
									 : result;
	}
	struct cli_subagents_feed *feed = page->feed;
	if (event->type == MORPH_TUI_EVENT_TICK || event->type == MORPH_TUI_EVENT_WAKE) {
		if (cli_event_queue_owner_call_pending(page->ctx))
			return MORPH_TUI_SCREEN_CLOSE;
		if (nb_time_monotonic_ms() - page->refreshed >= 500) {
			int rc = cli_subagents_refresh(page);
			if (rc != 0)
				return rc;
			if (!page->viewing)
				return MORPH_TUI_SCREEN_CLOSE;
			return MORPH_TUI_SCREEN_REDRAW;
		}
		return feed->dirty ? MORPH_TUI_SCREEN_REDRAW : MORPH_TUI_SCREEN_CONTINUE;
	}
	if (event->type == MORPH_TUI_EVENT_RESIZE)
		return MORPH_TUI_SCREEN_REDRAW;
	if (event->type != MORPH_TUI_EVENT_KEY)
		return MORPH_TUI_SCREEN_CONTINUE;
	int key = event->key;
	if (feed->editing) {
		if (key == MORPH_TUI_KEY_PASTE_BEGIN || key == MORPH_TUI_KEY_PASTE_END)
			return MORPH_TUI_SCREEN_REDRAW;
		if (event->pasting) {
			if (key > 0 && key <= 255 && feed->draft.len < 65532)
				(void)nb_buf_append_char(&feed->draft,
							 key == '\r' ? '\n' : (char)key);
		} else if (key == 27 || key == 3)
			feed->editing = 0;
		else if (key == '\r' || key == '\n') {
			int rc = nb_utf8_validate(nb_str_from_cstr(nb_buf_cstr(&feed->draft)), NULL)
					 ? NB_ERR_INVAL
					 : runtime_agent_followup(page->ctx->runtime,
								  feed->thread->id,
								  nb_buf_cstr(&feed->draft));
			nb_buf_clear(&feed->message);
			(void)nb_buf_append_cstr(
				&feed->message,
				rc ? nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc)
				   : "Follow-up sent");
			if (rc == 0) {
				feed->editing = 0;
				nb_buf_clear(&feed->draft);
			}
		} else if ((key == 127 || key == 8) && feed->draft.len)
			(void)nb_buf_truncate(
				&feed->draft,
				nb_utf8_clamp_bytes(feed->draft.ptr, feed->draft.len - 1));
		else if (key >= 32 && key <= 255 && feed->draft.len < 65532)
			(void)nb_buf_append_char(&feed->draft, (char)key);
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	if (key == 27 || key == 3 || key == 4 || key == 'q') {
		return MORPH_TUI_SCREEN_CLOSE;
	} else if (key == '\t' || key == MORPH_TUI_KEY_RIGHT || key == MORPH_TUI_KEY_LEFT ||
		   key == MORPH_TUI_KEY_BACK_TAB) {
		size_t count = (size_t)page->count + 1;
		int backward = key == MORPH_TUI_KEY_LEFT || key == MORPH_TUI_KEY_BACK_TAB;
		page->selection.index = backward ? (page->selection.index + count - 1) % count
						 : (page->selection.index + 1) % count;
		int rc = cli_subagents_select(page);
		if (rc != 0)
			return rc;
		if (!page->viewing)
			return MORPH_TUI_SCREEN_CLOSE;
	} else if (key == 'd' || key == 20) {
		feed->details = !feed->details;
		feed->dirty = 1;
		feed->rendered = 0;
		feed->follow = 1;
	} else if (key == 'f' || key == 'k') {
		if (key == 'f') {
			feed->editing = 1;
		} else {
			int rc = cli_agents_interrupt(page->ctx, feed->thread->id);
			nb_buf_clear(&feed->message);
			(void)nb_buf_append_cstr(
				&feed->message,
				rc ? nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc)
				   : "Interrupt requested");
		}
	} else {
		size_t delta = key == MORPH_TUI_KEY_PAGE_UP || key == MORPH_TUI_KEY_PAGE_DOWN
				       ? (size_t)(feed->rows > 5 ? feed->rows - 5 : 1)
				       : 1;
		if (key == MORPH_TUI_KEY_UP || key == MORPH_TUI_KEY_PAGE_UP) {
			feed->scroll = delta > feed->scroll ? 0 : feed->scroll - delta;
			feed->follow = 0;
		} else if (key == MORPH_TUI_KEY_DOWN || key == MORPH_TUI_KEY_PAGE_DOWN) {
			feed->scroll += delta;
			feed->follow = 0;
		} else if (key == MORPH_TUI_KEY_HOME) {
			feed->scroll = 0;
			feed->follow = 0;
		} else if (key == MORPH_TUI_KEY_END)
			feed->follow = 1;
	}
	return MORPH_TUI_SCREEN_REDRAW;
}

static void cli_subagents_complete(void *data, int result)
{
	struct cli_subagents_page *page = data;
	(void)result;
	page->open = 0;
	page->viewing = 0;
	if (page->feed)
		page->feed->editing = 0;
	(void)cli_agents_select(page->ctx, NULL);
}

static int cli_subagents_view_show(struct cli_subagents_page *page)
{
	static const struct morph_tui_screen_ops ops = {
		.update = cli_subagents_update,
		.draw = cli_subagents_draw,
	};
	return cli_application_present_screen(page->ctx, &ops, page, cli_subagents_complete);
}

static void cli_subagents_picker_complete(void *data, int result)
{
	struct cli_subagents_page *page = data;
	if (result == 0 && page->viewing) {
		int rc = cli_subagents_view_show(page);
		if (rc == 0)
			return;
		cli_subagents_complete(page, rc);
		return;
	}
	cli_subagents_complete(page, result);
}

void cli_subagents_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->subagents)
		return;
	struct cli_subagents_page *page = ctx->subagents;
	cli_agents_list_free(page->items, page->count);
	cli_subagents_feeds_clear(page);
	morph_tui_frame_cleanup(&page->picker_frame);
	nb_vec_cleanup(&page->feeds);
	free(page);
	ctx->subagents = NULL;
}

int cli_subagents_is_open(const struct cli_application *ctx)
{
	return ctx && ctx->subagents && ctx->subagents->open;
}

int cli_subagents_event(struct cli_application *ctx, const struct morph_event *event)
{
	const cJSON *sequence =
		cJSON_GetObjectItemCaseSensitive(event->data, "agent_event_sequence");
	if (!cJSON_IsNumber(sequence))
		return 0;
	/* Raw child events never enter the primary transcript, including queued
	 * callbacks left after closing or switching the subscription. */
	if (ctx && ctx->subagents) {
		const char *id = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(event->data, "agent_id"));
		struct cli_subagents_feed **feed;
		for (size_t _i_ = 0;
		     _i_ < nb_vec_len(&ctx->subagents->feeds) &&
		     (feed = (struct cli_subagents_feed **)nb_vec_at(&ctx->subagents->feeds, _i_));
		     _i_++) {
			if (!id || strcmp(id, (*feed)->thread->id))
				continue;
			int rc = cli_subagents_thread_event((*feed)->thread, event);
			if (rc != 0)
				return rc;
			(*feed)->dirty = 1;
			if (cli_subagents_is_open(ctx) && *feed == ctx->subagents->feed)
				morph_tui_application_invalidate(cli_application_tui_borrow(ctx),
								 MORPH_TUI_INVALIDATE_DISPLAY);
			break;
		}
	}
	return 1;
}

static int cli_subagents_show_mode(struct cli_application *ctx, const char *id, int fullscreen)
{
	if (!ctx || !ctx->runtime || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return NB_ERR_UNSUPPORTED;
	if (!ctx->subagents) {
		ctx->subagents = calloc(1, sizeof(*ctx->subagents));
		if (!ctx->subagents)
			return NB_ERR_NOMEM;
		ctx->subagents->ctx = ctx;
		struct cli_subagents_feed *feed = NULL;
		int rc = (nb_vec_init(&ctx->subagents->feeds, sizeof(feed)),
			  nb_vec_reserve(&ctx->subagents->feeds, 4) ? 0 : NB_ERR_NOMEM);
		if (rc != 0) {
			cli_subagents_cleanup(ctx);
			return rc;
		}
	}
	struct cli_subagents_page *page = ctx->subagents;
	if (page->open)
		return NB_ERR_BUSY;
	page->viewing = 0;
	page->fullscreen = fullscreen;
	memset(&page->browser, 0, sizeof(page->browser));
	int rc = cli_subagents_refresh(page);
	if (rc != 0)
		return rc;
	morph_tui_frame_invalidate(&page->picker_frame);
	page->selection.index = 0;
	if (id && strcmp(id, "/root")) {
		int found = 0;
		for (int i = 0; i < page->count; i++) {
			if (!strcmp(id, page->items[i].id) ||
			    !strcmp(id, cli_subagents_label(page->items[i].id))) {
				page->selection.index = (size_t)i + 1;
				found = 1;
				break;
			}
		}
		if (!found)
			return NB_ERR_NOT_FOUND;
		rc = cli_subagents_select(page);
	}
	if (rc != 0)
		return rc;
	if (page->viewing || fullscreen)
		rc = cli_subagents_view_show(page);
	else {
		static const struct cli_composer_panel_ops ops = {
			.update = cli_subagents_picker_update,
			.draw = cli_subagents_picker_draw,
			.complete = cli_subagents_picker_complete,
		};
		const struct cli_composer_panel_options options = {
			.max_rows = 12,
			.tick_ms = 100,
		};
		rc = cli_composer_panel_show(ctx, &options, &ops, page);
	}
	if (rc == 0)
		page->open = 1;
	else
		cli_subagents_complete(page, rc);
	return rc;
}

int cli_subagents_show(struct cli_application *ctx, const char *id)
{
	return cli_subagents_show_mode(ctx, id, 0);
}

int cli_subagents_show_fullscreen(struct cli_application *ctx, const char *id)
{
	return cli_subagents_show_mode(ctx, id, 1);
}
