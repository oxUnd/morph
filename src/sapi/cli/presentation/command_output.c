#include "sapi/cli/presentation/command_output.h"
#include "sapi/cli/pages/chat.h"

#include "sapi/cli/application.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/widgets/list.h"
#include "util/buf.h"
#include "util/cleanup.h"
#include "util/utf8.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *cli_command_output_style_prefix(enum cli_output_style style)
{
	switch (style) {
	case CLI_OUTPUT_STYLE_BOLD:
		return ANSI_BOLD;
	case CLI_OUTPUT_STYLE_DIM:
		return ANSI_DIM;
	case CLI_OUTPUT_STYLE_CYAN:
		return ANSI_CYAN;
	case CLI_OUTPUT_STYLE_GREEN:
		return ANSI_GREEN;
	case CLI_OUTPUT_STYLE_YELLOW:
		return ANSI_YELLOW;
	case CLI_OUTPUT_STYLE_RED:
		return ANSI_RED;
	case CLI_OUTPUT_STYLE_PLAIN:
	default:
		return "";
	}
}

static int cli_command_output_spaces(struct cli_application *ctx, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		if (cli_output_putchar(ctx, ' ') == EOF)
			return -EIO;
	}
	return 0;
}

static int cli_command_output_styled(struct cli_application *ctx, enum cli_output_style style,
				     const char *text)
{
	const char *prefix = cli_command_output_style_prefix(style);
	int styled = prefix[0] != '\0';

	if (styled && cli_output_fputs(ctx, prefix, stdout) == EOF)
		return -EIO;
	if (cli_output_fputs(ctx, text ? text : "", stdout) == EOF)
		return -EIO;
	if (styled && cli_output_fputs(ctx, ANSI_RESET, stdout) == EOF)
		return -EIO;
	return 0;
}

int cli_command_output_segments(struct cli_application *ctx,
				const struct cli_output_segment *segments, size_t count,
				unsigned indent, int newline)
{
	int rc;

	rc = cli_command_output_spaces(ctx, indent);
	if (rc != 0)
		return rc;
	for (size_t i = 0; segments && i < count; i++) {
		const struct cli_output_segment *segment = &segments[i];
		const char *prefix = cli_command_output_style_prefix(segment->style);
		int styled = prefix[0] != '\0';

		if (styled && cli_output_fputs(ctx, prefix, stdout) == EOF)
			return -EIO;
		if (segment->untrusted) {
			rc = cli_output_print_untrusted(ctx, segment->text ? segment->text : "",
							segment->untrusted_mode);
			if (rc < 0)
				return rc;
		} else if (cli_output_fputs(ctx, segment->text ? segment->text : "", stdout) ==
			   EOF) {
			return -EIO;
		}
		if (styled && cli_output_fputs(ctx, ANSI_RESET, stdout) == EOF)
			return -EIO;
	}
	if (newline && cli_output_putchar(ctx, '\n') == EOF)
		return -EIO;
	return 0;
}

int cli_command_output_line(struct cli_application *ctx, const struct cli_output_line *line)
{
	if (!line)
		return -EINVAL;
	return cli_command_output_segments(ctx, line->segments, line->segment_count, line->indent, 1);
}

int cli_command_output_linef(struct cli_application *ctx, enum cli_output_style style,
			     unsigned indent, const char *fmt, ...)
{
	struct cli_output_segment segment = {
		.style = style,
		.text = NULL,
		.untrusted = 0,
		.untrusted_mode = UTF8_TERMINAL_TEXT_SINGLE_LINE,
	};
	va_list ap;
	va_list ap_size;
	char *text;
	int needed;
	int rc;

	if (!fmt)
		return -EINVAL;
	va_start(ap, fmt);
	va_copy(ap_size, ap);
	needed = vsnprintf(NULL, 0, fmt, ap_size);
	va_end(ap_size);
	if (needed < 0) {
		va_end(ap);
		return needed;
	}
	text = malloc((size_t)needed + 1);
	if (!text) {
		va_end(ap);
		return -ENOMEM;
	}
	(void)vsnprintf(text, (size_t)needed + 1, fmt, ap);
	va_end(ap);
	segment.text = text;
	rc = cli_command_output_segments(ctx, &segment, 1, indent, 1);
	free(text);
	return rc;
}

