#include "sapi/cli/widgets/plan.h"
#include "nb/err.h"
#include "sapi/cli/presentation/messages.h"
#include <morph/tui/widget.h>
#include <morph/tui/widgets/text_view.h>

#include "sapi/cli/widgets/notice.h"
#include "sapi/cli/presentation/style.h"
#include "nb/cleanup.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include <errno.h>
#include <string.h>

static enum cli_notice_status cli_plan_step_status(const cJSON *step)
{
	const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(step, "status"));
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(step, "active")))
		return CLI_NOTICE_RUNNING;
	if (status && !strcmp(status, "completed"))
		return CLI_NOTICE_DONE;
	if (status && !strcmp(status, "failed"))
		return CLI_NOTICE_FAILED;
	if (status && (!strcmp(status, "skipped") || !strcmp(status, "cancelled")))
		return CLI_NOTICE_CANCELLED;
	if (status && !strcmp(status, "in_progress"))
		return CLI_NOTICE_RUNNING;
	return CLI_NOTICE_PENDING;
}

static int cli_plan_line(nb_buf *out, const char *prefix, const char *text, int columns, int styled,
			 enum cli_notice_status status)
{
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, strlen(text), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING - (int)nb_utf8_width_cstr(prefix) - 2;
	if (width < 1)
		width = 1;
	const char *end = morph_terminal_advance_display_width_borrow(safe, (size_t)width);
	int clipped = *end != '\0';
	if (clipped)
		end = morph_terminal_advance_display_width_borrow(safe, (size_t)(width - 1));
	int rc = (nb_buf_printf(out, "%s%s%s%s ", prefix, styled ? cli_notice_color(status) : "",
				cli_notice_marker(status), styled ? ANSI_RESET : "")
			  ? 0
			  : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_append(out, safe, (size_t)(end - safe)) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && clipped)
		rc = (nb_buf_append_cstr(out, "…") ? 0 : NB_ERR_NOMEM);
	return rc != 0 ? rc : (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);
}

static enum cli_notice_status cli_plan_status(const cJSON *steps, int *completed_out)
{
	int completed = 0;
	int count = cJSON_GetArraySize(steps);
	int running = 0;
	int failed = 0;
	int cancelled = 0;
	const cJSON *step;
	cJSON_ArrayForEach(step, steps) {
		enum cli_notice_status status = cli_plan_step_status(step);
		completed += status == CLI_NOTICE_DONE;
		running += status == CLI_NOTICE_RUNNING;
		failed += status == CLI_NOTICE_FAILED;
		cancelled += status == CLI_NOTICE_CANCELLED;
	}
	enum cli_notice_status status = failed ? CLI_NOTICE_FAILED
		: running ? CLI_NOTICE_RUNNING
		: count && completed == count ? CLI_NOTICE_DONE
		: count && completed + cancelled == count ? CLI_NOTICE_CANCELLED
		: CLI_NOTICE_PENDING;
	*completed_out = completed;
	return status;
}

static int cli_plan_one(nb_buf *out, const cJSON *steps, const struct cli_plan_style *style)
{
	int completed = 0;
	int count = cJSON_GetArraySize(steps);
	enum cli_notice_status status = cli_plan_status(steps, &completed);
	const cJSON *step;
	nb_buf heading NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_printf(&heading, "Plan · %d/%d completed", completed, count)
			  ? 0
			  : NB_ERR_NOMEM);
	if (rc == 0 && style->status_heading)
		rc = cli_plan_line(out, "", nb_buf_cstr(&heading), style->columns, style->styled,
				   status);
	else if (rc == 0)
		rc = (nb_buf_printf(out, "%s%s%s\n", style->styled ? ANSI_DIM : "",
				    nb_buf_cstr(&heading), style->styled ? ANSI_RESET : "")
			      ? 0
			      : NB_ERR_NOMEM);
	cJSON_ArrayForEach(step, steps) {
		const char *description = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(step, "description"));
		if (!description || rc != 0)
			continue;
		const cJSON *next = step->next;
		while (next && !cJSON_IsString(
			cJSON_GetObjectItemCaseSensitive(next, "description")))
			next = next->next;
		rc = cli_plan_line(out, next ? "├ " : "└ ", description, style->columns,
			style->styled, cli_plan_step_status(step));
	}
	return rc;
}

