#include "sapi/cli/pages/subagents.h"

#include "sapi/cli/application.h"
#include "sapi/cli/models/subagents.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/widgets/agents.h"
#include "sapi/cli/widgets/transcript.h"
#include "sapi/cli/widgets/subagents.h"
#include "util/child_io.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include <morph/tui/components.h>
#include <morph/tui/frame.h>
#include <morph/tui/input.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct cli_subagents_page {
	struct cli_application *ctx;
	struct morph_tui_frame frame;
	struct morph_tui_widget_list_state selection;
	struct morph_tui_widget_text_layout lines;
	struct agent_info *items;
	int count;
	struct agent_info *detail;
	struct cli_subagents_thread *thread;
	morph_buf_t document;
	morph_buf_t draft;
	morph_buf_t message;
	size_t scroll;
	int open;
	int viewing;
	int editing;
	int follow;
	int dirty;
	int columns;
	int rows;
	int64_t refreshed;
	int64_t rendered;
};

static const char *cli_subagents_label(const char *id)
{
	return !strncmp(id, "/root/", 6) ? id + 6 : id;
}

static const char *cli_subagents_activity(const struct agent_info *agent)
{
	if (agent->status == AGENT_RUNNING || agent->status == AGENT_PENDING)
		return agent->activity ? agent->activity : "Starting";
	if (agent->error_code < 0)
		return morph_strerror(agent->error_code);
	return agent->status == AGENT_COMPLETED ? "Result available" : "";
}

static const char *cli_subagents_selected(const struct cli_subagents_page *page)
{
	if (!page->selection.index || page->selection.index > (size_t)page->count)
		return "/root";
	return page->items[page->selection.index - 1].id;
}

static int cli_subagents_refresh(struct cli_subagents_page *page)
{
	struct agent_info *items = NULL;
	int count = 0;
	int64_t session = 0;
	(void)runtime_session_current_id(page->ctx->runtime, &session);
	int rc = runtime_agent_activity(page->ctx->runtime, session, &items, &count);
	if (rc != 0)
		return rc;
	char selected[256];
	const char *id = cli_subagents_selected(page);
	memcpy(selected, id, strlen(id) + 1);
	runtime_agent_list_free(page->items, page->count);
	page->items = items;
	page->count = count;
	page->selection.index = 0;
	for (int i = 0; i < count; i++) {
		if (!strcmp(items[i].id, selected)) {
			page->selection.index = (size_t)i + 1;
			break;
		}
	}
	if (page->thread && page->detail) {
		for (int i = 0; i < count; i++) {
			if (!strcmp(items[i].id, page->thread->id) &&
			    (items[i].status != page->detail->status ||
			     items[i].started_at_ms != page->detail->started_at_ms)) {
				struct agent_info *detail = NULL;
				if (runtime_agent_read(page->ctx->runtime, page->thread->id,
						       &detail) == 0) {
					runtime_agent_list_free(page->detail, 1);
					page->detail = detail;
					page->dirty = 1;
				}
				break;
			}
		}
	}
	page->refreshed = morph_io_now_ms();
	return 0;
}

struct cli_subagents_events {
	char **items;
	int count;
};

static void cli_subagents_events_cleanup(struct cli_subagents_events *events)
{
	runtime_agent_events_free(events->items, events->count);
}

static int cli_subagents_select(struct cli_subagents_page *page)
{
	const char *id = cli_subagents_selected(page);
	(void)runtime_agent_select(page->ctx->runtime, NULL);
	page->viewing = strcmp(id, "/root") != 0;
	page->editing = 0;
	page->follow = 1;
	page->scroll = 0;
	page->dirty = 1;
	cli_subagents_thread_destroy(page->thread);
	page->thread = NULL;
	runtime_agent_list_free(page->detail, page->detail ? 1 : 0);
	page->detail = NULL;
	if (!page->viewing)
		return 0;
	int rc = cli_subagents_thread_create(id, &page->thread);
	if (rc == 0)
		rc = runtime_agent_read(page->ctx->runtime, id, &page->detail);
	struct cli_subagents_events events MORPH_CLEANUP(cli_subagents_events_cleanup) = {0};
	int64_t sequence = 0;
	if (rc == 0)
		rc = runtime_agent_subscribe(page->ctx->runtime, id, &events.items, &events.count,
					     &sequence);
	for (int i = 0; i < events.count && rc == 0; i++)
		rc = cli_subagents_thread_replay(page->thread, events.items[i]);
	if (rc == 0)
		page->thread->sequence = sequence;
	else {
		(void)runtime_agent_select(page->ctx->runtime, NULL);
		page->viewing = 0;
	}
	return rc;
}

