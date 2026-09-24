#include "sapi/cli/internal.h"
#include "sapi/cli/list_ui.h"
#include "sapi/cli/shell_style.h"
#include "agent/patch.h"
#include <sys/ioctl.h>

struct transcript_tool {
	const char *id;
	const char *name;
	const char *label;
	const char *args;
	const char *subject;
	const char *result;
	const char *error;
	const char *summary;
	const char *patch_preview;
	morph_buf_t stream;
	struct utf8_terminal_sanitizer sanitizer;
	int state;
	double started;
	double ended;
};

struct cli_transcript {
	struct arena *arena;
	morph_array_t tools;
	int finished;
	int allocation_failed;
	int view_follow;
	size_t view_offset;
	morph_buf_t deferred;
	morph_buf_t previous_frame;
	int frame_rows;
	int frame_columns;
};

static double transcript_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int transcript_columns(void)
{
	struct winsize size = {0};

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col)
		return size.ws_col;
	return cli_list_columns();
}

static const char *transcript_workdir(struct cli_context *ctx)
{
	if (!ctx)
		return NULL;
	if (ctx->workdir[0])
		return ctx->workdir;
	return runtime_workdir_get(ctx->runtime);
}

static const char *json_string(const cJSON *data, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, key);

	return cJSON_IsString(item) ? item->valuestring : "";
}

static const char *save_text(struct cli_transcript *tr, const char *text)
{
	char *copy = arena_strdup(tr->arena, text ? text : "");

	if (!copy)
		tr->allocation_failed = 1;
	return copy ? copy : "";
}

