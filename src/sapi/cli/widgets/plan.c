#include "sapi/cli/widgets/plan.h"
#include <morph/tui/widgets/stack.h>

#include "sapi/cli/widgets/notice.h"
#include "sapi/cli/presentation/style.h"
#include "util/cleanup.h"
#include "util/utf8.h"
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

static int cli_plan_line(morph_buf_t *out, const char *prefix, const char *text,
			int columns, int styled, enum cli_notice_status status)
{
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		text, strlen(text), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return -ENOMEM;
	int width = columns - (int)CLI_CONTENT_RIGHT_PADDING - (int)utf8_display_width(prefix) - 2;
	if (width < 1)
		width = 1;
	const char *end = utf8_advance_display_width_borrow(safe, (size_t)width);
	int clipped = *end != '\0';
	if (clipped)
		end = utf8_advance_display_width_borrow(safe, (size_t)(width - 1));
	int rc = morph_buf_printf(out, "%s%s%s%s ", prefix,
		styled ? cli_notice_color(status) : "", cli_notice_marker(status),
		styled ? ANSI_RESET : "");
	if (rc == 0)
		rc = morph_buf_append(out, safe, (size_t)(end - safe));
	if (rc == 0 && clipped)
		rc = morph_buf_puts(out, "…");
	return rc != 0 ? rc : morph_buf_putc(out, '\n');
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

static int cli_plan_one(morph_buf_t *out, const cJSON *steps, const struct cli_plan_style *style)
{
	int completed = 0;
	int count = cJSON_GetArraySize(steps);
	enum cli_notice_status status = cli_plan_status(steps, &completed);
	const cJSON *step;
	morph_buf_t heading MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_printf(&heading, "Plan · %d/%d completed", completed, count);
	if (rc == 0 && style->status_heading)
		rc = cli_plan_line(out, "", morph_buf_cstr_borrow(&heading), style->columns,
			style->styled, status);
	else if (rc == 0)
		rc = morph_buf_printf(out, "%s%s%s\n", style->styled ? ANSI_DIM : "",
			morph_buf_cstr_borrow(&heading), style->styled ? ANSI_RESET : "");
	cJSON_ArrayForEach(step, steps) {
		const char *description = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(step, "description"));
		if (!description || rc != 0)
			continue;
		const cJSON *next = step->next;
		while (next && !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(next, "description")))
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

int cli_plan_append(morph_buf_t *out, const cJSON *state, const struct cli_plan_style *style)
{
	if (!out || !state || !style)
		return -EINVAL;
	const cJSON *plans = cJSON_GetObjectItemCaseSensitive(state, "plans");
	const char *key = "id";
	const char *selected = cJSON_GetStringValue(
		cJSON_GetObjectItemCaseSensitive(state, "selected_plan_id"));
	if (!selected) {
		selected = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(state, "selected_plan"));
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

struct morph_tui_view *cli_plan_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent, const cJSON *state, int styled)
{
	const struct morph_tui_view_options outer = {0};
	struct morph_tui_view *root = morph_tui_widget_stack_add_borrow(builder, parent,
		MORPH_TUI_WIDGET_STACK_VERTICAL, 0, &outer);
	const char *key = "id";
	const char *selected = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(state, "selected_plan_id"));
	if (!selected) {
		selected = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(state, "selected_plan"));
		key = "name";
	}
	const cJSON *plan;
	cJSON_ArrayForEach(plan, cJSON_GetObjectItemCaseSensitive(state, "plans")) {
		const cJSON *steps = cli_plan_steps(plan, key, selected);
		if (!steps)
			continue;
		int completed = 0;
		enum cli_notice_status status = cli_plan_status(steps, &completed);
		morph_buf_t title MORPH_CLEANUP(morph_buf_cleanup) = {0};
		int rc = morph_buf_printf(&title, "Plan · %d/%d completed", completed,
			cJSON_GetArraySize(steps));
		morph_tui_builder_fail(builder, rc);
		struct cli_notice heading = {.status = status, .title = morph_buf_cstr_borrow(&title)};
		struct morph_tui_view *header = cli_notice_add_borrow(builder, root, &heading, styled);
		if (header)
			morph_tui_view_set_margin(header, (struct morph_tui_view_insets){0});
		const cJSON *step;
		cJSON_ArrayForEach(step, steps) {
			const char *description = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(step, "description"));
			if (!description)
				continue;
			struct cli_notice notice = {.status = cli_plan_step_status(step),
				.title = description, .tree_branch = step->next ? 1 : 2};
			struct morph_tui_view *row = cli_notice_add_borrow(builder, root, &notice, styled);
			if (row)
				morph_tui_view_set_margin(row, (struct morph_tui_view_insets){0});
		}
		if (selected)
			break;
	}
	return root;
}
