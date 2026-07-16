#include "cli/internal.h"

/* ---- output_callback ---- */

static int is_image_ext(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return 0;
	return (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 ||
		strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".gif") == 0 ||
		strcasecmp(ext, ".webp") == 0 || strcasecmp(ext, ".bmp") == 0);
}

static int is_video_ext(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return 0;
	return (strcasecmp(ext, ".mp4") == 0 || strcasecmp(ext, ".mov") == 0 ||
		strcasecmp(ext, ".avi") == 0 || strcasecmp(ext, ".mkv") == 0 ||
		strcasecmp(ext, ".webm") == 0);
}

static int is_media_path(const char *path)
{
	return is_image_ext(path) || is_video_ext(path);
}

static char *wrap_bare_media_paths(const char *content)
{
	if (!content || !*content)
		return NULL;

	morph_buf_t out;
	int buf_ok = morph_buf_init(&out, 4096);
	if (buf_ok != 0)
		return NULL;

	const char *p = content;
	while (*p) {
		if (p[0] == '!' && p[1] == '[') {
			const char *close = strchr(p + 2, ')');
			if (close) {
				size_t chunk = (size_t)(close - p) + 1;
				if (morph_buf_append(&out, p, chunk) != 0)
					goto fail;
				p = close + 1;
				continue;
			}
		}
		if (p[0] == '[' && p[1] != ']') {
			const char *close = strchr(p + 1, ')');
			if (close) {
				size_t chunk = (size_t)(close - p) + 1;
				if (morph_buf_append(&out, p, chunk) != 0)
					goto fail;
				p = close + 1;
				continue;
			}
		}

		const char *path_start = NULL;
		const char *q = p;
		while (*q) {
			if ((q == content || q[-1] == ' ' || q[-1] == '\n' || q[-1] == '\r' || q[-1] == '\t' || q[-1] == ':' || q[-1] == '`') &&
			    (strncmp(q, "~/.morph/output/", 16) == 0 ||
			     strncmp(q, "/.morph/output/", 15) == 0)) {
				path_start = q;
				break;
			}
			q++;
		}

		if (!path_start) {
			if (morph_buf_putc(&out, *p++) != 0)
				goto fail;
			continue;
		}

		if (path_start > p) {
			size_t pre = (size_t)(path_start - p);
			if (morph_buf_append(&out, p, pre) != 0)
				goto fail;
		}

		const char *path_end = path_start;
		while (*path_end && *path_end != ' ' && *path_end != '\n' &&
		       *path_end != '\r' && *path_end != '\t' &&
		       *path_end != ')' && *path_end != ']' &&
		       *path_end != '`' && *path_end != '"' &&
		       *path_end != '\'' && *path_end != ',' &&
		       *path_end != ';' && *path_end != '\0')
			path_end++;

		size_t path_len = (size_t)(path_end - path_start);
		if (path_len > 0 && is_media_path(path_start)) {
			const char *prefix;
			const char *suffix;
			if (is_image_ext(path_start)) {
				prefix = "![image](";
				suffix = ")";
			} else {
				prefix = "[video](";
				suffix = ")";
			}
			if (morph_buf_puts(&out, prefix) != 0 ||
			    morph_buf_append(&out, path_start, path_len) != 0 ||
			    morph_buf_puts(&out, suffix) != 0)
				goto fail;
		} else {
			if (morph_buf_append(&out, path_start, path_len) != 0)
				goto fail;
		}
		p = path_end;
	}

	return morph_buf_detach(&out);

fail:
	morph_buf_cleanup(&out);
	return NULL;
}

void media_callback(const char *type, const char *path, void *user)
{
	if (strcmp(type, "image") == 0) {
		image_render_terminal(path);
	} else if (strcmp(type, "video") == 0) {
		struct cli_context *ctx = (struct cli_context *)user;
		const char *mpv = (ctx && ctx->config.render.mpv_args[0])
				  ? ctx->config.render.mpv_args : NULL;
		video_play(path, mpv);
	}
}

void cli_markdown_render_ansi(const char *md)
{
	char *normalized;

	if (!md) {
		markdown_render_ansi(NULL);
		return;
	}
	normalized = agent_ui_normalize_markdown(md);
	markdown_render_ansi(normalized ? normalized : md);
	free(normalized);
}