static int transcript_init(struct cli_context *ctx)
{
	struct cli_transcript *tr = calloc(1, sizeof(*tr));
	struct transcript_tool tool;

	if (!tr)
		MORPH_RETURN(-ENOMEM);
	tr->arena = arena_create(BUFSIZ);
	if (!tr->arena) {
		free(tr);
		MORPH_RETURN(-ENOMEM);
	}
	/* Arena-backed arrays have fixed capacity; event/tool counts are unbounded. */
	if (morph_array_init(&tr->tools,
		MORPH_ARRAY_INIT_CAP, sizeof(tool)) != 0) {
		morph_array_cleanup(&tr->tools);
		arena_destroy(tr->arena);
		free(tr);
		MORPH_RETURN(-ENOMEM);
	}
	ctx->transcript = tr;
	if (morph_buf_init(&tr->deferred, BUFSIZ) != 0 ||
	    morph_buf_init(&tr->previous_frame, BUFSIZ) != 0) {
		cli_transcript_reset(ctx);
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

void cli_transcript_reset(struct cli_context *ctx)
{
	struct cli_transcript *tr = ctx->transcript;

	if (!tr)
		return;
	morph_array_cleanup(&tr->tools);
	morph_buf_cleanup(&tr->deferred);
	morph_buf_cleanup(&tr->previous_frame);
	arena_destroy(tr->arena);
	free(tr);
	ctx->transcript = NULL;
}

int cli_transcript_capture_begin(struct cli_context *ctx)
{
	if (!ctx->transcript)
		MORPH_RETURN(-EINVAL);
	return cli_command_capture_styled_begin(&ctx->transcript->deferred);
}


static struct transcript_tool *find_tool(struct cli_transcript *tr,
					 const cJSON *data)
{
	const char *id = json_string(data, "tool_call_id");
	const char *name = json_string(data, "tool");

	for (size_t i = tr->tools.nelts; i > 0; i--) {
		struct transcript_tool *tool = morph_array_get(&tr->tools, i - 1);

		if (id[0] ? strcmp(id, tool->id) == 0 :
		    (!name[0] || strcmp(name, tool->name) == 0))
			return tool;
	}
	return NULL;
}

static void append_clipped_text(morph_buf_t *buf, const char *text, int width)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	const char *end;

	if (!safe || width < 1) {
		free(safe);
		return;
	}
	end = utf8_advance_display_width(safe, (size_t)width);
	if (*end && width > 3) {
		end = utf8_advance_display_width(safe, (size_t)(width - 3));
		(void)morph_buf_append(buf, safe, (size_t)(end - safe));
		(void)morph_buf_puts(buf, "...");
	} else {
		(void)morph_buf_append(buf, safe, (size_t)(end - safe));
	}
	free(safe);
}

static void tool_row(const struct transcript_tool *tool, morph_buf_t *row,
		     int styled)
{
	morph_buf_t action;
	morph_buf_t subject;
	morph_buf_t meta;
	int columns = transcript_columns() - (int)CLI_CONTENT_RIGHT_PADDING;
	int name_width = (int)utf8_display_width(tool->label);
	int is_skill = strcmp(tool->name, "activate_skill") == 0;
	const char *target = tool->subject;
	char cwd[PATH_MAX];
	double elapsed = (tool->state ? tool->ended : transcript_now()) - tool->started;

	if ((strcmp(tool->name, "exec") == 0 ||
	     strcmp(tool->name, "process") == 0) &&
	    getcwd(cwd, sizeof(cwd)))
		target = cli_shell_summary(target, cwd);
	/* Shorten only a whole path prefix, never shell source or sibling paths. */
	if (!is_skill && strcmp(tool->name, "exec") != 0 &&
	    target[0] == '/' && getcwd(cwd, sizeof(cwd))) {
		size_t len = strlen(cwd);

		if (strncmp(target, cwd, len) == 0 &&
		    (target[len] == '/' || target[len] == '\0'))
			target = target[len] ? target + len + 1 : ".";
	}
	if (morph_buf_init(&action, 64) != 0)
		return;
	if (morph_buf_init(&subject, 128) != 0) {
		morph_buf_cleanup(&action);
		return;
	}
	if (morph_buf_init(&meta, 64) != 0) {
		morph_buf_cleanup(&action);
		morph_buf_cleanup(&subject);
		return;
	}
	if (tool->summary[0] && strcmp(tool->summary, "exit 0") != 0)
		(void)morph_buf_printf(&meta, "%s  ", tool->summary);
	(void)morph_buf_printf(&meta, "%.1fs", elapsed);
	append_clipped_text(&action, tool->label, name_width);
	append_clipped_text(&subject, target,
		columns - 2 - name_width - 4 - (is_skill ? 2 : 0) -
		(int)utf8_display_width(meta.data));
	(void)morph_buf_printf(row, "%s%s%s ", styled ? ANSI_DIM : "",
		action.data, styled ? ANSI_RESET : "");
	if (is_skill)
		(void)morph_buf_printf(row, "(%s%s%s)",
			styled ? ANSI_CYAN : "", subject.data,
			styled ? ANSI_RESET : "");
	else if (styled && (strcmp(tool->name, "exec") == 0 ||
		       strcmp(tool->name, "process") == 0))
		(void)cli_shell_style(row, subject.data, 0);
	else
		(void)morph_buf_puts(row, subject.data);
	(void)morph_buf_printf(row, " · %s%s%s", styled ? ANSI_DIM : "",
		meta.data, styled ? ANSI_RESET : "");
	morph_buf_cleanup(&meta);
	morph_buf_cleanup(&subject);
	morph_buf_cleanup(&action);
}

int cli_transcript_live_text(struct cli_context *ctx, morph_buf_t *text, int styled)
{
	struct cli_transcript *tr = ctx->transcript;
	struct transcript_tool *tool;
	struct transcript_tool *latest = NULL;

	if (!tr)
		return 0;
	morph_array_foreach(tool, &tr->tools, struct transcript_tool) {
		if (!tool->state) {
			latest = tool;
		}
	}
	if (!latest)
		return 0;
	tool_row(latest, text, styled);
	return 1;
}

static void restore_live(struct cli_context *ctx)
{
	morph_buf_t live;

	if (morph_buf_init(&live, 128) != 0)
		return;
	if (cli_transcript_live_text(ctx, &live, 0))
		cli_terminal_live_set(ctx, morph_buf_cstr(&live));
	morph_buf_cleanup(&live);
}

static void print_inline(const char *text, int columns)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	const char *end;

	if (!safe)
		return;
	end = utf8_advance_display_width(safe, (size_t)(columns > 0 ? columns : 1));
	if (*end && columns > 3)
		end = utf8_advance_display_width(safe, (size_t)(columns - 3));
	printf("%.*s%s", (int)(end - safe), safe, *end ? "..." : "");
	free(safe);
}

