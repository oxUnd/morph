#include "sapi/cli/internal.h"
#include "sapi/cli/list_ui.h"

#include <sys/ioctl.h>

#define CLI_LIST_DEFAULT_COLUMNS 100
#define CLI_LIST_TEXT_MAX 8192
#define CLI_LIST_DEPTH_MAX 5
#define CLI_LIST_ITEMS_MAX 12

void cli_tree_prefix(const int *last, int depth, int is_last, int continuation)
{
	morph_buf_t output = {0};
	if (depth < 0 || morph_buf_init(&output, 64) != 0)
		return;
	if (tui_tree_prefix(&output, last, (size_t)depth, is_last,
			    continuation, 2) == 0)
		fputs(morph_buf_cstr(&output), stdout);
	morph_buf_cleanup(&output);
}

int cli_list_columns(void)
{
	struct winsize ws;
	const char *configured;
	char *end;
	long columns;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	configured = getenv("COLUMNS");
	if (!configured || !configured[0])
		return CLI_LIST_DEFAULT_COLUMNS;
	errno = 0;
	columns = strtol(configured, &end, 10);
	if (errno != 0 || *end != '\0' || columns <= 0 ||
	    columns > INT_MAX)
		return CLI_LIST_DEFAULT_COLUMNS;
	return (int)columns;
}