int cli_command_output_newline(struct cli_application *ctx)
{
	return cli_output_putchar(ctx, '\n') == EOF ? -EIO : 0;
}

const char *cli_command_output_status_marker(enum cli_output_status status)
{
	switch (status) {
	case CLI_OUTPUT_STATUS_RUNNING:
		return ANSI_CYAN CLI_FEED_RUNNING ANSI_RESET;
	case CLI_OUTPUT_STATUS_DONE:
		return ANSI_GREEN CLI_FEED_DONE ANSI_RESET;
	case CLI_OUTPUT_STATUS_FAILED:
		return CLI_FEED_FAILED_STYLE CLI_FEED_FAILED ANSI_RESET;
	case CLI_OUTPUT_STATUS_CANCELLED:
		return ANSI_DIM CLI_FEED_CANCELLED ANSI_RESET;
	case CLI_OUTPUT_STATUS_WARNING:
		return ANSI_YELLOW CLI_FEED_WARNING ANSI_RESET;
	case CLI_OUTPUT_STATUS_QUESTION:
		return CLI_FEED_QUESTION_STYLE CLI_FEED_QUESTION ANSI_RESET;
	case CLI_OUTPUT_STATUS_PENDING:
		return ANSI_DIM CLI_FEED_PENDING ANSI_RESET;
	case CLI_OUTPUT_STATUS_NONE:
	default:
		return "";
	}
}

int cli_command_output_group(struct cli_application *ctx, const char *name, int count,
			     int is_last)
{
	cli_list_group(ctx, name, count, is_last);
	return 0;
}

int cli_command_output_status_rows(struct cli_application *ctx,
				   const struct cli_output_status_row *rows, size_t count)
{
	int columns = cli_list_columns();

	for (size_t i = 0; rows && i < count; i++) {
		const struct cli_output_status_row *row = &rows[i];
		const char *ancestor = row->place == CLI_OUTPUT_ROW_INDENTED
					       ? "  "
					       : morph_tui_widget_tree_branch(row->group_is_last, 1);
		int name_width = row->wide ? 46 : 24;

		cli_list_item(ctx, ancestor, row->is_last,
			      cli_command_output_status_marker(row->status), row->name,
			      row->description, name_width, columns);
	}
	return 0;
}

static int cli_command_output_field_value(struct cli_application *ctx,
					  const struct cli_output_field *field)
{
	switch (field->kind) {
	case CLI_OUTPUT_FIELD_INT:
		return cli_output_printf(ctx, "%lld", field->integer) < 0 ? -EIO : 0;
	case CLI_OUTPUT_FIELD_DECIMAL_1:
		return cli_output_printf(ctx, "%.1f", field->decimal) < 0 ? -EIO : 0;
	case CLI_OUTPUT_FIELD_DECIMAL_3:
		return cli_output_printf(ctx, "%.3f", field->decimal) < 0 ? -EIO : 0;
	case CLI_OUTPUT_FIELD_DECIMAL_6:
		return cli_output_printf(ctx, "%.6f", field->decimal) < 0 ? -EIO : 0;
	case CLI_OUTPUT_FIELD_TEXT:
	default:
		return cli_output_fputs(ctx, field->text ? field->text : "", stdout) == EOF
			       ? -EIO
			       : 0;
	}
}