static void print_tool(const struct transcript_tool *tool)
{
	morph_buf_t line;
	const char *color = tool->state < 0 ? ANSI_RED :
		(tool->state ? ANSI_GREEN : ANSI_YELLOW);

	if (morph_buf_init(&line, 128) != 0)
		return;
	tool_row(tool, &line, 1);
	printf("%s%s" ANSI_RESET " %s\n", color,
		tool->state < 0 ? "⊗" : (tool->state ? "◯" : "◉"),
		morph_buf_cstr(&line));
	if (tool->state < 0) {
		printf("  " ANSI_RED);
		print_inline(tool->error[0] ? tool->error : "Tool failed",
			transcript_columns() - 8);
		printf(ANSI_RESET "\n");
	}
	morph_buf_cleanup(&line);
}

/* Details are complete terminal-safe text, including multiline strings inside
 * JSON envelopes. No tree depth, item count, or byte truncation is applied. */
static void print_lines(const char *text)
{
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	const char *line = safe;
	int width = transcript_columns() - 5;

	if (!safe)
		return;
	if (width < 1)
		width = 1;
	while (*line) {
		const char *end = strchr(line, '\n');
		const char *wrap = utf8_advance_display_width(line, (size_t)width);
		const char *style = line[0] == '+' ? ANSI_GREEN :
			(line[0] == '-' ? ANSI_RED : ANSI_DIM);

		if (!end || wrap < end) {
			end = wrap;
			if (*wrap && *wrap != '\n') {
				const char *space = wrap;

				while (space > line && space[-1] != ' ')
					space--;
				if (space > line)
					end = space;
			}
		}
		if (end == line && *line != '\n')
			end = line + utf8codepointcalcsize(line);
		printf("    %s%.*s" ANSI_RESET "\n", style, (int)(end - line), line);
		line = *end == '\n' ? end + 1 : end;
	}
	free(safe);
}

static void print_value(const cJSON *value, int depth)
{
	cJSON *child;
	char *json;

	if (cJSON_IsString(value)) {
		print_lines(value->valuestring);
		return;
	}
	if (depth < 32 && (cJSON_IsObject(value) || cJSON_IsArray(value))) {
		cJSON_ArrayForEach(child, value) {
			if (child->string) {
				printf("    " ANSI_DIM);
				print_inline(child->string, transcript_columns() - 10);
				printf(":" ANSI_RESET "\n");
			}
			print_value(child, depth + 1);
		}
		return;
	}
	json = cJSON_Print(value);
	if (json) {
		print_lines(json);
		free(json);
	}
}

static void print_payload(const char *text)
{
	cJSON *json = cJSON_Parse(text);

	if (json) {
		print_value(json, 0);
		cJSON_Delete(json);
	} else {
		print_lines(text);
	}
}