static int cli_subagents_document(struct cli_subagents_page *page)
{
	morph_buf_reset(&page->document);
	if (!page->viewing || !page->thread || !page->detail)
		return 0;
	int rc = cli_subagents_transcript(page->ctx, page->thread, &page->document, page->columns);
	if (rc == 0 && !page->thread->segments.nelts)
		rc = cli_agents_detail(&page->document, "Assignment",
				       page->detail->description ? page->detail->description : "",
				       page->columns, 0);
	if (rc == 0 && !page->thread->has_content && page->detail->result)
		rc = cli_agents_detail(&page->document, "Result", page->detail->result,
				       page->columns, 0);
	if (rc != 0)
		return rc;
	char *safe MORPH_CLEANUP(morph_charp_cleanup) =
		utf8_terminal_sanitize_dup(morph_buf_cstr_borrow(&page->document),
					   page->document.len, UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return -ENOMEM;
	morph_buf_reset(&page->document);
	rc = morph_buf_puts(&page->document, safe);
	if (rc == 0)
		rc = morph_tui_widget_text_layout_wrap_borrow(
			&page->lines, morph_buf_cstr_borrow(&page->document),
			(size_t)(page->columns > 4 ? page->columns - 4 : 1));
	page->dirty = 0;
	page->rendered = morph_io_now_ms();
	return rc;
}

static int cli_subagents_line(morph_buf_t *frame, const char *text, int columns, const char *style)
{
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	char clipped[BUFSIZ];
	if (!safe)
		return -ENOMEM;
	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), safe,
						 (size_t)(columns > 1 ? columns - 1 : 0), 0);
	return morph_buf_printf(frame, "%s%s%s\n", style, clipped, *style ? ANSI_RESET : "");
}

static int cli_subagents_draw(void *data, const struct morph_tui_screen *screen)
{
	struct cli_subagents_page *page = data;
	int changed = page->columns != screen->area.width;
	page->columns = screen->area.width;
	page->rows = screen->area.height;
	if (changed)
		page->dirty = 1;
	int height = page->rows > 6 ? page->rows - 6 : 1;
	morph_tui_widget_list_state_layout(&page->selection, (size_t)page->count + 1,
					   (size_t)height);
	if (page->viewing && page->dirty &&
	    (changed || morph_io_now_ms() - page->rendered >= 200)) {
		int rc = cli_subagents_document(page);
		if (rc != 0)
			return rc;
	}
	if (page->viewing) {
		page->scroll = page->follow && page->lines.lines.nelts > (size_t)height
				       ? page->lines.lines.nelts - (size_t)height
				       : page->scroll;
		page->scroll = morph_tui_widget_scroll_update(page->scroll, page->lines.lines.nelts,
							      (size_t)height, 0);
	}
	morph_buf_t frame MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t line MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&frame, BUFSIZ);
	if (rc == 0)
		rc = morph_buf_init(&line, 256);
	int styled = cli_output_color_enabled(page->ctx);
	for (int row = 0; row < page->rows && rc == 0; row++) {
		morph_buf_reset(&line);
		const char *style = "";
		if (row == 0) {
			(void)morph_buf_printf(
				&line, "  Subagents%s%s", page->viewing ? " / " : "",
				page->viewing ? cli_subagents_label(cli_subagents_selected(page))
					      : "");
			style = styled ? ANSI_BOLD : "";
		} else if (row == 1 && page->viewing && page->detail) {
			const struct agent_info *current =
				page->selection.index &&
						page->selection.index <= (size_t)page->count
					? &page->items[page->selection.index - 1]
					: page->detail;
			(void)morph_buf_printf(&line, "  %s · %s",
					       cli_agents_status(current->status),
					       cli_subagents_activity(current));
			style = styled ? ANSI_DIM : "";
		} else if (row >= 3 && row < height + 3) {
			if (page->viewing) {
				struct morph_tui_widget_text_line *text = morph_array_borrow(
					&page->lines.lines, page->scroll + (size_t)(row - 3));
				if (text) {
					(void)morph_buf_puts(&line, "  ");
					(void)morph_buf_append(&line,
							       page->document.data + text->offset,
							       text->length);
				}
			} else {
				size_t index = page->selection.first + (size_t)(row - 3);
				if (!index)
					(void)morph_buf_puts(&line, "  /root  Main conversation");
				else if (index <= (size_t)page->count) {
					const struct agent_info *agent = &page->items[index - 1];
					(void)morph_buf_printf(&line, "  %s  · %s  · %s",
							       cli_subagents_label(agent->id),
							       cli_agents_status(agent->status),
							       cli_subagents_activity(agent));
				}
				if (index == page->selection.index)
					style = styled ? ANSI_CYAN "\033[7m" : "";
			}
		} else if (row == page->rows - 2) {
			if (page->editing)
				(void)morph_buf_printf(&line, "  Follow up › %s",
						       morph_buf_cstr_borrow(&page->draft));
			else
				(void)morph_buf_printf(&line, "  %s",
						       morph_buf_cstr_borrow(&page->message));
		} else if (row == page->rows - 1) {
			(void)morph_buf_puts(
				&line, page->editing   ? "  Enter send · Esc cancel"
				       : page->viewing ? "  Tab/←/→ switch · PgUp/PgDn scroll · f "
							 "follow up · k interrupt · Esc back"
						       : "  ↑/↓ select · Enter view · Tab switch · "
							 "Esc main conversation");
			style = styled ? ANSI_DIM : "";
		}
		rc = cli_subagents_line(&frame, morph_buf_cstr_borrow(&line), page->columns, style);
	}
	if (rc == 0)
		rc = morph_tui_frame_present(&page->frame, screen->session.output, page->columns,
					     page->rows, morph_buf_cstr_borrow(&frame));
	return rc;
}

