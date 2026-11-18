#include "sapi/cli/page/subagents.h"
#include <nb/time.h>
#include <nb/err.h>

#include "sapi/cli/application.h"
#include "config/config.h"
#include "runtime/runtime.h"
#include "sapi/cli/models/subagents.h"
#include "sapi/cli/presentation/messages.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/component/agents.h"
#include "sapi/cli/component/agents_browser.h"
#include "sapi/cli/page/chat.h"
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/screen.h>
#include "sapi/cli/component/transcript.h"
#include "sapi/cli/component/subagents.h"
#include <nb/cleanup.h>
#include <unistd.h>
#include <nothing/tui/terminal.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/scroll_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/controls/text_field.h>
#include <nothing/tui/input.h>
#include <nothing/tui/views/text_view.h>
#include <nb/utf8.h>
#include <stdlib.h>
#include <string.h>
#include "sapi/cli/services/agents.h"

struct cli_subagents_feed {
	struct agent_info *detail;
	struct cli_subagents_thread *thread;
	nb_buf document;
	nb_buf draft;
	nb_buf message;
	int scroll;
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
	struct nt_list_state selection;
	struct agent_info *items;
	int count;
	nb_vec feeds;
	struct cli_subagents_feed *feed;
	struct nt_view_controller *controller;
	struct nt_view *root, *browser_view, *reader, *title, *status, *gap;
	struct nt_view *scroll, *text, *message, *edit_row, *editor, *help;
	int columns, rows;
	struct nt_view *picker_title, *picker_list, *picker_hint;
	nb_buf picker_row;
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
				if (cli_agents_read(page->ctx, feed->thread->id, &detail) == 0) {
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
	rc = cli_agents_subscribe(page->ctx, id, &events.items, &events.count, &sequence);
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
	if (feed->document.len && feed->document.ptr[feed->document.len - 1] == '\n')
		nb_buf_truncate(&feed->document, feed->document.len - 1);
	rc = cli_messages_set_rendered_view(page->text, nb_buf_cstr(&feed->document), style.styled);
	feed->dirty = 0;
	feed->rendered = nb_time_monotonic_ms();
	return rc;
}

static const char *cli_subagents_picker_row(void *data, size_t index)
{
	struct cli_subagents_page *page = data;
	const char *marker = page->selection.index == index ? ">" : " ";
	nb_buf_clear(&page->picker_row);
	if (!index)
		nb_buf_printf(&page->picker_row, "%s /root  Main conversation", marker);
	else if (index <= (size_t)page->count) {
		const struct agent_info *agent = &page->items[index - 1];
		nb_buf_printf(&page->picker_row, "%s %s  · %s  · %s", marker, agent->id,
			      cli_agents_status(agent->status), cli_subagents_activity(agent));
	}
	return nb_buf_cstr(&page->picker_row);
}

static void cli_subagents_picker_style(void *data, size_t index, int selected,
				       struct nt_text_style *style)
{
	(void)index;
	struct cli_subagents_page *page = data;
	*style = selected && cli_output_color_enabled(page->ctx)
			 ? (struct nt_text_style){.foreground = {NT_COLOR_INDEXED, 15},
						  .background = {NT_COLOR_INDEXED, 4}}
			 : (struct nt_text_style){0};
}

static int cli_subagents_sync(struct cli_subagents_page *page)
{
	struct cli_subagents_feed *feed = page->feed;
	int rc = nt_view_set_hidden(page->browser_view, page->viewing);
	if (!rc)
		rc = nt_view_set_hidden(page->reader, !page->viewing);
	if (rc)
		return rc;
	if (!page->viewing) {
		struct cli_agents_browser_data data = cli_subagents_browser_data(page);
		return cli_agents_browser_sync(page->browser_view, &page->browser, &data,
					       cli_output_color_enabled(page->ctx));
	}
	int changed = feed->columns != page->columns;
	feed->columns = page->columns;
	feed->rows = page->rows;
	if (changed ||
	    (feed->dirty && (!feed->rendered || nb_time_monotonic_ms() - feed->rendered >= 200)))
		rc = cli_subagents_document(page);
	nb_buf label NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!rc && !nb_buf_printf(&label, "  %s · %s", page->fullscreen ? "Agents" : "Subagents",
				  feed->thread->id))
		rc = NB_ERR_NOMEM;
	if (!rc)
		rc = nt_label_set_text(page->title, nb_buf_cstr(&label));
	nb_buf_clear(&label);
	const struct agent_info *agent =
		page->selection.index && page->selection.index <= (size_t)page->count
			? &page->items[page->selection.index - 1]
			: feed->detail;
	if (!rc &&
	    !nb_buf_printf(&label, "  %s · %s%s", cli_agents_status(agent->status),
			   cli_subagents_activity(agent), feed->details ? " · Tool details" : ""))
		rc = NB_ERR_NOMEM;
	if (!rc)
		rc = nt_label_set_text(page->status, nb_buf_cstr(&label));
	if (!rc)
		rc = nt_view_set_hidden(page->status, page->rows < 6);
	if (!rc)
		rc = nt_view_set_hidden(page->gap, page->rows < 6);
	if (!rc)
		rc = nt_view_set_hidden(page->message, page->rows < 4 || feed->editing);
	if (!rc)
		rc = nt_view_set_hidden(page->edit_row, !feed->editing);
	if (!rc)
		rc = nt_label_set_text(page->message, nb_buf_cstr(&feed->message));
	if (!rc)
		rc = nt_label_set_text(page->help, feed->editing
							   ? "  Enter send · Esc cancel"
							   : "  f follow · k stop · d details · "
							     "Tab switch · Esc main conversation");
	if (!rc)
		rc = nt_scroll_view_follow(page->scroll, feed->follow);
	if (!rc && !feed->follow)
		rc = nt_scroll_view_set_offset(page->scroll, feed->scroll);
	if (rc >= 0 && feed->editing)
		rc = nt_view_focus(page->editor);
	return rc < 0 ? rc : 0;
}