static int cli_command_output_field(struct cli_application *ctx,
				    const struct cli_output_field *field,
				    unsigned default_indent)
{
	const char *separator = field->separator ? field->separator : "=";
	unsigned indent = field->indent ? field->indent : default_indent;
	int rc;

	rc = cli_command_output_spaces(ctx, indent);
	if (rc != 0)
		return rc;
	if (cli_output_fputs(ctx, field->label ? field->label : "", stdout) == EOF)
		return -EIO;
	if (!field->separator && cli_output_fputs(ctx, " ", stdout) == EOF)
		return -EIO;
	if (cli_output_fputs(ctx, separator, stdout) == EOF)
		return -EIO;
	if (field->kind == CLI_OUTPUT_FIELD_LIST) {
		for (size_t i = 0; i < field->item_count; i++) {
			if (cli_output_printf(ctx, " %s",
					      field->items && field->items[i] ? field->items[i] : "") <
			    0)
				return -EIO;
		}
	} else {
		if (cli_output_fputs(ctx, " ", stdout) == EOF)
			return -EIO;
		rc = cli_command_output_field_value(ctx, field);
		if (rc != 0)
			return rc;
	}
	return cli_command_output_newline(ctx);
}

int cli_command_output_fields(struct cli_application *ctx,
			      const struct cli_output_field *fields, size_t count)
{
	for (size_t i = 0; fields && i < count; i++) {
		int rc = cli_command_output_field(ctx, &fields[i], 2);

		if (rc != 0)
			return rc;
	}
	return 0;
}

