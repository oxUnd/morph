#include "sapi/cli/internal.h"

#define CLI_STREAM_NONE      0
#define CLI_STREAM_THOUGHT   1
#define CLI_STREAM_REASONING 2
#define CLI_STREAM_FINAL     3
#define CLI_EVENT_TEXT_MAX   2000
#define CLI_EVENT_ARGS_MAX   180
#define CLI_TREE_VALUE_MAX   160
#define CLI_TREE_DEPTH_MAX   6
#define CLI_TREE_ITEMS_MAX   12

static void presentation_clear_status(struct cli_context *ctx)
{
	if (!ctx || !ctx->status_visible)
		return;
	printf("\r\033[2K");
	fflush(stdout);
	ctx->status_visible = 0;
}

static void presentation_status(struct cli_context *ctx, const char *text)
{
	if (!ctx || !text || !text[0])
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("status: %s\n", text);
		fflush(stdout);
		return;
	}
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	presentation_clear_status(ctx);
	if (isatty(STDOUT_FILENO) && cli_color_enabled()) {
		printf("\r" ANSI_BOLD ANSI_CYAN "•" ANSI_RESET " "
		       ANSI_DIM "%s" ANSI_RESET, text);
		ctx->status_visible = 1;
	} else {
		printf("• %s\n", text);
	}
	fflush(stdout);
}