size_t cli_list_compact_text(char *dst, size_t dst_cap, const char *src)
{
	char *safe;
	size_t in = 0;
	size_t out = 0;
	size_t src_len;
	int pending_space = 0;

	if (!dst || dst_cap == 0)
		return 0;
	dst[0] = '\0';
	if (!src)
		return 0;
	safe = utf8_terminal_sanitize_dup(src, strlen(src),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return 0;
	src = safe;
	src_len = strlen(safe);
	while (src[in]) {
		unsigned cp;
		size_t cp_len;
		int space;

		if (!utf8_decode_codepoint(src + in, src_len - in, &cp,
					   &cp_len)) {
			in++;
			continue;
		}
		space = cp < 128u ?
			isspace((unsigned char)cp) :
			utf8_is_unicode_space_cp(cp);
		if (space) {
			if (out > 0)
				pending_space = 1;
			in += cp_len;
			continue;
		}
		if (pending_space && out + 1 < dst_cap) {
			dst[out++] = ' ';
			pending_space = 0;
		}
		if (out + cp_len >= dst_cap)
			break;
		memcpy(dst + out, src + in, cp_len);
		out += cp_len;
		in += cp_len;
	}
	dst[out] = '\0';
	free(safe);
	return out;
}

static void cli_list_print_clipped(const char *text, int max_width)
{
	char compact[CLI_LIST_TEXT_MAX];
	char clipped[CLI_LIST_TEXT_MAX];
	size_t width;

	if (max_width <= 0)
		return;
	(void)cli_list_compact_text(compact, sizeof(compact), text);
	width = utf8_display_width(compact);
	if (width <= (size_t)max_width) {
		fputs(compact, stdout);
		return;
	}
	if (max_width == 1) {
		fputs("…", stdout);
		return;
	}
	(void)utf8_copy_display_width(clipped, sizeof(clipped), compact,
				      (size_t)max_width - 1);
	printf("%s…", clipped);
}

void cli_list_group(const char *name, int count, int is_last)
{
	char compact[CLI_LIST_TEXT_MAX];
	morph_buf_t label = {0};
	morph_buf_t output = {0};

	if (morph_buf_init(&label, 128) != 0)
		return;
	if (morph_buf_init(&output, 128) != 0) {
		morph_buf_cleanup(&label);
		return;
	}
	cli_list_compact_text(compact, sizeof(compact), name ? name : "Other");
	morph_buf_printf(&label, "%s  %d", compact, count);
	if (!label.failed && tui_tree_row(&output, "", is_last, "",
		morph_buf_cstr(&label), "", 0, cli_list_columns(),
		ANSI_BOLD, ANSI_RESET) == 0)
		fputs(morph_buf_cstr(&output), stdout);
	morph_buf_cleanup(&output);
	morph_buf_cleanup(&label);
}

void cli_list_item(const char *ancestor, int is_last, const char *marker,
		   const char *name, const char *description, int name_width,
		   int columns)
{
	morph_buf_t output = {0};
	char compact[CLI_LIST_TEXT_MAX];
	char detail[CLI_LIST_TEXT_MAX];

	if (morph_buf_init(&output, 256) != 0)
		return;
	cli_list_compact_text(compact, sizeof(compact), name);
	cli_list_compact_text(detail, sizeof(detail), description);
	if (tui_tree_row(&output, ancestor ? ancestor : "", is_last,
			 marker ? marker : "", compact, detail, name_width,
			 columns, ANSI_BOLD, ANSI_RESET) == 0)
		fputs(morph_buf_cstr(&output), stdout);
	morph_buf_cleanup(&output);
}

void cli_list_row(const char *id, const char *name, const char *metadata,
		  int is_current, int is_last, int columns)
{
	const char *branch = tui_tree_branch(is_last, 0);
	const char *continuation = tui_tree_branch(is_last, 1);
	char compact[CLI_LIST_TEXT_MAX];
	char safe_id[CLI_LIST_TEXT_MAX];
	char safe_metadata[CLI_LIST_TEXT_MAX];
	int id_width;
	int metadata_width;
	int name_width;
	int printed_width;

	if (!id)
		id = "";
	if (!name)
		name = "";
	if (!metadata)
		metadata = "";
	(void)cli_list_compact_text(compact, sizeof(compact), name);
	(void)cli_list_compact_text(safe_id, sizeof(safe_id), id);
	(void)cli_list_compact_text(safe_metadata, sizeof(safe_metadata),
		metadata);
	id = safe_id;
	metadata = safe_metadata;
	id_width = (int)utf8_display_width(id);
	metadata_width = (int)utf8_display_width(metadata);
	name_width = columns - 10 - id_width - metadata_width;
	if (name_width > 52)
		name_width = 52;
	printf("  " ANSI_DIM "%s" ANSI_RESET "%s%s%s "
	       ANSI_DIM "%s" ANSI_RESET "  ",
	       branch,
	       is_current ? ANSI_GREEN : ANSI_DIM,
	       is_current ? "●" : "○", ANSI_RESET, id);
	if (name_width >= 12) {
		cli_list_print_clipped(compact, name_width);
		printed_width = (int)utf8_display_width(compact);
		if (printed_width > name_width)
			printed_width = name_width;
		for (int i = printed_width; i < name_width; i++)
			putchar(' ');
		printf(ANSI_DIM "  %s" ANSI_RESET "\n", metadata);
		return;
	}
	name_width = columns - 9 - id_width;
	cli_list_print_clipped(compact, name_width);
	putchar('\n');
	printf("  " ANSI_DIM "%s" ANSI_RESET "   " ANSI_DIM
	       "%s%s" ANSI_RESET "\n", continuation,
	       tui_tree_branch(1, 0), metadata);
}

void cli_list_value_field(const char *label, const char *value, int is_last,
			  int label_width, int columns)
{
	char safe_label[CLI_LIST_TEXT_MAX];
	int used;

	if (!label)
		label = "";
	if (!value)
		value = "";
	(void)cli_list_compact_text(safe_label, sizeof(safe_label), label);
	label = safe_label;
	cli_tree_prefix(NULL, 0, is_last, 0);
	printf(ANSI_DIM "%s" ANSI_RESET, label);
	used = (int)utf8_display_width(label);
	for (int i = used; i < label_width; i++)
		putchar(' ');
	putchar(' ');
	cli_list_print_clipped(value, columns - label_width - 5);
	putchar('\n');
}

static const char *cli_list_wrap_end(const char *text, int width)
{
	const char *end;
	const char *space = NULL;
	const char *p;

	end = utf8_advance_display_width(text, (size_t)width);
	if (!*end)
		return end;
	for (p = text; p < end; p++) {
		if (*p == ' ')
			space = p;
	}
	return space && space > text ? space : end;
}

void cli_list_text_field(const char *label, const char *value, int is_last,
			 int columns)
{
	char compact[CLI_LIST_TEXT_MAX];
	char safe_label[CLI_LIST_TEXT_MAX];
	const char *line;
	const char *end;
	int width = columns - 8;

	(void)cli_list_compact_text(safe_label, sizeof(safe_label),
		label ? label : "");
	cli_tree_prefix(NULL, 0, is_last, 0);
	printf(ANSI_DIM "%s" ANSI_RESET "\n", safe_label);
	(void)cli_list_compact_text(compact, sizeof(compact), value);
	line = compact;
	if (width < 10)
		width = 10;
	while (*line) {
		end = cli_list_wrap_end(line, width);
		cli_tree_prefix(&is_last, 1, !end[0], 0);
		printf("%.*s\n",
		       (int)(end - line), line);
		line = end;
		while (*line == ' ')
			line++;
	}
}

static int cli_json_count(const cJSON *item)
{
	return cJSON_IsArray(item) || cJSON_IsObject(item) ?
		cJSON_GetArraySize(item) : 0;
}

static void cli_json_prefix(const int *ancestors_last, int depth, int is_last)
{
	cli_tree_prefix(ancestors_last, depth, is_last, 0);
}

static void cli_json_node(const cJSON *item, const char *label, int depth,
			  int is_last, int *ancestors_last, int columns)
{
	char safe_label[CLI_LIST_TEXT_MAX];
	cJSON *child;
	int count;
	int shown;
	int index = 0;
	int prefix_width = 4 + depth * 2;
	char *scalar;

	(void)cli_list_compact_text(safe_label, sizeof(safe_label),
		label ? label : "item");
	label = safe_label;
	cli_json_prefix(ancestors_last, depth, is_last);
	printf(ANSI_DIM "%s" ANSI_RESET, label);
	if (!cJSON_IsArray(item) && !cJSON_IsObject(item)) {
		fputs(": ", stdout);
		scalar = cJSON_IsString(item) ?
			strdup(item->valuestring) : cJSON_PrintUnformatted(item);
		cli_list_print_clipped(scalar, columns - prefix_width -
				       (int)utf8_display_width(label) - 2);
		free(scalar);
		putchar('\n');
		return;
	}
	count = cli_json_count(item);
	if (count == 0) {
		printf(": %s\n", cJSON_IsArray(item) ? "[]" : "{}");
		return;
	}
	putchar('\n');
	if (depth + 1 >= CLI_LIST_DEPTH_MAX)
		return;
	ancestors_last[depth] = is_last;
	shown = count < CLI_LIST_ITEMS_MAX ? count : CLI_LIST_ITEMS_MAX;
	cJSON_ArrayForEach(child, item) {
		char index_label[32];
		const char *child_label;

		if (index >= shown)
			break;
		if (cJSON_IsArray(item)) {
			snprintf(index_label, sizeof(index_label), "[%d]", index);
			child_label = index_label;
		} else {
			child_label = child->string ? child->string : "item";
		}
		cli_json_node(child, child_label, depth + 1,
			      index == shown - 1 && shown == count,
			      ancestors_last, columns);
		index++;
	}
	if (shown < count) {
		cli_json_prefix(ancestors_last, depth + 1, 1);
		printf(ANSI_DIM "… %d more" ANSI_RESET "\n", count - shown);
	}
}

void cli_list_json_field(const char *label, const char *json, int is_last,
			 int columns)
{
	cJSON *root = json ? cJSON_Parse(json) : NULL;
	char safe_label[CLI_LIST_TEXT_MAX];
	int ancestors_last[CLI_LIST_DEPTH_MAX] = {0};

	(void)cli_list_compact_text(safe_label, sizeof(safe_label),
		label ? label : "Schema");
	cli_tree_prefix(NULL, 0, is_last, 0);
	printf(ANSI_DIM "%s" ANSI_RESET "\n", safe_label);
	if (!root)
		return;
	ancestors_last[0] = is_last;
	if (cJSON_IsArray(root) || cJSON_IsObject(root)) {
		cJSON *child;
		int count = cli_json_count(root);
		int index = 0;

		cJSON_ArrayForEach(child, root) {
			const char *child_label = cJSON_IsArray(root) ? "item" :
				(child->string ? child->string : "item");

			cli_json_node(child, child_label, 1,
				      index == count - 1, ancestors_last,
				      columns);
			index++;
		}
	} else {
		cli_json_node(root, "value", 1, 1, ancestors_last, columns);
	}
	cJSON_Delete(root);
}