static void print_details(const struct transcript_tool *tool, int args,
			  const char *workdir)
{
	if (args) {
		if (strcmp(tool->name, "apply_patch") == 0) {
			cJSON *json = cJSON_Parse(tool->args);

			if (tool->patch_preview[0])
				cli_presentation_patch_diff(NULL,
					tool->patch_preview);
			else
				cli_presentation_patch_diff(workdir,
					json_string(json, "input"));
			cJSON_Delete(json);
		} else if (strcmp(tool->name, "exec") == 0) {
			cJSON *json = cJSON_Parse(tool->args);
			morph_buf_t command;

			if (morph_buf_init(&command, 128) == 0) {
				(void)cli_shell_style(&command, json_string(json, "command"),
					transcript_columns() - 6);
				printf("    $ %s\n", morph_buf_cstr(&command));
				morph_buf_cleanup(&command);
			}
			cJSON_DeleteItemFromObjectCaseSensitive(json, "command");
			if (json && json->child)
				print_value(json, 0);
			cJSON_Delete(json);
		} else {
			print_payload(tool->args);
		}
	}
	if (tool->stream.len) {
		print_lines(morph_buf_cstr(&tool->stream));
	}
	if (tool->result[0]) {
		cJSON *json = cJSON_Parse(tool->result);
		const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");

		if (!cJSON_IsObject(data))
			data = json;
		if (strcmp(tool->name, "exec") == 0 &&
		    cJSON_IsString(cJSON_GetObjectItemCaseSensitive(data, "stdout"))) {
			const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(data, "exit_code");

			printf("\n");
			print_lines(json_string(data, "stdout"));
			print_lines(json_string(data, "stderr"));
			if (cJSON_IsNumber(exit_code))
				printf(ANSI_DIM "    exit %d\n" ANSI_RESET, exit_code->valueint);
			if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "timed_out")))
				print_lines("Command timed out");
			if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "stdout_truncated")) ||
			    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "stderr_truncated")))
				print_lines("Output truncated by the tool");
		} else {
			print_payload(tool->result);
		}
		cJSON_Delete(json);
	}
}

static void present_frame(struct cli_transcript *tr, morph_buf_t *frame,
			  int rows, int columns)
{
	const char *current = morph_buf_cstr(frame);
	const char *previous = morph_buf_cstr(&tr->previous_frame);
	int resized = rows != tr->frame_rows || columns != tr->frame_columns;
	int writing = 0;
	int row = 1;

	while (*current || *previous) {
		size_t len = strcspn(current, "\n");
		size_t old_len = strcspn(previous, "\n");

		if (resized || len != old_len || memcmp(current, previous, len) != 0) {
			if (!writing) {
				fprintf(stdout, "\033[?2026h");
				if (resized)
					fprintf(stdout, "\033[H\033[2J");
				writing = 1;
			}
			fprintf(stdout, "\033[%d;1H", row);
			printf("%.*s" ANSI_RESET, (int)len, current);
			fprintf(stdout, "\033[K");
		}
		current += len + (current[len] == '\n');
		previous += old_len + (previous[old_len] == '\n');
		row++;
	}
	if (writing) {
		fprintf(stdout, "\033[?2026l");
		fflush(stdout);
	}
	morph_buf_reset(&tr->previous_frame);
	(void)morph_buf_puts(&tr->previous_frame, morph_buf_cstr(frame));
	tr->frame_rows = rows;
	tr->frame_columns = columns;
}