static const char *event_string(const struct morph_event *ev,
				const char *name)
{
	cJSON *item;

	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	item = cJSON_GetObjectItemCaseSensitive(ev->data, name);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int event_error_code(const struct morph_event *ev)
{
	cJSON *item;

	if (!ev || !cJSON_IsObject(ev->data))
		return 0;
	item = cJSON_GetObjectItemCaseSensitive(ev->data, "error_code");
	return cJSON_IsNumber(item) ? item->valueint : 0;
}

static char *event_args_json(const struct morph_event *ev)
{
	cJSON *args;

	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	args = cJSON_GetObjectItemCaseSensitive(ev->data, "args");
	if (!args)
		return NULL;
	return cJSON_PrintUnformatted(args);
}

static cJSON *event_item(const struct morph_event *ev, const char *name)
{
	if (!ev || !cJSON_IsObject(ev->data))
		return NULL;
	return cJSON_GetObjectItemCaseSensitive(ev->data, name);
}

static void print_tree_prefix(const int *ancestors_last, int depth,
			      int is_last)
{
	printf("  ");
	for (int i = 0; i < depth; i++)
		printf("%s", ancestors_last[i] ? "  " : "│ ");
	printf("%s ", is_last ? "└" : "├");
}

static void print_tree_scalar(const cJSON *item)
{
	char *value = NULL;

	if (cJSON_IsString(item)) {
		value = utf8_dup_clamped(item->valuestring,
					 CLI_TREE_VALUE_MAX);
		if (value) {
			for (char *p = value; *p; p++) {
				if (*p == '\n' || *p == '\r' || *p == '\t')
					*p = ' ';
			}
			printf("%s", value);
		}
		free(value);
		return;
	}
	value = cJSON_PrintUnformatted(item);
	if (value) {
		char *display = utf8_dup_clamped(value, CLI_TREE_VALUE_MAX);

		printf("%s", display ? display : value);
		free(display);
		free(value);
	}
}

static int json_child_count(const cJSON *item)
{
	return cJSON_IsArray(item) ? cJSON_GetArraySize(item) :
		(cJSON_IsObject(item) ? cJSON_GetArraySize(item) : 0);
}

static void print_json_tree_node(const cJSON *item, const char *label,
				 int depth, int is_last,
				 int *ancestors_last)
{
	int count;
	int shown;
	int index = 0;
	cJSON *child;

	print_tree_prefix(ancestors_last, depth, is_last);
	if (label && label[0])
		printf(ANSI_DIM "%s:" ANSI_RESET, label);
	if (!cJSON_IsArray(item) && !cJSON_IsObject(item)) {
		if (label && label[0])
			printf(" ");
		print_tree_scalar(item);
		printf("\n");
		return;
	}
	count = json_child_count(item);
	if (count == 0) {
		printf(" %s\n", cJSON_IsArray(item) ? "[]" : "{}");
		return;
	}
	printf("\n");
	ancestors_last[depth] = is_last;
	if (depth + 1 >= CLI_TREE_DEPTH_MAX) {
		print_tree_prefix(ancestors_last, depth + 1, 1);
		printf(ANSI_DIM "… nested data" ANSI_RESET "\n");
		return;
	}
	shown = count < CLI_TREE_ITEMS_MAX ? count : CLI_TREE_ITEMS_MAX;
	cJSON_ArrayForEach(child, item) {
		char array_label[32];
		const char *child_label;
		int child_last;

		if (index >= shown)
			break;
		if (cJSON_IsArray(item)) {
			snprintf(array_label, sizeof(array_label), "[%d]", index);
			child_label = array_label;
		} else {
			child_label = child->string ? child->string : "item";
		}
		child_last = index == shown - 1 && shown == count;
		print_json_tree_node(child, child_label, depth + 1,
				     child_last, ancestors_last);
		index++;
	}
	if (shown < count) {
		print_tree_prefix(ancestors_last, depth + 1, 1);
		printf(ANSI_DIM "… %d more" ANSI_RESET "\n", count - shown);
	}
}

static void print_json_tree_children(const cJSON *item)
{
	int ancestors_last[CLI_TREE_DEPTH_MAX] = {0};
	int count;
	int index = 0;
	cJSON *child;

	if (!cJSON_IsArray(item) && !cJSON_IsObject(item))
		return;
	count = json_child_count(item);
	cJSON_ArrayForEach(child, item) {
		char array_label[32];
		const char *label;
		int is_last;

		if (index >= CLI_TREE_ITEMS_MAX)
			break;
		if (cJSON_IsArray(item)) {
			snprintf(array_label, sizeof(array_label), "[%d]", index);
			label = array_label;
		} else {
			label = child->string ? child->string : "item";
		}
		is_last = index == count - 1 &&
			count <= CLI_TREE_ITEMS_MAX;
		print_json_tree_node(child, label, 0, is_last,
				     ancestors_last);
		index++;
	}
	if (count > CLI_TREE_ITEMS_MAX) {
		print_tree_prefix(ancestors_last, 0, 1);
		printf(ANSI_DIM "… %d more" ANSI_RESET "\n",
		       count - CLI_TREE_ITEMS_MAX);
	}
}

static void print_indented(const char *prefix, const char *content)
{
	char *display;
	const char *line;

	if (!content || !content[0])
		return;
	display = utf8_dup_clamped(content, CLI_EVENT_TEXT_MAX);
	if (!display)
		return;
	line = display;
	while (line) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);

		printf("%s%.*s\n", prefix, (int)len, line);
		line = end ? end + 1 : NULL;
	}
	free(display);
}

static void print_plain_labeled(const char *label, const char *content)
{
	char *display;

	if (!content || !content[0])
		return;
	display = utf8_dup_clamped(content, CLI_EVENT_TEXT_MAX);
	if (!display)
		return;
	if (strchr(display, '\n'))
		printf("%s:\n%s\n", label, display);
	else
		printf("%s: %s\n", label, display);
	free(display);
}