static int cli_subagents_picker_update(void *data, const struct nt_event *event)
{
	struct cli_subagents_page *page = data;
	if (event->type == NT_EVENT_TICK || event->type == NT_EVENT_WAKE) {
		if (cli_event_queue_owner_call_pending(page->ctx))
			return NT_SCREEN_CLOSE;
		if (nb_time_monotonic_ms() - page->refreshed >= 500) {
			int rc = cli_subagents_refresh(page);
			return rc != 0 ? rc : NT_SCREEN_REDRAW;
		}
		return NT_SCREEN_CONTINUE;
	}
	if (event->type == NT_EVENT_RESIZE)
		return NT_SCREEN_REDRAW;
	if (event->type != NT_EVENT_KEY || event->pasting)
		return NT_SCREEN_CONTINUE;
	int key = event->key;
	if (page->fullscreen) {
		struct cli_agents_browser_data snapshot = cli_subagents_browser_data(page);
		int action = cli_agents_browser_update(&page->browser, &snapshot, key);
		size_t selected = cli_agents_browser_selected(&page->browser, &snapshot);
		page->selection.index = selected == SIZE_MAX ? 0 : selected;
		if (action == NT_NAVIGATION_CANCEL)
			return NT_SCREEN_CLOSE;
		if (action == NT_NAVIGATION_SUBMIT) {
			int rc = cli_subagents_select(page);
			return rc != 0 ? rc : NT_SCREEN_CLOSE;
		}
		return NT_SCREEN_REDRAW;
	}
	if (key == 27 || key == 3 || key == 4 || key == 'q')
		return NT_SCREEN_CLOSE;
	if (key == '\r' || key == '\n') {
		int rc = cli_subagents_select(page);
		return rc != 0 ? rc : NT_SCREEN_CLOSE;
	}
	if (key == '\t' || key == NT_KEY_RIGHT || key == NT_KEY_LEFT || key == NT_KEY_BACK_TAB) {
		size_t count = (size_t)page->count + 1;
		int backward = key == NT_KEY_LEFT || key == NT_KEY_BACK_TAB;
		page->selection.index = backward ? (page->selection.index + count - 1) % count
						 : (page->selection.index + 1) % count;
	} else
		(void)nt_list_state_update(&page->selection, key);
	return NT_SCREEN_REDRAW;
}