void cli_transcript_view_render(struct cli_context *ctx, int scroll)
{
	struct cli_transcript *tr = ctx->transcript;
	struct transcript_tool *tool;
	struct winsize size = {0};
	morph_buf_t page;
	morph_buf_t frame;
	const char *line;
	size_t lines = 0;
	size_t first;
	int rows = 24;
	int height;

	if (!ctx->details_visible || !tr || morph_buf_init(&page, BUFSIZ) != 0)
		return;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row)
		rows = size.ws_row;
	height = rows > 5 ? rows - 4 : 1;
	if (cli_command_capture_styled_begin(&page) != 0) {
		morph_buf_cleanup(&page);
		return;
	}
	morph_array_foreach(tool, &tr->tools, struct transcript_tool) {
		print_tool(tool);
		print_details(tool, 1, transcript_workdir(ctx));
		printf("\n");
	}
	if (!tr->tools.nelts)
		printf("  No tool calls yet.\n");
	cli_command_capture_end();
	for (const char *p = morph_buf_cstr(&page); *p; p++)
		lines += *p == '\n';
	if (scroll == INT_MAX)
		tr->view_follow = 1;
	else if (scroll == INT_MIN) {
		tr->view_follow = 0;
		tr->view_offset = 0;
	} else if (scroll) {
		size_t step = (size_t)(scroll < 0 ? -scroll : scroll);

		tr->view_follow = 0;
		if (scroll < 0)
			tr->view_offset = step > tr->view_offset ? 0 :
				tr->view_offset - step;
		else
			tr->view_offset += step;
	}
	first = lines > (size_t)height ? lines - (size_t)height : 0;
	if (tr->view_follow || tr->view_offset > first)
		tr->view_offset = first;
	if (morph_buf_init(&frame, BUFSIZ) != 0) {
		morph_buf_cleanup(&page);
		return;
	}
	if (cli_command_capture_styled_begin(&frame) != 0) {
		morph_buf_cleanup(&frame);
		morph_buf_cleanup(&page);
		return;
	}
	printf(ANSI_BOLD "  Tool details" ANSI_RESET
	       ANSI_DIM "  · %zu calls\n\n" ANSI_RESET, tr->tools.nelts);
	line = morph_buf_cstr(&page);
	for (size_t i = 0; i < tr->view_offset && *line; i++) {
		const char *next = strchr(line, '\n');

		line = next ? next + 1 : line + strlen(line);
	}
	for (int i = 0; i < height; i++) {
		const char *next = strchr(line, '\n');
		size_t len = next ? (size_t)(next - line) : strlen(line);
		const char *style = strncmp(line, "    +", 5) == 0 ? ANSI_GREEN :
			(strncmp(line, "    -", 5) == 0 ? ANSI_RED : ANSI_DIM);

		printf("%s%.*s" ANSI_RESET "\n", style, (int)len, line);
		line = next ? next + 1 : line + len;
	}
	printf("\n");
	print_inline("  ctrl+o / esc back · PgUp/PgDn scroll · End follow",
		     transcript_columns() - 1);
	cli_command_capture_end();
	present_frame(tr, &frame, rows, transcript_columns());
	morph_buf_cleanup(&frame);
	morph_buf_cleanup(&page);
}

void cli_transcript_view_suspend(struct cli_context *ctx)
{
	if (!ctx || !ctx->details_visible)
		return;
	fprintf(stdout, "\033[?1006l\033[?1000l\033[?1049l\033[?25h");
	if (ctx->transcript) {
		morph_buf_t *pending = &ctx->transcript->deferred;

		(void)fwrite(morph_buf_cstr(pending), 1, pending->len, stdout);
		morph_buf_reset(pending);
	}
	fflush(stdout);
	ctx->details_visible = 0;
}

void cli_transcript_view_resume(struct cli_context *ctx)
{
	if (!ctx || !ctx->details_open || ctx->details_visible)
		return;
	fprintf(stdout, "\033[?1049h\033[?25l\033[?1000h\033[?1006h");
	morph_buf_reset(&ctx->transcript->previous_frame);
	ctx->details_visible = 1;
	cli_transcript_view_render(ctx, 0);
}