int cli_command_output_sections(struct cli_application *ctx,
				const struct cli_output_section *sections, size_t count)
{
	for (size_t i = 0; sections && i < count; i++) {
		const struct cli_output_section *section = &sections[i];
		unsigned default_indent = section->field_indent ? section->field_indent : 2;

		if (section->title &&
		    cli_command_output_linef(ctx, CLI_OUTPUT_STYLE_BOLD, 0, "[%s]",
					     section->title) != 0)
			return -EIO;
		for (size_t j = 0; section->fields && j < section->field_count; j++) {
			int rc = cli_command_output_field(ctx, &section->fields[j],
							  default_indent);

			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int cli_command_output_table_cell(struct cli_application *ctx, const char *text,
					 const struct cli_output_column *column)
{
	const char *value = text ? text : "";
	int width = (int)utf8_display_width(value);
	int pad = column->width > width ? column->width - width : 0;
	int rc;

	if (column->align == CLI_OUTPUT_ALIGN_RIGHT) {
		rc = cli_command_output_spaces(ctx, (unsigned)pad);
		if (rc != 0)
			return rc;
		pad = 0;
	}
	rc = cli_command_output_styled(ctx, column->style, value);
	if (rc != 0)
		return rc;
	return cli_command_output_spaces(ctx, (unsigned)pad);
}

static int cli_command_output_table_row(struct cli_application *ctx,
					const struct cli_output_table *table,
					const char *const *cells)
{
	const char *gap = table->gap ? table->gap : "  ";
	int rc;

	if (cli_output_fputs(ctx, table->indent ? table->indent : "", stdout) == EOF)
		return -EIO;
	for (size_t i = 0; i < table->column_count; i++) {
		rc = cli_command_output_table_cell(ctx, cells ? cells[i] : NULL,
						   &table->columns[i]);
		if (rc != 0)
			return rc;
		if (i + 1 < table->column_count && cli_output_fputs(ctx, gap, stdout) == EOF)
			return -EIO;
	}
	return cli_command_output_newline(ctx);
}

int cli_command_output_table(struct cli_application *ctx, const struct cli_output_table *table)
{
	int rc;

	if (!table || !table->columns || table->column_count == 0)
		return -EINVAL;
	if (table->header) {
		const char **headers = calloc(table->column_count, sizeof(*headers));

		if (!headers)
			return -ENOMEM;
		for (size_t i = 0; i < table->column_count; i++)
			headers[i] = table->columns[i].header;
		rc = cli_command_output_table_row(ctx, table, headers);
		free(headers);
		if (rc != 0)
			return rc;
	}
	for (size_t row = 0; row < table->row_count; row++) {
		rc = cli_command_output_table_row(
			ctx, table,
			table->cells ? table->cells + row * table->column_count : NULL);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int cli_command_output_markdown_cell(morph_buf_t *buf, const char *text)
{
	const char *value = text ? text : "";
	int rc = 0;

	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		switch (*p) {
		case '|':
			rc = morph_buf_puts(buf, "\\|");
			break;
		case '\\':
			rc = morph_buf_puts(buf, "\\\\");
			break;
		case '\n':
		case '\t':
			rc = morph_buf_putc(buf, ' ');
			break;
		case '\r':
			break;
		default:
			if (*p < 0x20)
				rc = morph_buf_putc(buf, ' ');
			else
				rc = morph_buf_putc(buf, (char)*p);
			break;
		}
		if (rc != 0)
			return rc;
	}
	return 0;
}

int cli_command_output_markdown_heading(morph_buf_t *buf, unsigned level, const char *text)
{
	int rc;

	if (!buf || !text)
		return -EINVAL;
	if (buf->len > 0) {
		rc = morph_buf_putc(buf, '\n');
		if (rc != 0)
			return rc;
	}
	for (unsigned i = 0; i < level; i++) {
		rc = morph_buf_putc(buf, '#');
		if (rc != 0)
			return rc;
	}
	return morph_buf_printf(buf, " %s\n\n", text);
}

int cli_command_output_markdown_table_header(morph_buf_t *buf, const struct cli_output_table *table)
{
	int rc;

	if (!buf || !table || !table->columns || table->column_count == 0)
		return -EINVAL;
	rc = morph_buf_putc(buf, '|');
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < table->column_count; i++) {
		rc = morph_buf_printf(buf, " %s |",
				      table->columns[i].header ? table->columns[i].header : "");
		if (rc != 0)
			return rc;
	}
	rc = morph_buf_putc(buf, '\n');
	if (rc != 0)
		return rc;
	rc = morph_buf_putc(buf, '|');
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < table->column_count; i++) {
		rc = morph_buf_puts(buf, table->columns[i].align == CLI_OUTPUT_ALIGN_RIGHT ? "---:|"
										      : "---|");
		if (rc != 0)
			return rc;
	}
	return morph_buf_putc(buf, '\n');
}

int cli_command_output_markdown_table_row(morph_buf_t *buf, const struct cli_output_table *table,
					  const char *const *cells)
{
	int rc;

	if (!buf || !table || !table->columns || !cells)
		return -EINVAL;
	rc = morph_buf_putc(buf, '|');
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < table->column_count; i++) {
		rc = morph_buf_putc(buf, ' ');
		if (rc == 0)
			rc = cli_command_output_markdown_cell(buf, cells[i]);
		if (rc == 0)
			rc = morph_buf_puts(buf, " |");
		if (rc != 0)
			return rc;
	}
	return morph_buf_putc(buf, '\n');
}

int cli_command_output_markdown_table(morph_buf_t *buf, const struct cli_output_table *table)
{
	int rc;

	rc = cli_command_output_markdown_table_header(buf, table);
	for (size_t row = 0; rc == 0 && row < table->row_count; row++)
		rc = cli_command_output_markdown_table_row(
			buf, table,
			table->cells ? table->cells + row * table->column_count : NULL);
	return rc;
}

int cli_command_output_markdown_block(morph_buf_t *buf, const char *text)
{
	const char *value = text ? text : "";
	int line_start = 1;
	int rc = 0;

	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		if (line_start) {
			rc = morph_buf_puts(buf, "    ");
			if (rc != 0)
				return rc;
			line_start = 0;
		}
		if (*p == '\r')
			continue;
		rc = morph_buf_putc(buf, (char)*p);
		if (rc != 0)
			return rc;
		if (*p == '\n')
			line_start = 1;
	}
	if (line_start)
		rc = morph_buf_puts(buf, "    ");
	if (rc == 0)
		rc = morph_buf_putc(buf, '\n');
	return rc;
}