static int cli_subagents_update(void *data, const struct morph_tui_event *event)
{
	struct cli_subagents_page *page = data;
	if (event->type == MORPH_TUI_EVENT_TICK || event->type == MORPH_TUI_EVENT_WAKE) {
		if (cli_event_queue_owner_call_pending(page->ctx))
			return MORPH_TUI_SCREEN_CLOSE;
		if (morph_io_now_ms() - page->refreshed >= 500) {
			int rc = cli_subagents_refresh(page);
			if (rc != 0)
				return rc;
			return MORPH_TUI_SCREEN_REDRAW;
		}
		return page->dirty ? MORPH_TUI_SCREEN_REDRAW : MORPH_TUI_SCREEN_CONTINUE;
	}
	if (event->type == MORPH_TUI_EVENT_RESIZE)
		return MORPH_TUI_SCREEN_REDRAW;
	if (event->type != MORPH_TUI_EVENT_KEY)
		return MORPH_TUI_SCREEN_CONTINUE;
	int key = event->key;
	if (page->editing) {
		if (key == MORPH_TUI_KEY_PASTE_BEGIN || key == MORPH_TUI_KEY_PASTE_END)
			return MORPH_TUI_SCREEN_REDRAW;
		if (event->pasting) {
			if (key > 0 && key <= 255 && page->draft.len < 65532)
				(void)morph_buf_putc(&page->draft, key == '\r' ? '\n' : (char)key);
		} else if (key == 27 || key == 3)
			page->editing = 0;
		else if (key == '\r' || key == '\n') {
			int rc = utf8valid(morph_buf_cstr_borrow(&page->draft))
					 ? -EINVAL
					 : runtime_agent_followup(
						   page->ctx->runtime, cli_subagents_selected(page),
						   morph_buf_cstr_borrow(&page->draft));
			morph_buf_reset(&page->message);
			(void)morph_buf_puts(&page->message,
					     rc ? morph_strerror(rc) : "Follow-up sent");
			if (rc == 0) {
				page->editing = 0;
				morph_buf_reset(&page->draft);
			}
		} else if ((key == 127 || key == 8) && page->draft.len)
			(void)morph_buf_truncate(
				&page->draft,
				utf8_clamp_bytes(page->draft.data, page->draft.len - 1));
		else if (key >= 32 && key <= 255 && page->draft.len < 65532)
			(void)morph_buf_putc(&page->draft, (char)key);
		return MORPH_TUI_SCREEN_REDRAW;
	}
	if (event->pasting)
		return MORPH_TUI_SCREEN_CONTINUE;
	if (key == 27 || key == 3 || key == 4 || key == 'q') {
		if (page->viewing && key != 3 && key != 4) {
			page->viewing = 0;
			(void)runtime_agent_select(page->ctx->runtime, NULL);
		} else
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
	} else if ((key == '\r' || key == '\n') && !page->viewing) {
		if (!page->selection.index)
			return MORPH_TUI_SCREEN_CLOSE;
		int rc = cli_subagents_select(page);
		if (rc != 0)
			return rc;
	} else if ((key == 'f' || key == 'k') && page->selection.index) {
		if (key == 'f') {
			page->editing = 1;
			morph_buf_reset(&page->draft);
		} else {
			int rc = runtime_agent_interrupt(page->ctx->runtime,
							 cli_subagents_selected(page));
			morph_buf_reset(&page->message);
			(void)morph_buf_puts(&page->message,
					     rc ? morph_strerror(rc) : "Interrupt requested");
		}
	} else if (page->viewing) {
		size_t delta = key == MORPH_TUI_KEY_PAGE_UP || key == MORPH_TUI_KEY_PAGE_DOWN
				       ? (size_t)(page->rows > 6 ? page->rows - 6 : 1)
				       : 1;
		if (key == MORPH_TUI_KEY_UP || key == MORPH_TUI_KEY_PAGE_UP) {
			page->scroll = delta > page->scroll ? 0 : page->scroll - delta;
			page->follow = 0;
		} else if (key == MORPH_TUI_KEY_DOWN || key == MORPH_TUI_KEY_PAGE_DOWN) {
			page->scroll += delta;
			page->follow = 0;
		} else if (key == MORPH_TUI_KEY_HOME) {
			page->scroll = 0;
			page->follow = 0;
		} else if (key == MORPH_TUI_KEY_END)
			page->follow = 1;
	} else
		(void)morph_tui_widget_list_state_update(&page->selection, key);
	return MORPH_TUI_SCREEN_REDRAW;
}