void cli_transcript_toggle(struct cli_context *ctx)
{
	if (!ctx || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (ctx->details_open) {
		cli_transcript_view_suspend(ctx);
		ctx->details_open = 0;
	} else {
		if (!ctx->transcript && transcript_init(ctx) != 0)
			return;
		ctx->transcript->view_follow = 1;
		ctx->details_open = 1;
		cli_transcript_view_resume(ctx);
	}
}

void cli_transcript_finish(struct cli_context *ctx)
{
	struct cli_transcript *tr = ctx->transcript;
	struct transcript_tool *tool;

	if (!tr || tr->finished)
		return;
	morph_array_foreach(tool, &tr->tools, struct transcript_tool) {
		if (!tool->state) {
			tool->state = -1;
			tool->ended = transcript_now();
			tool->error = "Interrupted before a result was received";
			print_tool(tool);
		}
	}
	tr->finished = 1;
}

static const char *process_action_label(const char *action)
{
	if (strcmp(action, "poll") == 0)
		return "wait";
	if (strcmp(action, "write") == 0)
		return "send input";
	if (strcmp(action, "interrupt") == 0)
		return "interrupt";
	if (strcmp(action, "kill") == 0)
		return "terminate";
	return "process";
}

static const char *find_process_command(struct cli_transcript *tr,
					const char *session_id)
{
	for (size_t i = tr->tools.nelts; i > 0; i--) {
		struct transcript_tool *candidate =
			morph_array_get(&tr->tools, i - 1);
		cJSON *json;
		const cJSON *data;
		const char *candidate_id;

		if (strcmp(candidate->name, "exec") != 0 ||
		    !candidate->result[0])
			continue;
		json = cJSON_Parse(candidate->result);
		data = cJSON_GetObjectItemCaseSensitive(json, "data");
		if (!cJSON_IsObject(data))
			data = json;
		candidate_id = json_string(data, "session_id");
		if (strcmp(candidate_id, session_id) == 0) {
			cJSON_Delete(json);
			return candidate->subject;
		}
		cJSON_Delete(json);
	}
	return "";
}

static int process_action_is(const struct transcript_tool *tool,
			     const char *action)
{
	cJSON *args;
	int matches;

	if (strcmp(tool->name, "process") != 0)
		return 0;
	args = cJSON_Parse(tool->args);
	matches = strcmp(json_string(args, "action"), action) == 0;
	cJSON_Delete(args);
	return matches;
}

static int process_poll_is_running(const struct transcript_tool *tool)
{
	cJSON *result;
	const cJSON *data;
	int running;

	if (!process_action_is(tool, "poll"))
		return 0;
	result = cJSON_Parse(tool->result);
	data = cJSON_GetObjectItemCaseSensitive(result, "data");
	if (!cJSON_IsObject(data))
		data = result;
	running = strcmp(json_string(data, "status"), "running") == 0;
	cJSON_Delete(result);
	return running;
}

static int add_tool(struct cli_transcript *tr, const cJSON *data,
		    const char *workdir)
{
	const cJSON *args = cJSON_GetObjectItemCaseSensitive(data, "args");
	struct transcript_tool *tool = morph_array_push(&tr->tools);
	char *json;
	const char *subject = "";
	static const char *keys[] = {
		"command", "file_path", "dir_path", "path", "query", "pattern", "url"
	};

	if (!tool)
		MORPH_RETURN(-ENOMEM);
	memset(tool, 0, sizeof(*tool));
	tool->id = save_text(tr, json_string(data, "tool_call_id"));
	tool->name = save_text(tr, json_string(data, "tool"));
	tool->label = tool->name;
	json = args ? cJSON_PrintUnformatted(args) : NULL;
	tool->args = save_text(tr, json ? json : "{}");
	free(json);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		subject = json_string(args, keys[i]);
		if (subject[0])
			break;
	}
	tool->subject = save_text(tr, subject);
	tool->result = "";
	tool->error = "";
	tool->summary = "";
	tool->patch_preview = "";
	if (strcmp(tool->name, "activate_skill") == 0) {
		tool->label = "SKILL.md";
		tool->subject = save_text(tr, json_string(args, "name"));
	}
	if (strcmp(tool->name, "process") == 0) {
		const char *action = json_string(args, "action");
		const char *session_id = json_string(args, "session_id");
		const char *command = find_process_command(tr, session_id);
		morph_buf_t fallback;

		tool->label = save_text(tr, process_action_label(action));
		if (command[0]) {
			tool->subject = command;
		} else if (morph_buf_init(&fallback, 64) == 0) {
			(void)morph_buf_printf(&fallback, "session %s", session_id);
			tool->subject = save_text(tr, morph_buf_cstr(&fallback));
			morph_buf_cleanup(&fallback);
		}
	}
	if (strcmp(tool->name, "apply_patch") == 0) {
		const char *input = json_string(args, "input");
		const char *path = strstr(input, " File: ");
		morph_buf_t patch;
		morph_buf_t preview;
		char preview_error[256];
		size_t added = 0;
		size_t removed = 0;

		if (workdir && workdir[0] &&
		    morph_buf_init(&preview, 256) == 0) {
			if (patch_preview(workdir, input, &preview, preview_error,
				sizeof(preview_error)) == 0)
				tool->patch_preview = save_text(
					tr, morph_buf_cstr(&preview));
			morph_buf_cleanup(&preview);
		}
		if (morph_buf_init(&patch, 128) == 0) {
			if (path) {
				path += strlen(" File: ");
				(void)morph_buf_append(&patch, path, strcspn(path, "\r\n"));
				tool->subject = save_text(tr, morph_buf_cstr(&patch));
			}
			for (const char *p = input; *p; p++) {
				if (p == input || p[-1] == '\n') {
					added += *p == '+';
					removed += *p == '-';
				}
			}
			morph_buf_reset(&patch);
			(void)morph_buf_printf(&patch, "+%zu -%zu", added, removed);
			tool->summary = save_text(tr, morph_buf_cstr(&patch));
			morph_buf_cleanup(&patch);
		}
	}
	tool->started = transcript_now();
	utf8_terminal_sanitizer_init(&tool->sanitizer,
		UTF8_TERMINAL_TEXT_MULTILINE);
	if (morph_buf_init_arena(&tool->stream, tr->arena, 128) != 0)
		MORPH_RETURN(-ENOMEM);
	if (tr->allocation_failed)
		MORPH_RETURN(-ENOMEM);
	return 0;
}

