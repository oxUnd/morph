#ifndef MORPH_CLI_PRESENTATION_COMMAND_OUTPUT_H
#define MORPH_CLI_PRESENTATION_COMMAND_OUTPUT_H

#include <stddef.h>

#include "util/buf.h"
#include "util/utf8.h"

struct cli_application;

/* Shared command-output model. Commands collect borrowed strings and
   semantic enums; presentation owns styling, indentation, widths, alignment,
   status markers, headers and footers. */

enum cli_output_style {
	CLI_OUTPUT_STYLE_PLAIN = 0,
	CLI_OUTPUT_STYLE_BOLD,
	CLI_OUTPUT_STYLE_DIM,
	CLI_OUTPUT_STYLE_CYAN,
	CLI_OUTPUT_STYLE_GREEN,
	CLI_OUTPUT_STYLE_YELLOW,
	CLI_OUTPUT_STYLE_RED,
};

struct cli_output_segment {
	enum cli_output_style style;
	const char *text;
	int untrusted;
	enum utf8_terminal_text_mode untrusted_mode;
};

struct cli_output_line {
	const struct cli_output_segment *segments;
	size_t segment_count;
	unsigned indent;
};

int cli_command_output_segments(struct cli_application *ctx,
				const struct cli_output_segment *segments, size_t count,
				unsigned indent, int newline);
int cli_command_output_line(struct cli_application *ctx, const struct cli_output_line *line);
int cli_command_output_linef(struct cli_application *ctx, enum cli_output_style style,
			     unsigned indent, const char *fmt, ...);
int cli_command_output_newline(struct cli_application *ctx);

/* Status rows reuse the shared tree/list renderer. */
enum cli_output_status {
	CLI_OUTPUT_STATUS_NONE = 0,
	CLI_OUTPUT_STATUS_PENDING,
	CLI_OUTPUT_STATUS_RUNNING,
	CLI_OUTPUT_STATUS_DONE,
	CLI_OUTPUT_STATUS_FAILED,
	CLI_OUTPUT_STATUS_CANCELLED,
	CLI_OUTPUT_STATUS_WARNING,
	CLI_OUTPUT_STATUS_QUESTION,
};

enum cli_output_row_place {
	CLI_OUTPUT_ROW_GROUP = 0,
	CLI_OUTPUT_ROW_INDENTED,
};

struct cli_output_status_row {
	enum cli_output_status status;
	enum cli_output_row_place place;
	int group_is_last;
	int is_last;
	int wide;
	const char *name;
	const char *description;
};

int cli_command_output_group(struct cli_application *ctx, const char *name, int count,
			     int is_last);
int cli_command_output_status_rows(struct cli_application *ctx,
				   const struct cli_output_status_row *rows, size_t count);
const char *cli_command_output_status_marker(enum cli_output_status status);

/* Field values may be text, integers, fixed-precision decimals or borrowed
   item lists. Separators default to "=". */
enum cli_output_field_kind {
	CLI_OUTPUT_FIELD_TEXT = 0,
	CLI_OUTPUT_FIELD_INT,
	CLI_OUTPUT_FIELD_DECIMAL_1,
	CLI_OUTPUT_FIELD_DECIMAL_3,
	CLI_OUTPUT_FIELD_DECIMAL_6,
	CLI_OUTPUT_FIELD_LIST,
};

struct cli_output_field {
	const char *label;
	enum cli_output_field_kind kind;
	const char *text;
	long long integer;
	double decimal;
	const char *const *items;
	size_t item_count;
	const char *separator;
	unsigned indent;
};

struct cli_output_section {
	const char *title;
	const struct cli_output_field *fields;
	size_t field_count;
	unsigned field_indent;
};

int cli_command_output_fields(struct cli_application *ctx,
			      const struct cli_output_field *fields, size_t count);
int cli_command_output_sections(struct cli_application *ctx,
				const struct cli_output_section *sections, size_t count);

/* Tables pad and align by display width. Markdown helpers escape cells and
   emit headers; terminal tables only emit a header when requested. */
enum cli_output_align {
	CLI_OUTPUT_ALIGN_LEFT = 0,
	CLI_OUTPUT_ALIGN_RIGHT,
};

struct cli_output_column {
	const char *header;
	int width;
	enum cli_output_align align;
	enum cli_output_style style;
};

struct cli_output_table {
	const struct cli_output_column *columns;
	size_t column_count;
	const char *const *cells;
	size_t row_count;
	const char *indent;
	const char *gap;
	int header;
};

int cli_command_output_table(struct cli_application *ctx, const struct cli_output_table *table);
int cli_command_output_markdown_heading(morph_buf_t *buf, unsigned level, const char *text);
int cli_command_output_markdown_table_header(morph_buf_t *buf, const struct cli_output_table *table);
int cli_command_output_markdown_table_row(morph_buf_t *buf, const struct cli_output_table *table,
					  const char *const *cells);
int cli_command_output_markdown_table(morph_buf_t *buf, const struct cli_output_table *table);
int cli_command_output_markdown_block(morph_buf_t *buf, const char *text);
int cli_command_output_markdown_render(struct cli_application *ctx, const char *markdown);

/* Ratio styling keeps threshold policy in presentation. */
enum cli_output_style cli_command_output_ratio_style(double ratio, double warn_ratio);
void cli_command_output_clear_screen(struct cli_application *ctx);

/* Slash-command help. Commands supply grouped, alias-collapsed data;
   presentation owns columns and header/footer typography. */
struct cli_help_item {
	const char *name;
	const char *alias;
	const char *description;
};

void cli_help_begin(struct cli_application *ctx);
void cli_help_group(struct cli_application *ctx, const char *name,
		    const struct cli_help_item *items, int count, int is_last);
void cli_help_end(struct cli_application *ctx);

/* Skill list rows; descriptions are borrowed for the duration of the call. */
enum cli_skill_list_state {
	CLI_SKILL_LIST_ACTIVE,
	CLI_SKILL_LIST_AVAILABLE,
	CLI_SKILL_LIST_DISABLED,
};

struct cli_skill_list_row {
	enum cli_skill_list_state state;
	const char *name;
	const char *description;
};

int cli_skill_list_render(struct cli_application *ctx, const struct cli_skill_list_row *rows,
			  size_t count, int active_count);

/* Tool list rows grouped by an opaque group id chosen by the command. */
struct cli_tool_list_row {
	int group;
	int enabled;
	const char *name;
	const char *description;
};

struct cli_tool_list_group {
	const char *name;
	int id;
};

int cli_tool_list_render(struct cli_application *ctx, const struct cli_tool_list_row *rows,
			 size_t row_count, const struct cli_tool_list_group *groups,
			 int group_count, int enabled_count);

/* Shared one-line receipts for interactive command handlers. */
void cli_command_print_cancelled(struct cli_application *ctx);
void cli_command_print_tree_note(struct cli_application *ctx, const char *text);

#endif