static void cli_subagents_complete(void *data, int result)
{
	struct cli_subagents_page *page = data;
	(void)result;
	page->open = 0;
	page->editing = 0;
	(void)runtime_agent_select(page->ctx->runtime, NULL);
}

void cli_subagents_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->subagents)
		return;
	struct cli_subagents_page *page = ctx->subagents;
	runtime_agent_list_free(page->items, page->count);
	runtime_agent_list_free(page->detail, page->detail ? 1 : 0);
	cli_subagents_thread_destroy(page->thread);
	morph_buf_cleanup(&page->document);
	morph_buf_cleanup(&page->draft);
	morph_buf_cleanup(&page->message);
	morph_tui_widget_text_layout_cleanup(&page->lines);
	morph_tui_frame_cleanup(&page->frame);
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
	if (cli_subagents_is_open(ctx) && ctx->subagents->viewing && ctx->subagents->thread) {
		int rc = cli_subagents_thread_event(ctx->subagents->thread, event);
		if (rc != 0)
			return rc;
		ctx->subagents->dirty = 1;
		morph_tui_application_invalidate(cli_application_tui_borrow(ctx),
						 MORPH_TUI_INVALIDATE_DISPLAY);
	}
	return 1;
}

int cli_subagents_show(struct cli_application *ctx, const char *id)
{
	if (!ctx || !ctx->runtime || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return -ENOTSUP;
	if (!ctx->subagents) {
		ctx->subagents = calloc(1, sizeof(*ctx->subagents));
		if (!ctx->subagents)
			return -ENOMEM;
		ctx->subagents->ctx = ctx;
		int rc = morph_buf_init(&ctx->subagents->document, BUFSIZ);
		if (rc == 0)
			rc = morph_buf_init(&ctx->subagents->draft, 256);
		if (rc == 0)
			rc = morph_buf_init(&ctx->subagents->message, 128);
		if (rc != 0) {
			cli_subagents_cleanup(ctx);
			return rc;
		}
	}
	struct cli_subagents_page *page = ctx->subagents;
	if (page->open)
		return -EBUSY;
	int rc = cli_subagents_refresh(page);
	if (rc != 0)
		return rc;
	page->viewing = 0;
	page->editing = 0;
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
			return -ENOENT;
		rc = cli_subagents_select(page);
	}
	if (rc != 0)
		return rc;
	static const struct morph_tui_screen_ops ops = {
		.update = cli_subagents_update,
		.draw = cli_subagents_draw,
	};
	rc = cli_application_present_screen(ctx, &ops, page, cli_subagents_complete);
	if (rc == 0)
		page->open = 1;
	return rc;
}