void cli_markdown_render_ansi_with_media(const char *md,
						markdown_media_cb cb,
						void *user)
{
	char *normalized;

	if (!md) {
		markdown_render_ansi_with_media(NULL, cb, user);
		return;
	}
	normalized = agent_ui_normalize_markdown(md);
	markdown_render_ansi_with_media(normalized ? normalized : md, cb, user);
	free(normalized);
}


/* ---- output_callback helpers ---- */

static const char *output_label(enum react_step_type type)
{
	switch (type) {
	case REACT_STEP_THOUGHT:
		return "thought";
	case REACT_STEP_REASONING:
		return "reasoning";
	case REACT_STEP_OBSERVATION:
		return "observation";
	case REACT_STEP_ACTION:
		return "tool";
	case REACT_STEP_REFLECTION:
		return "guardrail";
	case REACT_STEP_FINAL:
		return "final";
	}
	return "output";
}

static void print_labeled_text(const char *label, const char *content)
{
	char *display;

	if (!content || !*content)
		return;
	display = utf8_dup_clamped(content, 2000);
	if (!display)
		return;
	if (strchr(display, '\n')) {
		printf(ANSI_DIM "%s:" ANSI_RESET "\n%s\n", label, display);
	} else {
		printf(ANSI_DIM "%s:" ANSI_RESET " %s\n", label, display);
	}
	free(display);
	fflush(stdout);
}

static void output_flush_stream(struct cli_context *ctx)
{
	if (!ctx || !ctx->streaming)
		return;
	if (ctx->stream_buf_len > 0)
		print_labeled_text(output_label(ctx->stream_type),
				   ctx->stream_buf);
	ctx->streaming = 0;
	ctx->stream_buf[0] = '\0';
	ctx->stream_buf_len = 0;
}

static int output_handle_stream(struct cli_context *ctx,
				enum react_step_type type,
				const char *content)
{
	if (content && *content) {
		if (!ctx->streaming || ctx->stream_type != type) {
			output_flush_stream(ctx);
			ctx->streaming = 1;
			ctx->stream_type = type;
			ctx->stream_buf[0] = '\0';
			ctx->stream_buf_len = 0;
		}
		size_t clen = strlen(content);
		size_t avail = sizeof(ctx->stream_buf) - ctx->stream_buf_len - 1;
		if (clen > avail) {
			size_t keep = sizeof(ctx->stream_buf) / 2;
			if (keep > ctx->stream_buf_len)
				keep = ctx->stream_buf_len;
			memmove(ctx->stream_buf,
				ctx->stream_buf + ctx->stream_buf_len - keep,
				keep);
			ctx->stream_buf_len = keep;
			avail = sizeof(ctx->stream_buf) - ctx->stream_buf_len - 1;
		}
		if (clen > avail)
			clen = avail;
		memcpy(ctx->stream_buf + ctx->stream_buf_len, content, clen);
		ctx->stream_buf_len += clen;
		ctx->stream_buf[ctx->stream_buf_len] = '\0';
	} else if (!ctx->streaming) {
		ctx->streaming = 1;
		ctx->stream_type = type;
		ctx->stream_buf[0] = '\0';
		ctx->stream_buf_len = 0;
	}
	return 0;
}

static int output_handle_thought(struct cli_context *ctx, const char *content)
{
	return output_handle_stream(ctx, REACT_STEP_THOUGHT, content);
}

static int output_handle_reasoning(struct cli_context *ctx, const char *content)
{
	return output_handle_stream(ctx, REACT_STEP_REASONING, content);
}

static int output_handle_action(struct cli_context *ctx,
				const struct react_output_event *event)
{
	const char *tool_name;
	char display_args[512];

	output_flush_stream(ctx);
	if (!event)
		return 0;

	if (event->status != REACT_OUTPUT_STARTED)
		return 0;

	tool_name = event->tool_name && *event->tool_name ?
		event->tool_name : "Executing";
	ctx->last_tool_was_plan = (strcmp(tool_name, "plan") == 0);
	printf(ANSI_DIM "tool:" ANSI_RESET " " ANSI_BOLD "%s" ANSI_RESET,
	       tool_name);
	if (event->tool_args && *event->tool_args &&
	    strcmp(event->tool_args, "{}") != 0) {
		utf8_copy_sanitized_display_width(display_args,
						  sizeof(display_args),
						  event->tool_args, 180);
		printf(" " ANSI_DIM "%s" ANSI_RESET, display_args);
	}
	printf("\n");
	fflush(stdout);
	return 0;
}