static void summarize_result(struct cli_transcript *tr, struct transcript_tool *tool)
{
	cJSON *json = cJSON_Parse(tool->result);
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
	const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
	const cJSON *value;
	morph_buf_t summary;

	if (!json)
		return;
	if (!cJSON_IsObject(data))
		data = json;
	if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")) ||
	    cJSON_IsObject(error) || cJSON_IsString(error))
		tool->state = -1;
	if (cJSON_IsObject(error)) {
		const char *message = json_string(error, "message");

		if (message[0])
			tool->error = save_text(tr, message);
	} else if (cJSON_IsString(error)) {
		tool->error = save_text(tr, error->valuestring);
	}
	if (morph_buf_init(&summary, 64) != 0) {
		cJSON_Delete(json);
		return;
	}
	value = cJSON_GetObjectItemCaseSensitive(data, "duration_ms");
	if (strcmp(tool->name, "process") == 0 && cJSON_IsNumber(value) &&
	    value->valuedouble >= 0)
		tool->started = tool->ended - value->valuedouble / 1000.0;
	value = cJSON_GetObjectItemCaseSensitive(data, "exit_code");
	if (cJSON_IsNumber(value)) {
		(void)morph_buf_printf(&summary, "exit %d", value->valueint);
		if (value->valueint != 0) {
			tool->state = -1;
			tool->error = save_text(tr, json_string(data, "stderr"));
			if (!tool->error[0])
				tool->error = save_text(tr, morph_buf_cstr(&summary));
		}
	} else if (cJSON_IsString(value =
		cJSON_GetObjectItemCaseSensitive(data, "status"))) {
		(void)morph_buf_puts(&summary, value->valuestring);
	} else if (cJSON_IsNumber(value =
		cJSON_GetObjectItemCaseSensitive(data, "returned_lines"))) {
		(void)morph_buf_printf(&summary, "%d lines", value->valueint);
	} else if (cJSON_IsArray(value =
		cJSON_GetObjectItemCaseSensitive(data, "entries"))) {
		(void)morph_buf_printf(&summary, "%d entries", cJSON_GetArraySize(value));
	}
	if (summary.len)
		tool->summary = save_text(tr, morph_buf_cstr(&summary));
	morph_buf_cleanup(&summary);
	cJSON_Delete(json);
}