static int cli_subagents_update(void *data, const struct nt_event *event)
{
	struct cli_subagents_page *page = data;
	if (!page->viewing) {
		int result = cli_subagents_picker_update(data, event);
		return result == NT_SCREEN_CLOSE && page->viewing ? NT_SCREEN_REDRAW : result;
	}
	struct cli_subagents_feed *feed = page->feed;
	if (event->type == NT_EVENT_TICK || event->type == NT_EVENT_WAKE) {
		if (cli_event_queue_owner_call_pending(page->ctx))
			return NT_SCREEN_CLOSE;
		if (nb_time_monotonic_ms() - page->refreshed >= 500) {
			int rc = cli_subagents_refresh(page);
			if (rc != 0)
				return rc;
			if (!page->viewing)
				return NT_SCREEN_CLOSE;
			return NT_SCREEN_REDRAW;
		}
		return feed->dirty ? NT_SCREEN_REDRAW : NT_SCREEN_CONTINUE;
	}
	if (event->type == NT_EVENT_RESIZE)
		return NT_SCREEN_REDRAW;
	if (event->type != NT_EVENT_KEY)
		return NT_SCREEN_CONTINUE;
	int key = event->key;
	if (feed->editing) {
		if (key != 27 && key != 3)
			return NT_SCREEN_CONTINUE;
		nb_buf_clear(&feed->draft);
		if (!nb_buf_append_cstr(&feed->draft, nt_text_field_text(page->editor)))
			return NB_ERR_NOMEM;
		feed->editing = 0;
		return NT_SCREEN_REDRAW;
	}
	if (event->pasting)
		return NT_SCREEN_CONTINUE;
	if (key == 27 || key == 3 || key == 4 || key == 'q') {
		return NT_SCREEN_CLOSE;
	} else if (key == '\t' || key == NT_KEY_RIGHT || key == NT_KEY_LEFT ||
		   key == NT_KEY_BACK_TAB) {
		size_t count = (size_t)page->count + 1;
		int backward = key == NT_KEY_LEFT || key == NT_KEY_BACK_TAB;
		page->selection.index = backward ? (page->selection.index + count - 1) % count
						 : (page->selection.index + 1) % count;
		int rc = cli_subagents_select(page);
		if (rc != 0)
			return rc;
		if (!page->viewing)
			return NT_SCREEN_CLOSE;
	} else if (key == 'd' || key == 20) {
		feed->details = !feed->details;
		feed->dirty = 1;
		feed->rendered = 0;
		feed->follow = 1;
	} else if (key == 'f' || key == 'k') {
		if (key == 'f') {
			int rc = nt_text_field_set_text(page->editor, nb_buf_cstr(&feed->draft));
			if (rc)
				return rc;
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
		int rc = nt_scroll_view_handle_key(page->scroll, key);
		if (rc < 0)
			return rc;
		const struct nt_scroll_state *scroll = nt_scroll_view_state(page->scroll);
		feed->scroll = scroll->offset;
		feed->follow = scroll->offset == nt_scroll_max(scroll);
	}
	return NT_SCREEN_REDRAW;
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

static int cli_subagents_submit(void *data, const struct nt_view_event *event)
{
	struct cli_subagents_page *page = data;
	struct cli_subagents_feed *feed = page->feed;
	(void)event;
	int rc = runtime_agent_followup(page->ctx->runtime, feed->thread->id,
					nt_text_field_text(page->editor));
	nb_buf_clear(&feed->message);
	if (!nb_buf_append_cstr(&feed->message,
				rc ? nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc)
				   : "Follow-up sent"))
		return NB_ERR_NOMEM;
	if (!rc) {
		feed->editing = 0;
		nb_buf_clear(&feed->draft);
	}
	return cli_subagents_sync(page);
}

static int cli_subagents_handle(struct cli_subagents_page *page, const struct nt_event *event)
{
	int action = cli_subagents_update(page, event);
	if (action == NT_SCREEN_CLOSE) {
		int rc = nt_view_controller_dismiss(page->controller, 0);
		return rc < 0 ? rc : 1;
	}
	if (action == NT_SCREEN_REDRAW) {
		int rc = cli_subagents_sync(page);
		return rc ? rc : 1;
	}
	return action;
}

static int cli_subagents_input(void *data, const struct nt_view_event *event)
{
	struct cli_subagents_page *page = data;
	if (event->input->action == NT_KEY_ACTION_RELEASE)
		return 0;
	int key = event->input->key;
	if (page->viewing && page->feed->editing && !event->input->pasting &&
	    (key == '\r' || key == '\n')) {
		int rc = cli_subagents_submit(page, event);
		return rc < 0 ? rc : NT_VIEW_EVENT_STOP;
	}
	int rc = cli_subagents_handle(page, event->input);
	return rc > 0 ? NT_VIEW_EVENT_STOP : rc;
}

static int cli_subagents_size(void *data, const struct nt_view_event *event)
{
	struct cli_subagents_page *page = data;
	if (event->target != page->root)
		return 0;
	page->columns = event->rect.width > 1 ? event->rect.width - 1 : 1;
	page->rows = event->rect.height;
	return cli_subagents_sync(page);
}

static int cli_subagents_page_event(struct nt_view_controller *controller,
				    const struct nt_event *event)
{
	if (event->type == NT_EVENT_TICK || event->type == NT_EVENT_WAKE)
		return cli_subagents_handle(controller->data, event);
	return 0;
}

static void cli_subagents_build(struct nt_view_controller *controller, struct nt_builder *builder)
{
	struct cli_subagents_page *page = controller->data;
	page->controller = controller;
	const struct nt_view_options fill = {.grow = 1};
	const struct nt_view_options row = {.height = 1};
	const struct nt_view_options dim = {.height = 1, .style = {.attributes = NT_STYLE_DIM}};
	page->root =
		nt_column_add(builder, NULL, 0, &(struct nt_view_options){.padding = {.right = 1}});
	page->browser_view = cli_agents_browser_add(builder, page->root);
	page->reader = nt_column_add(builder, page->root, 0, &fill);
	page->title = nt_label_add(
		builder, page->reader, "",
		&(struct nt_view_options){.height = 1,
					  .style = {.attributes = NT_STYLE_BOLD,
						    .foreground = {NT_COLOR_INDEXED, 141}}});
	page->status = nt_label_add(builder, page->reader, "", &dim);
	page->gap = nt_label_add(builder, page->reader, "", &row);
	page->scroll = nt_scroll_view_add(builder, page->reader, &fill);
	page->text = nt_text_view_add(builder, page->scroll, "", NULL);
	page->message =
		nt_label_add(builder, page->reader, "",
			     &(struct nt_view_options){.height = 1, .padding = {.left = 2}});
	page->edit_row = nt_row_add(builder, page->reader, 0, &row);
	nt_label_add(builder, page->edit_row, "  Follow up › ", &row);
	page->editor = nt_text_field_add(builder, page->edit_row,
					 &(struct nt_text_field_options){.limit = 65533}, &fill);
	page->help = nt_label_add(builder, page->reader, "", &dim);
	if (nt_builder_error(builder))
		return;
	nt_builder_fail(builder, nt_view_set_options(page->browser_view, &fill));
	nt_builder_fail(builder, nt_view_set_focusable(page->root, 1));
	nt_builder_fail(builder, nt_text_view_set_wrap_mode(page->text, NT_TEXT_WRAP_NONE));
	nt_builder_fail(builder, nt_scroll_view_set_scrollbars(page->scroll, 0));
	nt_builder_fail(builder, nt_text_field_set_multiline(page->editor, 1));
	if (!nt_view_on_full(page->root, NT_VIEW_EVENT_KEY,
			     &(struct nt_view_listener_options){
				     .flags = NT_VIEW_LISTEN_CAPTURE,
				     NT_VIEW_LISTENER(cli_subagents_input, page)}) ||
	    !nt_view_on(page->root, NT_VIEW_EVENT_SIZE_CHANGED, cli_subagents_size, page) ||
	    !nt_view_on(page->editor, NT_VIEW_EVENT_SUBMIT, cli_subagents_submit, page))
		nt_builder_fail(builder, NB_ERR_NOMEM);
	nt_builder_fail(builder, cli_subagents_sync(page));
}

static void cli_subagents_destroy(struct nt_view_controller *controller)
{
	struct cli_subagents_page *page = controller->data;
	if (page) {
		page->controller = NULL;
		cli_application_panel_complete(page->ctx, page, cli_subagents_complete,
					       nt_view_controller_result(controller));
	}
}

static void cli_subagents_screen_boundary(struct nt_view_controller *controller)
{
	struct nt_application *application = nt_view_controller_application(controller);
	if (!nt_application_suspend(application))
		(void)nt_application_resume(application);
}

static int cli_subagents_view_show(struct cli_subagents_page *page)
{
	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct nt_view_controller),
		.view_will_appear = cli_subagents_screen_boundary,
		.view_will_disappear = cli_subagents_screen_boundary,
		.load_view = cli_subagents_build,
		.handle_event = cli_subagents_page_event,
		.deinit = cli_subagents_destroy};
	int accepted = 0;
	return cli_application_show_page(page->ctx, &type, page, 100, &accepted);
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