static char plan_status_mark(const char *status)
{
	if (!status)
		return ' ';
	if (strcmp(status, "completed") == 0)
		return 'x';
	if (strcmp(status, "in_progress") == 0)
		return '>';
	if (strcmp(status, "failed") == 0)
		return '!';
	if (strcmp(status, "skipped") == 0)
		return '-';
	return ' ';
}

static int render_plan_data(const cJSON *data)
{
	const cJSON *plans;
	const cJSON *plan;
	int printed = 0;

	if (!cJSON_IsObject(data))
		return 0;
	plans = cJSON_GetObjectItemCaseSensitive((cJSON *)data, "plans");
	if (!cJSON_IsArray(plans))
		return 0;

	cJSON_ArrayForEach(plan, plans) {
		const cJSON *name;
		const cJSON *goal;
		const cJSON *steps;
		const cJSON *step;
		int step_printed = 0;

		if (!cJSON_IsObject(plan))
			continue;
		name = cJSON_GetObjectItemCaseSensitive((cJSON *)plan, "name");
		goal = cJSON_GetObjectItemCaseSensitive((cJSON *)plan, "goal");
		steps = cJSON_GetObjectItemCaseSensitive((cJSON *)plan, "steps");

		if (printed)
			printf("\n");
		printf(ANSI_BOLD "Plan:" ANSI_RESET " %s\n",
		       cJSON_GetStringValue((cJSON *)name) ?
		       cJSON_GetStringValue((cJSON *)name) : "(unnamed)");
		printed = 1;

		if (cJSON_GetStringValue((cJSON *)goal) &&
		    *cJSON_GetStringValue((cJSON *)goal)) {
			printf(ANSI_BOLD "Goal:" ANSI_RESET " %s\n",
			       cJSON_GetStringValue((cJSON *)goal));
		}

		if (!cJSON_IsArray(steps))
			continue;
		cJSON_ArrayForEach(step, steps) {
			const cJSON *desc;
			const cJSON *status;
			const cJSON *active;
			char mark;

			if (!cJSON_IsObject(step))
				continue;
			desc = cJSON_GetObjectItemCaseSensitive((cJSON *)step,
								"description");
			status = cJSON_GetObjectItemCaseSensitive((cJSON *)step,
								  "status");
			active = cJSON_GetObjectItemCaseSensitive((cJSON *)step,
								  "active");
			if (!cJSON_GetStringValue((cJSON *)desc))
				continue;
			if (!step_printed) {
				printf("\n");
				step_printed = 1;
			}
			mark = plan_status_mark(
				cJSON_GetStringValue((cJSON *)status));
			if (cJSON_IsTrue(active))
				printf("  %c %s " ANSI_DIM "(active)"
				       ANSI_RESET "\n", mark,
				       cJSON_GetStringValue((cJSON *)desc));
			else if (mark == ' ')
				printf("    %s\n",
				       cJSON_GetStringValue((cJSON *)desc));
			else
				printf("  %c %s\n", mark,
				       cJSON_GetStringValue((cJSON *)desc));
		}
	}
	return printed;
}

static void print_tool_observation(const char *content)
{
	size_t len = strlen(content);

	if (len > 2000) {
		printf("%.1997s...\n", content);
	} else {
		printf("%s\n", content);
	}
}

/*
 * Handle OBSERVATION step: stop spinner with appropriate status.
 * ctx - CLI context.
 * content - Observation content (tool result or error).
 *
 * Returns 0 always.
 */
static int output_handle_observation(struct cli_context *ctx,
				     const struct react_output_event *event)
{
	const char *content = event && event->text ? event->text : "";

	output_flush_stream(ctx);
	if (ctx->last_tool_was_plan && content && *content) {
		printf(ANSI_DIM "observation:" ANSI_RESET "\n");
		if (!render_plan_data(event ? event->data : NULL))
			print_tool_observation(content);
		fflush(stdout);
	} else if (content && *content) {
		print_labeled_text("observation", content);
	}
	return 0;
}

/*
 * Handle REFLECTION step: display guardrail message.
 * ctx - CLI context.
 * content - Reflection message from guardrail.
 *
 * Returns 0 always.
 */
static int output_handle_reflection(struct cli_context *ctx, const char *content)
{
	output_flush_stream(ctx);
	printf(ANSI_BOLD ANSI_CYAN "🛡 Guardrail" ANSI_RESET " %s\n",
	       content ? content : "");
	fflush(stdout);
	return 0;
}

/*
 * Handle FINAL step: render the final answer as markdown.
 * ctx - CLI context.
 * content - Final answer content (may be NULL or empty).
 *
 * Returns 0 always.
 */