int cli_command_output_markdown_render(struct cli_application *ctx, const char *markdown)
{
	if (!markdown)
		return -EINVAL;
	if (ctx && ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return cli_output_printf(ctx, "%s", markdown) < 0 ? -EIO : 0;
	cli_markdown_render_ansi(ctx, markdown);
	return 0;
}

enum cli_output_style cli_command_output_ratio_style(double ratio, double warn_ratio)
{
	return ratio >= warn_ratio ? CLI_OUTPUT_STYLE_YELLOW : CLI_OUTPUT_STYLE_PLAIN;
}

void cli_command_output_clear_screen(struct cli_application *ctx)
{
	if (cli_chat_active(ctx)) {
		(void)cli_chat_reset(ctx);
		return;
	}
	if (ctx && (ctx->output.structured || ctx->output.capture))
		return;
	if (fputs("\033[2J\033[H", stdout) == EOF)
		return;
	(void)fflush(stdout);
}

static void cli_command_output_help_display(char *dst, size_t cap, const struct cli_help_item *item)
{
	const char *shorter;
	const char *longer;
	const char *short_name;
	const char *long_name;
	size_t short_len;

	if (!item->alias) {
		snprintf(dst, cap, "%s", item->name);
		return;
	}
	shorter = strlen(item->name) <= strlen(item->alias) ? item->name : item->alias;
	longer = strlen(item->name) <= strlen(item->alias) ? item->alias : item->name;
	short_name = shorter + 1;
	long_name = longer + 1;
	short_len = strlen(short_name);
	if (strncmp(short_name, long_name, short_len) == 0)
		snprintf(dst, cap, "/%s[%s]", short_name, long_name + short_len);
	else
		snprintf(dst, cap, "%s,%s", shorter, longer);
}

void cli_help_begin(struct cli_application *ctx)
{
	cli_output_printf(ctx, "\n" ANSI_BOLD ANSI_CYAN "Commands" ANSI_RESET "\n");
}

void cli_help_group(struct cli_application *ctx, const char *name,
		    const struct cli_help_item *items, int count, int is_last)
{
	const char *ancestor = morph_tui_widget_tree_branch(is_last, 1);
	int columns = cli_list_columns();

	cli_list_group(ctx, name ? name : "Other", count, is_last);
	if (!items)
		return;
	for (int i = 0; i < count; i++) {
		char display[128];

		cli_command_output_help_display(display, sizeof(display), &items[i]);
		cli_list_item(ctx, ancestor, i == count - 1, "", display, items[i].description,
			      22, columns);
	}
}

void cli_help_end(struct cli_application *ctx)
{
	cli_output_printf(ctx,
			  "\n" ANSI_DIM "Use /help <command> for details." ANSI_RESET "\n");
}

static const char *cli_command_output_skill_marker(enum cli_skill_list_state state)
{
	if (state == CLI_SKILL_LIST_DISABLED)
		return "×";
	return state == CLI_SKILL_LIST_ACTIVE ? CLI_FEED_DONE : CLI_FEED_PENDING;
}

int cli_skill_list_render(struct cli_application *ctx, const struct cli_skill_list_row *rows,
			  size_t count, int active_count)
{
	struct morph_tui_widget_tree tree MORPH_CLEANUP(morph_tui_widget_tree_cleanup) = {0};
	morph_buf_t output MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int columns = cli_list_columns();
	int rc;

	cli_output_printf(ctx, "\n" ANSI_BOLD ANSI_CYAN CLI_FEED_MESSAGE " Skills %zu" ANSI_DIM
			  " · %d active" ANSI_RESET "\n", count, active_count);
	if (count == 0) {
		cli_output_printf(ctx, "  " ANSI_DIM "(none — install to ~/.morph/skills/ "
				  "or ~/.agents/skills/)" ANSI_RESET "\n");
		return 0;
	}
	rc = morph_tui_widget_tree_init(&tree);
	if (rc != 0)
		return rc;
	rc = morph_buf_init(&output, 1024);
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < count; i++) {
		morph_buf_t label MORPH_CLEANUP(morph_buf_cleanup) = {0};

		rc = morph_buf_init(&label, 128);
		if (rc != 0)
			break;
		morph_buf_printf(&label, "%s %s", cli_command_output_skill_marker(rows[i].state), rows[i].name);
		if (label.failed)
			rc = label.failed;
		else
			(void)morph_tui_widget_tree_add(&tree, MORPH_TUI_WIDGET_TREE_ROOT,
							morph_buf_cstr_borrow(&label),
							rows[i].description);
		if (rc != 0)
			break;
	}
	if (rc == 0)
		rc = morph_tui_widget_tree_render(&tree, &output, columns, 0, SIZE_MAX);
	if (rc == 0)
		cli_output_fputs(ctx, morph_buf_cstr_borrow(&output), stdout);
	cli_output_printf(ctx, "\n  " ANSI_DIM CLI_FEED_DONE " active   " CLI_FEED_PENDING
			  " available   × disabled" ANSI_RESET "\n");
	return rc;
}