static int cli_subagents_picker_band(struct nt_view *view, const char *text, int width)
{
	nb_buf band NB_CLEANUP(nb_buf_cleanup) = {0};
	int cells = (int)nb_utf8_width_cstr(text);
	if (!nb_buf_printf(&band, "%s%*s", text, width > cells ? width - cells : 0, ""))
		return NB_ERR_NOMEM;
	return nt_label_set_text(view, nb_buf_cstr(&band));
}

static int cli_subagents_picker_sync(struct cli_subagents_page *page)
{
	int columns, rows;
	nt_terminal_dimensions(STDOUT_FILENO, &columns, &rows);
	int budget = rows >= 5 ? rows - 4 : 1;
	int height = page->count < 9 ? page->count + 3 : 12;
	if (height > budget)
		height = budget;
	int list_height = height > 2 ? height - 2 : 1;
	nt_list_state_layout(&page->selection, (size_t)page->count + 1, (size_t)list_height);
	int rc = cli_subagents_picker_band(page->picker_title, "  Subagents", columns - 1);
	if (!rc)
		rc = cli_subagents_picker_band(
			page->picker_hint, "  ↑/↓ select · Enter view · Esc cancel", columns - 1);
	if (!rc)
		rc = nt_view_set_hidden(page->picker_title, height < 2);
	if (!rc)
		rc = nt_view_set_hidden(page->picker_hint, height < 3);
	struct nt_view_options options = nt_view_options(page->picker_list);
	if (!rc && options.height != list_height) {
		options.height = list_height;
		rc = nt_view_set_options(page->picker_list, &options);
	}
	if (!rc)
		rc = nt_list_view_set_source(page->picker_list, (size_t)page->count + 1,
					     cli_subagents_picker_row, page);
	if (!rc)
		rc = nt_list_view_select(page->picker_list, page->selection.index);
	return rc;
}