static int output_handle_final(struct cli_context *ctx, const char *content)
{
	output_flush_stream(ctx);
	printf(ANSI_DIM "final:" ANSI_RESET "\n");
	if (content && *content) {
		char *wrapped = wrap_bare_media_paths(content);
		cli_markdown_render_ansi_with_media(wrapped ? wrapped : content,
						    media_callback, ctx);
		free(wrapped);
	} else {
		printf("\n");
	}

	printf("\n");
	fflush(stdout);
	return 0;
}

/*
 * ReAct output callback: dispatch step-type events to per-type handlers.
 * event - Structured ReAct output event.
 * user_data - Pointer to cli_context.
 *
 * Returns 0 always.
 */
int output_callback(const struct react_output_event *event, void *user_data)
{
	struct cli_context *ctx = user_data;
	const char *content = event && event->text ? event->text : "";

	if (!event)
		return 0;

	switch (event->type) {
	case REACT_STEP_THOUGHT:
		return output_handle_thought(ctx, content);
	case REACT_STEP_ACTION:
		return output_handle_action(ctx, event);
	case REACT_STEP_OBSERVATION:
		return output_handle_observation(ctx, event);
	case REACT_STEP_REFLECTION:
		return output_handle_reflection(ctx, content);
	case REACT_STEP_FINAL:
		return output_handle_final(ctx, content);
	case REACT_STEP_REASONING:
		return output_handle_reasoning(ctx, content);
	}
	return 0;
}


static int cli_set_ask_user_answers(char ***answers,
				    int *answers_count,
				    const char *const *values,
				    int values_count)
{
	if (!answers || !answers_count)
		return -EINVAL;
	*answers = NULL;
	*answers_count = 0;
	if (values_count <= 0)
		return 0;

	char **out = calloc((size_t)values_count, sizeof(char *));
	if (!out)
		return -ENOMEM;
	for (int i = 0; i < values_count; i++) {
		out[i] = strdup(values[i] ? values[i] : "");
		if (!out[i]) {
			for (int j = 0; j < i; j++)
				free(out[j]);
			free(out);
			return -ENOMEM;
		}
	}
	*answers = out;
	*answers_count = values_count;
	return 0;
}

static int cli_parse_multi_choice_answer(char *input,
					 const char *const *choices,
					 int choices_count,
					 int min_choices,
					 int max_choices,
					 char ***answers,
					 int *answers_count)
{
	int selected[64] = {0};
	int selected_count = 0;
	char *save = NULL;

	for (char *tok = strtok_r(input, ", ", &save); tok;
	     tok = strtok_r(NULL, ", ", &save)) {
		int n = atoi(tok);
		if (n < 1 || n > choices_count)
			continue;
		if (!selected[n - 1]) {
			selected[n - 1] = 1;
			selected_count++;
		}
	}

	if (selected_count < min_choices ||
	    (max_choices > 0 && selected_count > max_choices))
		return cli_set_ask_user_answers(answers, answers_count, NULL, 0);

	const char *values[64] = {0};
	int count = 0;
	for (int i = 0; i < choices_count; i++) {
		if (selected[i])
			values[count++] = choices[i];
	}
	return cli_set_ask_user_answers(answers, answers_count, values, count);
}

