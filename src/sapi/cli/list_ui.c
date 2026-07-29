#include "sapi/cli/internal.h"
#include "sapi/cli/list_ui.h"

#include <sys/ioctl.h>

#define CLI_LIST_DEFAULT_COLUMNS 100
#define CLI_LIST_MIN_COLUMNS 40
#define CLI_LIST_TEXT_MAX 8192
#define CLI_LIST_DEPTH_MAX 5
#define CLI_LIST_ITEMS_MAX 12

int cli_list_columns(void)
{
	struct winsize ws;
	const char *configured;
	char *end;
	long columns;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col < CLI_LIST_MIN_COLUMNS ?
			CLI_LIST_MIN_COLUMNS : ws.ws_col;
	configured = getenv("COLUMNS");
	if (!configured || !configured[0])
		return CLI_LIST_DEFAULT_COLUMNS;
	errno = 0;
	columns = strtol(configured, &end, 10);
	if (errno != 0 || *end != '\0' || columns <= 0 ||
	    columns > INT_MAX)
		return CLI_LIST_DEFAULT_COLUMNS;
	return columns < CLI_LIST_MIN_COLUMNS ?
		CLI_LIST_MIN_COLUMNS : (int)columns;
}

size_t cli_list_compact_text(char *dst, size_t dst_cap, const char *src)
{
	size_t in = 0;
	size_t out = 0;
	size_t src_len;
	int pending_space = 0;

	if (!dst || dst_cap == 0)
		return 0;
	dst[0] = '\0';
	if (!src)
		return 0;
	src_len = strlen(src);
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
	printf("  " ANSI_DIM "%s" ANSI_RESET " " ANSI_BOLD "%s"
	       ANSI_RESET ANSI_DIM "  %d" ANSI_RESET "\n",
	       is_last ? "└" : "├", name ? name : "Other", count);
}

void cli_list_item(const char *ancestor, int is_last, const char *marker,
		   const char *name, const char *description, int name_width,
		   int columns)
{
	int prefix_width;
	int marker_width;
	int item_width;
	int description_width;
	int one_line;

	if (!ancestor)
		ancestor = "";
	if (!marker)
		marker = "";
	if (!name)
		name = "";
	prefix_width = 2 + (int)utf8_display_width_ansi(ancestor) + 2;
	marker_width = marker[0] ?
		(int)utf8_display_width_ansi(marker) + 1 : 0;
	item_width = marker_width + (int)utf8_display_width(name);
	one_line = description && description[0] &&
		prefix_width + item_width + 3 + 18 <= columns;
	printf("  %s" ANSI_DIM "%s" ANSI_RESET " ", ancestor,
	       is_last ? "└" : "├");
	if (marker[0])
		printf("%s ", marker);
	printf(ANSI_BOLD "%s" ANSI_RESET, name);
	if (one_line) {
		int target = name_width + marker_width;

		for (int i = item_width; i < target; i++)
			putchar(' ');
		fputs(ANSI_DIM "  " ANSI_RESET, stdout);
		description_width = columns - prefix_width - target - 2;
		cli_list_print_clipped(description, description_width);
		putchar('\n');
		return;
	}
	putchar('\n');
	if (!description || !description[0])
		return;
	printf("  %s%s  ", ancestor, is_last ? "  " : "│ ");
	description_width = columns - prefix_width - 2;
	cli_list_print_clipped(description, description_width);
	putchar('\n');
}

void cli_list_value_field(const char *label, const char *value, int is_last,
			  int label_width, int columns)
{
	int used;

	if (!label)
		label = "";
	if (!value)
		value = "";
	printf("  " ANSI_DIM "%s" ANSI_RESET " " ANSI_DIM "%s"
	       ANSI_RESET, is_last ? "└" : "├", label);
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
	const char *line;
	const char *end;
	int width = columns - 8;

	printf("  " ANSI_DIM "%s" ANSI_RESET " " ANSI_DIM "%s"
	       ANSI_RESET "\n", is_last ? "└" : "├",
	       label ? label : "");
	(void)cli_list_compact_text(compact, sizeof(compact), value);
	line = compact;
	if (width < 10)
		width = 10;
	while (*line) {
		end = cli_list_wrap_end(line, width);
		printf("  %s" ANSI_DIM "%s" ANSI_RESET " %.*s\n",
		       is_last ? "  " : "│ ", end[0] ? "├" : "└",
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
	printf("  ");
	for (int i = 0; i < depth; i++)
		printf("%s", ancestors_last[i] ? "  " : "│ ");
	printf(ANSI_DIM "%s" ANSI_RESET " ", is_last ? "└" : "├");
}

static void cli_json_node(const cJSON *item, const char *label, int depth,
			  int is_last, int *ancestors_last, int columns)
{
	cJSON *child;
	int count;
	int shown;
	int index = 0;
	int prefix_width = 4 + depth * 2;
	char *scalar;

	cli_json_prefix(ancestors_last, depth, is_last);
	printf(ANSI_DIM "%s" ANSI_RESET, label ? label : "item");
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
	int ancestors_last[CLI_LIST_DEPTH_MAX] = {0};

	printf("  " ANSI_DIM "%s" ANSI_RESET " " ANSI_DIM "%s"
	       ANSI_RESET "\n", is_last ? "└" : "├",
	       label ? label : "Schema");
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