static const cJSON *cli_plan_steps(const cJSON *plan, const char *key, const char *selected)
{
	if (selected) {
		const char *value = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(plan, key));
		if (!value || strcmp(value, selected))
			return NULL;
	}
	const cJSON *steps = cJSON_GetObjectItemCaseSensitive(plan, "steps");
	return cJSON_IsArray(steps) ? steps : NULL;
}

int cli_plan_renders(const cJSON *state)
{
	if (!cJSON_IsObject(state))
		return 0;
	const char *key = "id";
	const char *selected = cJSON_GetStringValue(
		cJSON_GetObjectItemCaseSensitive(state, "selected_plan_id"));
	if (!selected) {
		selected = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(state, "selected_plan"));
		key = "name";
	}
	const cJSON *plans = cJSON_GetObjectItemCaseSensitive(state, "plans");
	const cJSON *plan;
	cJSON_ArrayForEach(plan, plans) {
		if (cli_plan_steps(plan, key, selected))
			return 1;
	}
	return 0;
}

int cli_plan_append(nb_buf *out, const cJSON *state, const struct cli_plan_style *style)
{
	if (!out || !state || !style)
		return NB_ERR_INVAL;
	const cJSON *plans = cJSON_GetObjectItemCaseSensitive(state, "plans");
	const char *key = "id";
	const char *selected = cJSON_GetStringValue(
		cJSON_GetObjectItemCaseSensitive(state, "selected_plan_id"));
	if (!selected) {
		selected = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(state, "selected_plan"));
		key = "name";
	}
	const cJSON *plan;
	cJSON_ArrayForEach(plan, plans) {
		const cJSON *steps = cli_plan_steps(plan, key, selected);
		if (!steps)
			continue;
		int rc = cli_plan_one(out, steps, style);
		if (rc != 0)
			return rc;
		if (selected)
			break;
	}
	return 0;
}

struct cli_plan_widget {
	struct morph_tui_widget base;
	struct morph_tui_view *body;
	cJSON *state;
	int columns, styled;
};

static int cli_plan_widget_measure(struct morph_tui_view *view, int width,
				   struct morph_tui_view_size *size)
{
	struct cli_plan_widget *widget = (struct cli_plan_widget *)view;
	if (widget->columns != width) {
		nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
		const struct cli_plan_style style = {
			.columns = width + (int)CLI_CONTENT_RIGHT_PADDING,
			.styled = widget->styled, .status_heading = 1};
		int rc = cli_plan_append(&rendered, widget->state, &style);
		while (!rc && rendered.len && rendered.ptr[rendered.len - 1] == '\n')
			rendered.ptr[--rendered.len] = '\0';
		if (!rc)
			rc = cli_messages_set_rendered_view(widget->body, nb_buf_cstr(&rendered),
							    widget->styled);
		if (rc)
			return rc;
		widget->columns = width;
	}
	return morph_tui_view_measure(widget->body, width, size);
}

static int cli_plan_widget_layout(struct morph_tui_view *view)
{
	return morph_tui_view_arrange(((struct cli_plan_widget *)view)->body,
				      morph_tui_view_content_rect(view));
}

static void cli_plan_widget_destroy(struct morph_tui_view *view)
{
	cJSON_Delete(((struct cli_plan_widget *)view)->state);
}

static const struct morph_tui_view_ops cli_plan_widget_ops = {
	.measure = cli_plan_widget_measure,
	.layout = cli_plan_widget_layout,
	.destroy = cli_plan_widget_destroy,
};

struct morph_tui_view *cli_plan_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, const cJSON *state, int styled)
{
	if (!state) {
		morph_tui_builder_fail(builder, NB_ERR_INVAL);
		return NULL;
	}
	const struct morph_tui_view_options options = {
		.margin = {.top = 1}, .padding = {.right = (int)CLI_CONTENT_RIGHT_PADDING}};
	struct cli_plan_widget *widget = (struct cli_plan_widget *)morph_tui_widget_add_borrow(
		builder, parent, sizeof(*widget), &cli_plan_widget_ops, &options);
	if (!widget)
		return NULL;
	struct morph_tui_view *view = &widget->base.view;
	widget->state = cJSON_Duplicate(state, 1);
	if (!widget->state)
		morph_tui_builder_fail(builder, NB_ERR_NOMEM);
	widget->columns = -1;
	widget->styled = styled;
	widget->body = morph_tui_widget_text_view_add_borrow(builder, view, "", NULL);
	if (widget->body) {
		morph_tui_builder_fail(builder,
			morph_tui_widget_text_view_set_content_sizing(widget->body, 1));
		morph_tui_view_set_focusable(widget->body, 0);
	}
	return view;
}