int cli_ask_user_callback(const char *question,
			  const char *const *choices,
			  int choices_count,
			  const char *selection_mode,
			  int min_choices,
			  int max_choices,
			  char ***answers,
			  int *answers_count,
			  void *user_data)
{
	struct cli_context *ctx = user_data;
	int multi = selection_mode && strcmp(selection_mode, "multi") == 0;
	if (!ctx || !answers || !answers_count)
		return -EINVAL;

	printf(ANSI_BOLD ANSI_CYAN "? %s" ANSI_RESET "\n", question);

	char prompt[128];
	if (choices && choices_count > 0) {
		for (int i = 0; i < choices_count; i++)
			printf("  %d. %s\n", i + 1, choices[i]);
		if (multi && max_choices > 0) {
			snprintf(prompt, sizeof(prompt),
				 "  [" ANSI_GREEN "1-%d, comma separated; %d-%d choices" ANSI_RESET "]: ",
				 choices_count, min_choices, max_choices);
		} else if (multi) {
			snprintf(prompt, sizeof(prompt),
				 "  [" ANSI_GREEN "1-%d, comma separated; min %d" ANSI_RESET "]: ",
				 choices_count, min_choices);
		} else {
			snprintf(prompt, sizeof(prompt),
				 "  [" ANSI_GREEN "1-%d" ANSI_RESET "]: ",
				 choices_count);
		}
	} else {
		snprintf(prompt, sizeof(prompt), "  > ");
	}
#ifdef HAVE_READLINE
	{
		char *input = NULL;
		FILE *tty = fopen("/dev/tty", "r");
		if (tty) {
			FILE *old_in = rl_instream;
			rl_instream = tty;
			input = readline(prompt);
			rl_instream = old_in;
			fclose(tty);
		} else {
			input = readline(prompt);
		}
		if (!input) {
			printf("\n");
			return -EIO;
		}
		if (input[0] != '\0')
			add_history(input);
		if (choices && choices_count > 0) {
			if (multi) {
				int rc = cli_parse_multi_choice_answer(
					input, choices, choices_count,
					min_choices, max_choices,
					answers, answers_count);
				free(input);
				return rc;
			}
			int n = atoi(input);
			if (n >= 1 && n <= choices_count) {
				const char *values[] = { choices[n - 1] };
				free(input);
				return cli_set_ask_user_answers(
					answers, answers_count, values, 1);
			}
		}
		const char *values[] = { input };
		int rc = cli_set_ask_user_answers(answers, answers_count,
						  values, 1);
		free(input);
		return rc;
	}
#else
	fflush(stdout);
	char buf[1024] = {0};
	FILE *tty = fopen("/dev/tty", "r");
	if (!tty) {
		printf("\n");
		return -ENOTTY;
	}
	if (!fgets(buf, sizeof(buf), tty)) {
		fclose(tty);
		printf("\n");
		return -EIO;
	}
	fclose(tty);

	buf[strcspn(buf, "\n")] = '\0';

	if (choices && choices_count > 0) {
		if (multi)
			return cli_parse_multi_choice_answer(
				buf, choices, choices_count, min_choices,
				max_choices, answers, answers_count);
		int n = atoi(buf);
		if (n >= 1 && n <= choices_count) {
			const char *values[] = { choices[n - 1] };
			return cli_set_ask_user_answers(answers, answers_count,
							values, 1);
		}
	}

	const char *values[] = { buf };
	return cli_set_ask_user_answers(answers, answers_count, values, 1);
#endif
}

/*
 * Generic y/n/a prompt shared by HITL and bash_exec approval flows.
 *
 * subject - Short label rendered after the "Approved (...)"/"Denied (...)"
 *           summary so the user knows which decision they made.
 *
 * Returns:
 *   0 - denied
 *   1 - approved once
 *   2 - approved with "always" semantics
 */
static int prompt_yna(const char *subject)
{
	static pthread_mutex_t prompt_lock = PTHREAD_MUTEX_INITIALIZER;
	int v;

	pthread_mutex_lock(&prompt_lock);
#ifdef HAVE_READLINE
	char *rl_input = readline("  [y]es / [n]o / [a]lways: ");
	if (!rl_input) {
		printf("\n");
		pthread_mutex_unlock(&prompt_lock);
		return 0;
	}
	if (rl_input[0] == 'a' || rl_input[0] == 'A')
		v = 2;
	else if (rl_input[0] == 'y' || rl_input[0] == 'Y')
		v = 1;
	else
		v = 0;
	free(rl_input);
#else
	printf("  [" ANSI_GREEN "y" ANSI_RESET "]es / ["
	       ANSI_RED "n" ANSI_RESET "]o / [a]lways: ");
	fflush(stdout);

	char buf[16];
	FILE *tty = fopen("/dev/tty", "r");
	if (!tty) {
		printf("\n");
		pthread_mutex_unlock(&prompt_lock);
		return 0;
	}
	if (!fgets(buf, sizeof(buf), tty)) {
		fclose(tty);
		printf("\n");
		pthread_mutex_unlock(&prompt_lock);
		return 0;
	}
	fclose(tty);

	if (buf[0] == 'a' || buf[0] == 'A')
		v = 2;
	else if (buf[0] == 'y' || buf[0] == 'Y')
		v = 1;
	else
		v = 0;
#endif

	if (v == 1 || v == 2)
		printf(ANSI_BOLD ANSI_GREEN "  ✓ Approved" ANSI_RESET " (%s%s)\n",
		       subject ? subject : "",
		       v == 2 ? ", always" : "");
	else
		printf(ANSI_BOLD ANSI_RED "  ✗ Denied" ANSI_RESET " (%s)\n",
		       subject ? subject : "");

	fflush(stdout);
	pthread_mutex_unlock(&prompt_lock);
	return v;
}