static int cli_subagents_picker_event(void *data, const struct nt_event *event)
{
	struct cli_subagents_page *page = data;
	int action = cli_subagents_picker_update(page, event);
	if (action == NT_SCREEN_CLOSE)
		return page->controller ? nt_view_controller_dismiss(page->controller, 0)
					: cli_chat_panel_close(page->ctx, 0);
	if (action == NT_SCREEN_REDRAW)
		return cli_subagents_picker_sync(page);
	return action < 0 ? action : 0;
}

static int cli_subagents_picker_input(void *data, const struct nt_view_event *event)
{
	if (event->input->action == NT_KEY_ACTION_RELEASE || event->input->pasting)
		return NT_VIEW_EVENT_STOP;
	int rc = cli_subagents_picker_event(data, event->input);
	return rc < 0 ? rc : NT_VIEW_EVENT_STOP;
}

static struct nt_view *cli_subagents_picker_build(struct nt_builder *builder,
						  struct nt_view *parent, void *data)
{
	struct cli_subagents_page *page = data;
	struct nt_view *root = nt_column_add(builder, parent, 0,
					     &(struct nt_view_options){.padding = {.right = 1}});
	int styled = cli_output_color_enabled(page->ctx);
	page->picker_title = nt_label_add(
		builder, root, "",
		&(struct nt_view_options){
			.height = 1,
			.style = styled ? (struct nt_text_style){.attributes = NT_STYLE_BOLD,
								 .foreground =
									 {NT_COLOR_INDEXED,
									  CLI_INPUT_ULTRA_INDEX}}
					: (struct nt_text_style){0}});
	page->picker_list = nt_list_view_add(builder, root, NULL, 0, 0, NULL);
	page->picker_hint = nt_label_add(
		builder, root, "",
		&(struct nt_view_options){.height = 1,
					  .style = {.attributes = styled ? NT_STYLE_DIM : 0}});
	if (nt_builder_error(builder))
		return NULL;
	nt_builder_fail(builder, nt_label_set_wrap_mode(page->picker_title, NT_TEXT_WRAP_NONE));
	nt_builder_fail(builder, nt_label_set_wrap_mode(page->picker_hint, NT_TEXT_WRAP_NONE));
	nt_builder_fail(builder, nt_view_set_focusable(root, 1));
	nt_builder_fail(builder, nt_list_view_set_row_style(page->picker_list,
							    cli_subagents_picker_style, page));
	if (!nt_view_on_full(root, NT_VIEW_EVENT_KEY,
			     &(struct nt_view_listener_options){
				     .flags = NT_VIEW_LISTEN_CAPTURE,
				     NT_VIEW_LISTENER(cli_subagents_picker_input, page)}))
		nt_builder_fail(builder, NB_ERR_NOMEM);
	nt_builder_fail(builder, cli_subagents_picker_sync(page));
	return root;
}