static const char *cli_command_output_tool_marker(const struct cli_tool_list_row *row)
{
	return row->enabled ? ANSI_GREEN CLI_FEED_DONE ANSI_RESET
			    : ANSI_DIM CLI_FEED_PENDING ANSI_RESET;
}

static int cli_command_output_tool_group_name_width(const struct cli_tool_list_row *rows, size_t row_count,
				     int group)
{
	int width = 12;

	for (size_t i = 0; i < row_count; i++) {
		size_t item_width;

		if (rows[i].group != group)
			continue;
		item_width = utf8_display_width(rows[i].name);
		if (item_width > (size_t)width)
			width = item_width > 28u ? 28 : (int)item_width;
	}
	return width;
}

static int cli_command_output_tool_group_count(const struct cli_tool_list_row *rows, size_t row_count,
				int group)
{
	int count = 0;

	for (size_t i = 0; i < row_count; i++) {
		if (rows[i].group == group)
			count++;
	}
	return count;
}

int cli_tool_list_render(struct cli_application *ctx, const struct cli_tool_list_row *rows,
			 size_t row_count, const struct cli_tool_list_group *groups,
			 int group_count, int enabled_count)
{
	int columns = cli_list_columns();
	int printed_groups = 0;

	cli_output_printf(ctx, "\n" ANSI_BOLD ANSI_CYAN CLI_FEED_MESSAGE " Tools %zu" ANSI_DIM
			  " · %d enabled" ANSI_RESET "\n", row_count, enabled_count);
	if (row_count == 0) {
		cli_output_printf(ctx, "  " ANSI_DIM "(none)" ANSI_RESET "\n");
		return 0;
	}
	for (int g = 0; g < group_count; g++) {
		int is_last = g == group_count - 1 ? 1 : 0;
		int count = cli_command_output_tool_group_count(rows, row_count, groups[g].id);
		int name_width;
		int index = 0;
		const char *ancestor;

		if (count == 0)
			continue;
		name_width = cli_command_output_tool_group_name_width(rows, row_count, groups[g].id);
		ancestor = morph_tui_widget_tree_branch(is_last, 1);
		if (printed_groups > 0)
			cli_output_putchar(ctx, '\n');
		printed_groups++;
		cli_list_group(ctx, groups[g].name, count, is_last);
		for (size_t i = 0; i < row_count; i++) {
			if (rows[i].group != groups[g].id)
				continue;
			index++;
			cli_list_item(ctx, ancestor, index == count, cli_command_output_tool_marker(&rows[i]),
				      rows[i].name, rows[i].description, name_width, columns);
		}
	}
	cli_output_printf(ctx, "\n  " ANSI_DIM CLI_FEED_DONE " enabled   " CLI_FEED_PENDING
			  " disabled" ANSI_RESET "\n");
	return 0;
}

void cli_command_print_cancelled(struct cli_application *ctx)
{
	cli_output_printf(ctx, ANSI_DIM "  cancelled" ANSI_RESET "\n");
}

void cli_command_print_tree_note(struct cli_application *ctx, const char *text)
{
	cli_output_printf(ctx, ANSI_DIM);
	cli_list_tree_prefix(ctx, NULL, 0, 1, 0);
	cli_output_printf(ctx, "%s" ANSI_RESET "\n", text ? text : "");
}