static void presentation_print_stream(struct cli_context *ctx)
{
	const char *content;
	const char *label;
	int already_streamed;
	int reasoning_streamed;

	if (!ctx || ctx->event_stream_kind == CLI_STREAM_NONE)
		return;
	reasoning_streamed =
		ctx->event_stream_kind == CLI_STREAM_REASONING &&
		ctx->event_stream_visible;
	already_streamed = ctx->markdown_stream_visible &&
		ctx->markdown_stream_kind == ctx->event_stream_kind;
	if (already_streamed)
		cli_markdown_stream_reset(ctx, 1);
	presentation_clear_status(ctx);
	content = morph_buf_cstr(&ctx->event_stream);
	if (!content || !content[0])
		goto reset;
	if (reasoning_streamed) {
		size_t len = strlen(content);

		if (len > 0 && content[len - 1] != '\n')
			printf("\n");
		goto reset;
	}
	if (already_streamed)
		goto reset;
	label = ctx->event_stream_kind == CLI_STREAM_REASONING ?
		"reasoning" : "thought";
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		print_plain_labeled(label, content);
	} else {
		printf("\n" ANSI_BOLD ANSI_CYAN "•" ANSI_RESET " ");
		print_indented("", content);
	}
reset:
	morph_buf_reset(&ctx->event_stream);
	ctx->event_stream_kind = CLI_STREAM_NONE;
	ctx->event_stream_has_delta = 0;
	ctx->event_stream_complete = 0;
	ctx->event_stream_visible = 0;
}

static void presentation_discard_stream(struct cli_context *ctx)
{
	if (!ctx)
		return;
	cli_markdown_stream_reset(ctx, 1);
	morph_buf_reset(&ctx->event_stream);
	ctx->event_stream_kind = CLI_STREAM_NONE;
	ctx->event_stream_has_delta = 0;
	ctx->event_stream_complete = 0;
	ctx->event_stream_visible = 0;
}

static int presentation_append_stream(struct cli_context *ctx, int kind,
				      const char *text, int is_delta,
				      int is_complete)
{
	int rc;

	if (!ctx || !text)
		return 0;
	if (ctx->event_stream_kind != CLI_STREAM_NONE &&
	    ctx->event_stream_kind != kind)
		presentation_print_stream(ctx);
	if (ctx->event_stream_kind == CLI_STREAM_NONE)
		ctx->event_stream_kind = kind;
	if (!is_delta && ctx->event_stream_has_delta) {
		ctx->event_stream_complete = is_complete;
		return 0;
	}
	rc = morph_buf_puts(&ctx->event_stream, text);
	if (rc != 0)
		return rc;
	if (is_delta)
		ctx->event_stream_has_delta = 1;
	if (is_complete)
		ctx->event_stream_complete = 1;
	return 0;
}

static void presentation_reasoning_delta(struct cli_context *ctx,
					 const char *text)
{
	if (!ctx || !text || !text[0])
		return;
	presentation_clear_status(ctx);
	if (!ctx->event_stream_visible)
		printf("\n" ANSI_DIM "• Reasoning  " ANSI_RESET);
	printf(ANSI_DIM "%s" ANSI_RESET, text);
	fflush(stdout);
	ctx->event_stream_visible = 1;
}

static void presentation_tool_call(struct cli_context *ctx,
				   const struct morph_event *ev)
{
	const char *tool = event_string(ev, "tool");
	const char *title = event_string(ev, "toolTitle");
	cJSON *args_item = event_item(ev, "args");
	char *args = event_args_json(ev);
	char display[512];

	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	if (!tool || !tool[0])
		tool = "tool";
	if (!title || !title[0])
		title = tool;
	display[0] = '\0';
	if (args && strcmp(args, "{}") != 0) {
		utf8_copy_sanitized_display_width(display, sizeof(display),
						  args,
						  CLI_EVENT_ARGS_MAX);
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("tool: %s", tool);
		if (display[0])
			printf(" %s", display);
		printf("\n");
	} else {
		printf("\n" ANSI_BOLD ANSI_CYAN "•" ANSI_RESET " "
		       ANSI_BOLD "%s" ANSI_RESET "\n", title);
		print_json_tree_children(args_item);
		presentation_status(ctx, "Running tool…");
	}
	free(args);
}