static void cli_subagents_picker_page_build(struct nt_view_controller *controller,
					    struct nt_builder *builder)
{
	struct cli_subagents_page *page = controller->data;
	page->controller = controller;
	struct nt_view *root = cli_subagents_picker_build(builder, NULL, page);
	if (root)
		nt_builder_fail(builder, nt_view_focus(root));
}

static int cli_subagents_picker_page_event(struct nt_view_controller *controller,
					   const struct nt_event *event)
{
	return event->type == NT_EVENT_KEY ? 0
					   : cli_subagents_picker_event(controller->data, event);
}

static void cli_subagents_picker_destroy(struct nt_view_controller *controller)
{
	struct cli_subagents_page *page = controller->data;
	if (page) {
		page->controller = NULL;
		cli_application_panel_complete(page->ctx, page, cli_subagents_picker_complete,
					       nt_view_controller_result(controller));
	}
}

static int cli_subagents_picker_show(struct cli_subagents_page *page)
{
	if (cli_chat_active(page->ctx)) {
		int rc = cli_chat_panel_mount(page->ctx, cli_subagents_picker_build, page,
					      cli_subagents_picker_complete);
		if (!rc)
			cli_chat_panel_set_refresh(page->ctx, cli_subagents_picker_event);
		return rc;
	}
	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct nt_view_controller),
		.load_view = cli_subagents_picker_page_build,
		.handle_event = cli_subagents_picker_page_event,
		.deinit = cli_subagents_picker_destroy};
	int accepted = 0;
	return cli_application_show_page(page->ctx, &type, page, 100, &accepted);
}

void cli_subagents_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->subagents)
		return;
	struct cli_subagents_page *page = ctx->subagents;
	cli_agents_list_free(page->items, page->count);
	cli_subagents_feeds_clear(page);
	nb_vec_cleanup(&page->feeds);
	nb_buf_cleanup(&page->picker_row);
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
				nt_application_invalidate(cli_application_tui_borrow(ctx),
							  NT_INVALIDATE_DISPLAY);
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
	nt_terminal_dimensions(STDOUT_FILENO, &page->columns, &page->rows);
	if (page->columns > 1)
		page->columns--;
	memset(&page->browser, 0, sizeof(page->browser));
	int rc = cli_subagents_refresh(page);
	if (rc != 0)
		return rc;
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
	page->open = 1;
	if (page->viewing || fullscreen)
		rc = cli_subagents_view_show(page);
	else
		rc = cli_subagents_picker_show(page);
	if (rc)
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