enum hitl_verdict hitl_approval_callback(const char *tool_name,
						const char *tool_args,
						void *user_data)
{
	struct cli_context *ctx = user_data;
	if (!ctx)
		return HITL_DENY;

	printf(ANSI_BOLD ANSI_YELLOW "⚠ Approval Required" ANSI_RESET "\n");
	printf("  Tool: " ANSI_BOLD "%s" ANSI_RESET "\n", tool_name);

	if (tool_args && *tool_args && strcmp(tool_args, "{}") != 0) {
		char display_args[512];
		utf8_copy_sanitized_display_width(display_args,
						  sizeof(display_args),
						  tool_args, 200);
		printf("  Args: " ANSI_DIM "%s" ANSI_RESET "\n", display_args);
	}

	int v = prompt_yna(tool_name);
	if (v == 2)
		return HITL_ALWAYS;
	if (v == 1)
		return HITL_APPROVE;
	return HITL_DENY;
}

static const char *operation_label(enum tool_operation_kind kind)
{
	switch (kind) {
	case TOOL_OP_COMMAND:
		return "Shell Command Approval";
	case TOOL_OP_PATH_READ:
		return "Read Path Approval";
	case TOOL_OP_PATH_LIST:
		return "List Path Approval";
	case TOOL_OP_PATH_WRITE:
		return "Write Path Approval";
	case TOOL_OP_NETWORK:
		return "Network Approval";
	case TOOL_OP_EXTERNAL_SEND:
		return "External Send Approval";
	}
	return "Operation Approval";
}

static const char *operation_subject(enum tool_operation_kind kind)
{
	switch (kind) {
	case TOOL_OP_COMMAND:
		return "command";
	case TOOL_OP_PATH_READ:
		return "read_path";
	case TOOL_OP_PATH_LIST:
		return "list_path";
	case TOOL_OP_PATH_WRITE:
		return "write_path";
	case TOOL_OP_NETWORK:
		return "network";
	case TOOL_OP_EXTERNAL_SEND:
		return "external_send";
	}
	return "operation";
}

static const char *operation_scope_label(enum tool_operation_kind kind)
{
	switch (kind) {
	case TOOL_OP_COMMAND:
		return "Cwd";
	case TOOL_OP_PATH_READ:
	case TOOL_OP_PATH_LIST:
		return "Workspace";
	case TOOL_OP_PATH_WRITE:
		return "Output dir";
	case TOOL_OP_NETWORK:
		return "Scope";
	case TOOL_OP_EXTERNAL_SEND:
		return "Scope";
	}
	return "Scope";
}

enum tool_operation_verdict operation_approval_callback(
	const struct tool_operation *op, void *user_data)
{
	struct cli_context *ctx = user_data;
	if (!ctx || !op)
		return TOOL_OP_DENY;

	printf(ANSI_BOLD ANSI_YELLOW "⚠ %s" ANSI_RESET "\n",
	       operation_label(op->kind));
	if (op->tool_name && *op->tool_name)
		printf("  Tool: " ANSI_BOLD "%s" ANSI_RESET "\n",
		       op->tool_name);
	if (op->kind == TOOL_OP_COMMAND) {
		const char *command = op->action;
		if (command) {
			char display[512];
			utf8_copy_sanitized_display_width(display,
							  sizeof(display),
							  command, 380);
			printf("  Cmd:  " ANSI_BOLD "%s" ANSI_RESET "\n",
			       display);
		}
	} else if (op->target && *op->target) {
		printf("  Target: " ANSI_BOLD "%s" ANSI_RESET "\n",
		       op->target);
	}
	if (op->scope && *op->scope)
		printf("  %s:  " ANSI_DIM "%s" ANSI_RESET "\n",
		       operation_scope_label(op->kind), op->scope);
	if (op->kind == TOOL_OP_COMMAND)
		printf("  " ANSI_DIM "'always' will trust this program "
		       "(and cwd) for the rest of the session." ANSI_RESET "\n");
	else
		printf("  " ANSI_DIM "'always' will trust this scope "
		       "for the rest of the session." ANSI_RESET "\n");

	int v = prompt_yna(operation_subject(op->kind));
	if (v == 2)
		return TOOL_OP_ALWAYS;
	if (v == 1)
		return TOOL_OP_ALLOW;
	return TOOL_OP_DENY;
}