static void presentation_tool_end(struct cli_context *ctx,
				  const struct morph_event *ev)
{
	const char *title = event_string(ev, "toolTitle");
	const char *tool = event_string(ev, "tool");
	const char *error = event_string(ev, "error");
	int failed;

	presentation_clear_status(ctx);
	if (!title || !title[0])
		title = tool && tool[0] ? tool : "Tool";
	failed = !ev->name || strcmp(ev->name, "tool.result") != 0;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("status: %s %s\n", tool ? tool : "tool",
		       failed ? "failed" : "completed");
		return;
	}
	if (failed) {
		printf("  " ANSI_BOLD ANSI_RED "✗ %s failed" ANSI_RESET,
		       title);
		if (error && error[0])
			printf(ANSI_DIM " · %s" ANSI_RESET, error);
		printf("\n");
	} else {
		printf("  " ANSI_BOLD ANSI_GREEN "✓ %s completed"
		       ANSI_RESET "\n", title);
	}
}

static const char *plan_mark(const char *status, int active)
{
	if (active)
		return "›";
	if (status && strcmp(status, "completed") == 0)
		return "✔";
	if (status && strcmp(status, "failed") == 0)
		return "✘";
	if (status && strcmp(status, "skipped") == 0)
		return "–";
	return "□";
}

static int presentation_plan(const struct morph_event *ev)
{
	cJSON *result;
	cJSON *plans;
	cJSON *plan;
	int printed = 0;

	if (!ev || !cJSON_IsObject(ev->data) ||
	    !event_string(ev, "tool") ||
	    strcmp(event_string(ev, "tool"), "plan") != 0)
		return 0;
	result = cJSON_GetObjectItemCaseSensitive(ev->data, "data");
	plans = cJSON_IsObject(result) ?
		cJSON_GetObjectItemCaseSensitive(result, "plans") : NULL;
	if (!cJSON_IsArray(plans))
		return 0;
	cJSON_ArrayForEach(plan, plans) {
		cJSON *steps;
		cJSON *step;
		int index = 0;
		int count;

		if (!cJSON_IsObject(plan))
			continue;
		steps = cJSON_GetObjectItemCaseSensitive(plan, "steps");
		if (!cJSON_IsArray(steps))
			continue;
		if (!printed)
			printf("  " ANSI_DIM "└ Updated plan" ANSI_RESET "\n");
		printed = 1;
		count = cJSON_GetArraySize(steps);
		cJSON_ArrayForEach(step, steps) {
			cJSON *description;
			cJSON *status;
			cJSON *active;
			const char *branch;

			if (!cJSON_IsObject(step))
				continue;
			description = cJSON_GetObjectItemCaseSensitive(
				step, "description");
			status = cJSON_GetObjectItemCaseSensitive(step, "status");
			active = cJSON_GetObjectItemCaseSensitive(step, "active");
			if (!cJSON_IsString(description))
				continue;
			index++;
			branch = index == count ? "└" : "├";
			printf("    " ANSI_DIM "%s" ANSI_RESET " %s %s\n",
			       branch,
			       plan_mark(cJSON_IsString(status) ?
					 status->valuestring : NULL,
					 cJSON_IsTrue(active)),
			       description->valuestring);
		}
	}
	return printed;
}

static void presentation_observation(struct cli_context *ctx,
				     const struct morph_event *ev)
{
	const char *text = event_string(ev, "text");
	cJSON *structured = event_item(ev, "data");
	cJSON *parsed = NULL;

	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	if (!text || !text[0])
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		print_plain_labeled("observation", text);
	} else if (presentation_plan(ev)) {
		return;
	} else if (event_error_code(ev) < 0) {
		print_indented("  " ANSI_RED "└ Error: " ANSI_RESET, text);
	} else {
		if (!cJSON_IsArray(structured) &&
		    !cJSON_IsObject(structured))
			parsed = cJSON_Parse(text);
		if (cJSON_IsArray(structured) ||
		    cJSON_IsObject(structured)) {
			int ancestors_last[CLI_TREE_DEPTH_MAX] = {0};

			print_json_tree_node(structured, "result", 0, 1,
					     ancestors_last);
		} else if (parsed) {
			int ancestors_last[CLI_TREE_DEPTH_MAX] = {0};

			print_json_tree_node(parsed, "result", 0, 1,
					     ancestors_last);
		} else {
			print_indented("  " ANSI_DIM "└ " ANSI_RESET, text);
		}
		cJSON_Delete(parsed);
	}
}