/* Return one for events fully presented here, zero for the usual presenter. */
int cli_transcript_event(struct cli_context *ctx, const struct morph_event *ev)
{
	struct cli_transcript *tr;
	struct transcript_tool *tool;
	const char *name = ev->name ? ev->name : "";
	int rc;

	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return 0;
	if (!ctx->transcript && transcript_init(ctx) != 0)
		MORPH_RETURN(-ENOMEM);
	tr = ctx->transcript;
	if (strcmp(name, "react.observation") == 0 &&
	    strcmp(json_string(ev->data, "tool"), "plan") != 0 &&
	    (tool = find_tool(tr, ev->data))) {
		if (!tool->result[0]) {
			tool->result = save_text(tr, json_string(ev->data, "text"));
			if (ctx->tool_details)
				print_payload(tool->result);
		}
		return 1;
	}
	if (ev->type != MORPH_EVENT_TOOL)
		return 0;
	if (strcmp(name, "tool.call") == 0) {
		rc = add_tool(tr, ev->data, transcript_workdir(ctx));
		if (rc != 0)
			MORPH_RETURN(rc);
	}
	tool = find_tool(tr, ev->data);
	if (!tool)
		return 0;
	if (strcmp(name, "tool.call") == 0) {
		int quiet_poll = !ctx->tool_details &&
			process_action_is(tool, "poll");

		cli_terminal_live_clear(ctx);
		cli_presentation_flush_stream(ctx);
		if (!quiet_poll &&
		    (ctx->tool_details || !isatty(STDOUT_FILENO)))
			print_tool(tool);
		if (ctx->tool_details)
			print_details(tool, 1, transcript_workdir(ctx));
		restore_live(ctx);
		return 1;
	}
	if (strcmp(name, "tool.running") == 0)
		return 1;
	if (strcmp(name, "tool.stream.delta") == 0) {
		const char *text = json_string(ev->data, "text");
		size_t offset = tool->stream.len;

		rc = utf8_terminal_sanitize_feed(&tool->sanitizer, &tool->stream,
			text, strlen(text), 0);
		if (rc != 0)
			MORPH_RETURN(rc);
		if (ctx->tool_details) {
			cli_terminal_live_clear(ctx);
			print_lines(morph_buf_cstr(&tool->stream) + offset);
			restore_live(ctx);
		}
		return 1;
	}
	if (strcmp(name, "tool.result") != 0 && strcmp(name, "tool.failed") != 0 &&
	    strcmp(name, "tool.cancelled") != 0)
		return 0;
	tool->state = strcmp(name, "tool.result") == 0 ? 1 : -1;
	tool->ended = transcript_now();
	tool->result = save_text(tr, json_string(ev->data, "result"));
	tool->error = save_text(tr, json_string(ev->data, "error"));
	(void)utf8_terminal_sanitize_feed(&tool->sanitizer, &tool->stream, NULL, 0, 1);
	summarize_result(tr, tool);
	cli_terminal_live_clear(ctx);
	if (ctx->tool_details || !process_poll_is_running(tool))
		print_tool(tool);
	if (ctx->tool_details) {
		print_details(tool, 0, transcript_workdir(ctx));
	} else if (tool->state > 0 &&
		   strcmp(tool->name, "apply_patch") == 0) {
		cJSON *args = cJSON_Parse(tool->args);

		if (tool->patch_preview[0])
			cli_presentation_patch_diff(NULL, tool->patch_preview);
		else
			cli_presentation_patch_diff(transcript_workdir(ctx),
				json_string(args, "input"));
		cJSON_Delete(args);
	}
	restore_live(ctx);
	/*
	 * The next react.thinking event is emitted only after tool result
	 * persistence and the next model round have been prepared. Keep a
	 * visible busy state during that gap so a completed tool does not look
	 * like the turn has stalled.
	 */
	{
		morph_buf_t live;

		if (morph_buf_init(&live, 128) == 0) {
			if (!cli_transcript_live_text(ctx, &live, 0))
				cli_terminal_live_set(ctx, "Thinking…");
			morph_buf_cleanup(&live);
		}
	}
	return 1;
}