static void presentation_reflection(struct cli_context *ctx,
				    const struct morph_event *ev)
{
	const char *text = event_string(ev, "text");

	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		print_plain_labeled("guardrail", text);
	else if (text && text[0])
		print_indented(ANSI_YELLOW "• Guardrail  " ANSI_RESET, text);
}

static void presentation_final(struct cli_context *ctx,
			       const struct morph_event *ev)
{
	const char *text = event_string(ev, "text");
	int had_stream = ctx->markdown_stream_visible &&
		(ctx->markdown_stream_kind == CLI_STREAM_THOUGHT ||
		 ctx->markdown_stream_kind == CLI_STREAM_FINAL);

	presentation_clear_status(ctx);
	if (ctx->event_stream_kind == CLI_STREAM_THOUGHT &&
	    !ctx->event_stream_complete)
		presentation_discard_stream(ctx);
	else
		presentation_print_stream(ctx);
	cli_markdown_stream_reset(ctx, 1);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("final:\n");
		if (text && text[0])
			printf("%s\n", text);
	} else {
		if (!had_stream)
			printf("\n");
		if (!had_stream && text && text[0]) {
			cli_markdown_render_ansi_with_media_indented(
				text, 2, media_callback, ctx);
		} else if (!had_stream) {
			printf("\n");
		}
		printf("\n");
	}
	ctx->final_rendered = 1;
}

static void presentation_turn_end(struct cli_context *ctx,
				  const struct morph_event *ev)
{
	const char *error;
	const char *detail;
	const char *outcome;

	if (!ev->phase || strcmp(ev->phase, "end") == 0)
		return;
	if (ctx->final_rendered)
		return;
	cli_markdown_stream_reset(ctx, 1);
	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	error = event_string(ev, "error");
	detail = event_string(ev, "detail");
	outcome = event_string(ev, "outcome");
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("error: %s", error ? error :
		       (outcome ? outcome : "turn failed"));
		if (detail && detail[0])
			printf(" (%s)", detail);
		printf("\n");
	} else {
		printf("\n" ANSI_BOLD ANSI_RED "• Error" ANSI_RESET " %s",
		       error ? error : (outcome ? outcome : "turn failed"));
		if (detail && detail[0])
			printf(ANSI_DIM " (%s)" ANSI_RESET, detail);
		printf("\n");
	}
	ctx->final_rendered = 1;
}

static void presentation_auth(struct cli_context *ctx,
			      const struct morph_event *ev)
{
	const char *backend = event_string(ev, "backend");
	const char *tool = event_string(ev, "tool");
	const char *env_name = event_string(ev, "env_name");
	const struct config *config;

	presentation_clear_status(ctx);
	presentation_print_stream(ctx);
	if (!backend)
		backend = "configured";
	config = ctx->runtime ? runtime_config_get(ctx->runtime) : NULL;
	if ((!env_name || !env_name[0]) && config) {
		if (strcmp(backend, "image") == 0)
			env_name = config->models.image.api_key_env;
		else if (strcmp(backend, "video") == 0)
			env_name = config->models.video.api_key_env;
		else
			env_name = config->models.text.api_key_env;
	}
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("error: authentication required for %s", backend);
		if (tool && tool[0])
			printf(" tool %s", tool);
		if (env_name && env_name[0])
			printf("; export %s", env_name);
		printf("\n");
	} else {
		printf("\n" ANSI_BOLD ANSI_YELLOW
		       "• Authentication required" ANSI_RESET " for %s",
		       backend);
		if (tool && tool[0])
			printf(" tool " ANSI_BOLD "%s" ANSI_RESET, tool);
		if (env_name && env_name[0])
			printf(ANSI_DIM " · export %s" ANSI_RESET, env_name);
		printf("\n");
	}
}

static void presentation_artifact(struct cli_context *ctx,
				  const struct morph_event *ev)
{
	const char *path = event_string(ev, "path");
	const char *kind = event_string(ev, "kind");

	if (!ev->name || strcmp(ev->name, "artifact.ready") != 0 ||
	    !path || !path[0])
		return;
	if (morph_strmap_contains(&ctx->rendered_artifacts, path))
		return;
	presentation_clear_status(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		(void)morph_strmap_set(&ctx->rendered_artifacts, path,
				       (void *)1);
		printf("artifact: %s %s\n", kind ? kind : "file", path);
		return;
	}
	printf("  " ANSI_DIM "└ %s: %s" ANSI_RESET "\n",
	       kind ? kind : "artifact", path);
	if (kind && strcmp(kind, "image") == 0)
		media_callback("image", path, ctx);
	else if (kind && strcmp(kind, "video") == 0)
		media_callback("video", path, ctx);
	else
		(void)morph_strmap_set(&ctx->rendered_artifacts, path,
				       (void *)1);
}

static void presentation_auxiliary(struct cli_context *ctx,
				   const struct morph_event *ev)
{
	const char *prefix;

	if (!ctx->presentation_ready ||
	    ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (ev->type == MORPH_EVENT_BACKGROUND)
		prefix = "Background";
	else if (ev->type == MORPH_EVENT_TASK)
		prefix = "Task";
	else if (ev->type == MORPH_EVENT_MCP)
		prefix = "MCP";
	else if (ev->type == MORPH_EVENT_ERROR)
		prefix = "Error";
	else
		return;
	presentation_clear_status(ctx);
	printf("\n" ANSI_DIM "• %s  %s" ANSI_RESET "\n", prefix,
	       ev->message ? ev->message : (ev->name ? ev->name : ""));
}

int cli_presentation_init(struct cli_context *ctx)
{
	int rc;

	if (!ctx)
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_init(&ctx->event_stream, BUFSIZ);
	if (rc != 0)
		return rc;
	rc = morph_buf_init(&ctx->markdown_stream_text, BUFSIZ);
	if (rc != 0) {
		morph_buf_cleanup(&ctx->event_stream);
		return rc;
	}
	rc = morph_strmap_init(&ctx->rendered_artifacts,
			       MORPH_STRMAP_INIT_CAP);
	if (rc != 0) {
		morph_buf_cleanup(&ctx->markdown_stream_text);
		morph_buf_cleanup(&ctx->event_stream);
		return rc;
	}
	return 0;
}

void cli_presentation_reset(struct cli_context *ctx)
{
	if (!ctx)
		return;
	morph_buf_reset(&ctx->event_stream);
	morph_strmap_clear(&ctx->rendered_artifacts);
	cli_markdown_stream_reset(ctx, 1);
	ctx->event_stream_kind = CLI_STREAM_NONE;
	ctx->event_stream_has_delta = 0;
	ctx->event_stream_complete = 0;
	ctx->event_stream_visible = 0;
	ctx->final_rendered = 0;
	ctx->status_visible = 0;
}

void cli_presentation_finish(struct cli_context *ctx)
{
	presentation_clear_status(ctx);
}

void cli_presentation_cleanup(struct cli_context *ctx)
{
	if (!ctx)
		return;
	presentation_clear_status(ctx);
	cli_markdown_stream_reset(ctx, 0);
	morph_buf_cleanup(&ctx->markdown_stream_text);
	morph_buf_cleanup(&ctx->event_stream);
	morph_strmap_cleanup(&ctx->rendered_artifacts);
}

int cli_presentation_event(struct cli_context *ctx,
			   const struct morph_event *ev)
{
	const char *text;

	if (!ctx || !ev)
		MORPH_RETURN(-EINVAL);
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		char *json = morph_event_to_json_string(ev);

		if (!json)
			MORPH_RETURN(-ENOMEM);
		printf("%s\n", json);
		fflush(stdout);
		free(json);
		return 0;
	}
	if (!ctx->presentation_ready)
		return 0;
	if (!ctx->turn_active &&
	    (ev->type == MORPH_EVENT_REACT ||
	     ev->type == MORPH_EVENT_TOOL ||
	     ev->type == MORPH_EVENT_HITL ||
	     ev->type == MORPH_EVENT_ARTIFACT))
		return 0;

	if (ev->type == MORPH_EVENT_REACT && ev->name) {
		text = event_string(ev, "text");
		if (strcmp(ev->name, "react.turn.begin") == 0) {
			if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
				presentation_status(ctx, "Starting…");
			return 0;
		}
		if (strcmp(ev->name, "react.thinking") == 0) {
			presentation_status(ctx, "Thinking…");
			return 0;
		}
		if (strcmp(ev->name, "react.thought.delta") == 0) {
			int rc = presentation_append_stream(
				ctx, CLI_STREAM_THOUGHT, text, 1, 0);

			if (rc == 0 &&
			    ctx->presentation_mode == CLI_PRESENT_INTERACTIVE) {
				presentation_clear_status(ctx);
				rc = cli_markdown_stream_append(
					ctx, text, CLI_STREAM_THOUGHT);
			}
			return rc;
		}
		if (strcmp(ev->name, "react.thought.end") == 0)
			return presentation_append_stream(
				ctx, CLI_STREAM_THOUGHT, text, 0, 1);
		if (strcmp(ev->name, "react.reasoning.delta") == 0) {
			int rc = presentation_append_stream(
				ctx, CLI_STREAM_REASONING, text, 1, 0);

			if (rc == 0 &&
			    ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
				presentation_reasoning_delta(ctx, text);
			return rc;
		}
		if (strcmp(ev->name, "react.final.delta") == 0) {
			if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
				return 0;
			presentation_clear_status(ctx);
			if (ctx->event_stream_kind != CLI_STREAM_NONE)
				presentation_print_stream(ctx);
			return cli_markdown_stream_append(
				ctx, text, CLI_STREAM_FINAL);
		}
		if (strcmp(ev->name, "react.observation") == 0) {
			presentation_observation(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.reflection") == 0) {
			presentation_reflection(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.final") == 0) {
			presentation_final(ctx, ev);
			return 0;
		}
		if (strcmp(ev->name, "react.turn.end") == 0) {
			presentation_turn_end(ctx, ev);
			return 0;
		}
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name &&
	    strcmp(ev->name, "tool.call") == 0) {
		presentation_tool_call(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name &&
	    strcmp(ev->name, "tool.running") == 0) {
		presentation_status(ctx, "Running tool…");
		return 0;
	}
	if (ev->type == MORPH_EVENT_TOOL && ev->name &&
	    (strcmp(ev->name, "tool.result") == 0 ||
	     strcmp(ev->name, "tool.failed") == 0 ||
	     strcmp(ev->name, "tool.cancelled") == 0)) {
		presentation_tool_end(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_HITL && ev->name &&
	    strcmp(ev->name, "auth.required") == 0) {
		presentation_auth(ctx, ev);
		return 0;
	}
	if (ev->type == MORPH_EVENT_ARTIFACT) {
		presentation_artifact(ctx, ev);
		return 0;
	}
	presentation_auxiliary(ctx, ev);
	fflush(stdout);
	return 0;
}
