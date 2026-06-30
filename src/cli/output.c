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

/*
 * Handle THOUGHT step: update spinner with streaming thought content.
 * ctx - CLI context.
 * content - Thought content fragment (may be NULL or empty).
 *
 * Returns 0 always.
 */
static int output_handle_thought(struct cli_context *ctx, const char *content)
{
	if (content && *content) {
		if (!ctx->streaming) {
			ctx->streaming = 1;
			ctx->stream_buf[0] = '\0';
			ctx->stream_buf_len = 0;
			if (!ctx->spin.running) {
				spin_start(&ctx->spin, SPIN_STATE_THINKING,
					   "Thinking");
			}
		}
		size_t clen = strlen(content);
		size_t avail = sizeof(ctx->stream_buf) - ctx->stream_buf_len - 1;
		if (clen > avail) {
			size_t keep = sizeof(ctx->stream_buf) / 2;
			memmove(ctx->stream_buf, ctx->stream_buf + ctx->stream_buf_len - keep, keep);
			ctx->stream_buf_len = keep;
			avail = sizeof(ctx->stream_buf) - ctx->stream_buf_len - 1;
		}
		if (clen > avail)
			clen = avail;
		memcpy(ctx->stream_buf + ctx->stream_buf_len, content, clen);
		ctx->stream_buf_len += clen;
		ctx->stream_buf[ctx->stream_buf_len] = '\0';

		const char *last_nl = strrchr(ctx->stream_buf, '\n');
		const char *preview = last_nl ? last_nl + 1 : ctx->stream_buf;
		while (*preview == ' ' || *preview == '\t')
			preview++;
		preview = utf8_suffix_display_width(preview, 60);
		char sub[128];
		utf8_copy_display_width(sub, sizeof(sub), preview, 60);
		spin_set_sub(&ctx->spin, sub);
		spin_render(&ctx->spin);
	} else if (!ctx->streaming) {
		if (!ctx->spin.running) {
			spin_start(&ctx->spin, SPIN_STATE_THINKING, "Thinking");
		}
		ctx->streaming = 1;
		ctx->stream_buf[0] = '\0';
		ctx->stream_buf_len = 0;
		spin_set_sub(&ctx->spin, "waiting for model stream...");
		spin_render(&ctx->spin);
	}
	return 0;
}

static int output_handle_reasoning(struct cli_context *ctx, const char *content)
{
	return output_handle_thought(ctx, content);
}

/*
 * Handle ACTION step: show tool execution spinner.
 * ctx - CLI context.
 * content - Action description, typically "tool_name(args)".
 *
 * Returns 0 always.
 */
static int output_handle_action(struct cli_context *ctx, const char *content)
{
	if (ctx->streaming) {
		spin_set_sub(&ctx->spin, NULL);
		ctx->streaming = 0;
	}
	if (content && strncmp(content, "Executing ", 10) == 0) {
		if (ctx->spin.running) {
			char msg[256];
			snprintf(msg, sizeof(msg), "Running %s", content + 10);
			spin_update(&ctx->spin, msg);
		}
		return 0;
	}
	if (content && strstr(content, " completed")) {
		return 0;
	}
	{
		char tool_name[64] = {0};
		if (content) {
			const char *paren = strchr(content, '(');
			if (paren) {
				size_t nlen = (size_t)(paren - content);
				if (nlen >= sizeof(tool_name)) nlen = sizeof(tool_name) - 1;
				memcpy(tool_name, content, nlen);
				tool_name[nlen] = '\0';
			} else {
				snprintf(tool_name, sizeof(tool_name), "%s", content);
			}
		}
		ctx->last_tool_was_plan = (strcmp(tool_name, "plan") == 0);
		if (!ctx->spin.running) {
			spin_start(&ctx->spin, SPIN_STATE_EXECUTING,
				   tool_name[0] ? tool_name : "Executing");
		} else {
			spin_update(&ctx->spin, tool_name[0] ? tool_name : "Executing");
		}
		if (content)
			spin_set_sub(&ctx->spin, content);
	}
	return 0;
}

static char *trim_plan_line(char *line)
{
	char *end;

	while (*line == ' ' || *line == '\t' || *line == '\r')
		line++;

	end = line + strlen(line);
	while (end > line &&
	       (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
		end--;
	}
	*end = '\0';
	return line;
}

static int render_plan_title(const char *line, int *printed)
{
	const char *start;
	const char *end;

	if (strncmp(line, "Plan \"", 6) != 0)
		return 0;

	start = line + 6;
	end = strrchr(start, '"');
	if (!end || end <= start)
		return 0;

	if (*printed)
		printf("\n");
	printf(ANSI_BOLD "Plan:" ANSI_RESET " %.*s\n",
	       (int)(end - start), start);
	*printed = 1;
	return 1;
}

static int render_plan_step(char *line, int *step_started)
{
	char mark;
	char *rest;
	char *active;

	if (line[0] != '[' || line[1] == '\0' || line[2] != ']')
		return 0;

	mark = line[1];
	rest = line + 3;
	while (*rest == ' ' || *rest == '\t')
		rest++;
	if (!*rest)
		return 0;

	active = strstr(rest, " <-- active");
	if (active)
		*active = '\0';

	if (!*step_started) {
		printf("\n");
		*step_started = 1;
	}

	if (mark == ' ')
		printf("    %s\n", rest);
	else
		printf("  %c %s\n", mark, rest);
	return 1;
}

static int render_plan_observation(const char *content)
{
	char *copy;
	char *saveptr = NULL;
	char *line;
	int printed = 0;
	int step_started = 0;
	int consumed = 0;

	if (!content || !*content)
		return 0;

	copy = strdup(content);
	if (!copy)
		return 0;

	line = strtok_r(copy, "\n", &saveptr);
	while (line) {
		char *trimmed = trim_plan_line(line);

		if (trimmed[0] == '\0' ||
		    strcmp(trimmed, "Plan created.") == 0 ||
		    strncmp(trimmed, "Plan created (", 14) == 0 ||
		    strncmp(trimmed, "Step ", 5) == 0) {
			line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}

		if (render_plan_title(trimmed, &printed)) {
			step_started = 0;
			consumed = 1;
		} else if (strncmp(trimmed, "Goal:", 5) == 0 && printed) {
			printf(ANSI_BOLD "Goal:" ANSI_RESET "%s\n",
			       trimmed + 5);
			consumed = 1;
		} else if (strstr(trimmed, "step(s)") && printed) {
			consumed = 1;
		} else if (render_plan_step(trimmed, &step_started)) {
			consumed = 1;
		}

		line = strtok_r(NULL, "\n", &saveptr);
	}

	free(copy);
	return consumed;
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
static int output_handle_observation(struct cli_context *ctx, const char *content)
{
	if (ctx->streaming) {
		spin_set_sub(&ctx->spin, NULL);
		ctx->streaming = 0;
	}
	if (ctx->spin.running) {
		char msg[128] = {0};
		if (content && strncmp(content, "tool error:", 11) == 0) {
			snprintf(msg, sizeof(msg), "Tool execution failed");
			spin_stop(&ctx->spin, SPIN_STATE_ERROR, msg);
		} else if (content && strncmp(content, "image generated:", 15) == 0) {
			snprintf(msg, sizeof(msg), "Image generated");
			spin_stop(&ctx->spin, SPIN_STATE_COMPLETE, msg);
		} else if (content && strncmp(content, "video generated:", 16) == 0) {
			snprintf(msg, sizeof(msg), "Video generated");
			spin_stop(&ctx->spin, SPIN_STATE_COMPLETE, msg);
		} else {
			snprintf(msg, sizeof(msg), "Done");
			spin_stop(&ctx->spin, SPIN_STATE_COMPLETE, msg);
		}
	}
	if (ctx->last_tool_was_plan && content && *content
	    && strncmp(content, "image generated:", 15) != 0
	    && strncmp(content, "video generated:", 16) != 0) {
		printf("\n");
		if (!render_plan_observation(content))
			print_tool_observation(content);
		fflush(stdout);
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
	if (ctx->streaming) {
		spin_set_sub(&ctx->spin, NULL);
		ctx->streaming = 0;
	}
	spin_pause(&ctx->spin);
	printf("\r\033[K");
	printf(ANSI_BOLD ANSI_CYAN "🛡 Guardrail" ANSI_RESET " %s\n",
	       content ? content : "");
	fflush(stdout);
	spin_resume(&ctx->spin);
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
	if (ctx->streaming) {
		spin_set_sub(&ctx->spin, NULL);
		ctx->streaming = 0;
	}
	if (ctx->spin.running) {
		char msg[128];
		if (content && *content) {
			snprintf(msg, sizeof(msg), "Done");
		} else {
			snprintf(msg, sizeof(msg), "No output");
		}
		spin_stop(&ctx->spin, SPIN_STATE_COMPLETE, msg);
		printf("\n");
	}
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
 * type - Step type (THOUGHT, ACTION, OBSERVATION, REFLECTION, FINAL).
 * content - Step content string.
 * user_data - Pointer to cli_context.
 *
 * Returns 0 always.
 */
int output_callback(enum react_step_type type, const char *content,
			   void *user_data)
{
	struct cli_context *ctx = user_data;

	switch (type) {
	case REACT_STEP_THOUGHT:
		return output_handle_thought(ctx, content);
	case REACT_STEP_ACTION:
		return output_handle_action(ctx, content);
	case REACT_STEP_OBSERVATION:
		return output_handle_observation(ctx, content);
	case REACT_STEP_REFLECTION:
		return output_handle_reflection(ctx, content);
	case REACT_STEP_FINAL:
		return output_handle_final(ctx, content);
	case REACT_STEP_REASONING:
		return output_handle_reasoning(ctx, content);
	}
	return 0;
}


int cli_ask_user_callback(const char *question,
				  const char *const *choices,
				  int choices_count,
				  char **answer,
				  void *user_data)
{
	struct cli_context *ctx = user_data;
	if (!ctx || !answer)
		return -EINVAL;

	spin_pause(&ctx->spin);

	printf("\r\033[K");
	printf(ANSI_BOLD ANSI_CYAN "? %s" ANSI_RESET "\n", question);

	char prompt[64];
	if (choices && choices_count > 0) {
		for (int i = 0; i < choices_count; i++)
			printf("  %d. %s\n", i + 1, choices[i]);
		snprintf(prompt, sizeof(prompt),
			 "  [" ANSI_GREEN "1-%d" ANSI_RESET "]: ",
			 choices_count);
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
			spin_resume(&ctx->spin);
			*answer = strdup("");
			return -EIO;
		}
		if (input[0] != '\0')
			add_history(input);
		if (choices && choices_count > 0) {
			int n = atoi(input);
			if (n >= 1 && n <= choices_count) {
				*answer = strdup(choices[n - 1]);
				free(input);
			} else {
				*answer = input;
			}
		} else {
			*answer = input;
		}
	}
#else
	fflush(stdout);
	char buf[1024] = {0};
	FILE *tty = fopen("/dev/tty", "r");
	if (!tty) {
		printf("\n");
		spin_resume(&ctx->spin);
		*answer = strdup("");
		return -ENOTTY;
	}
	if (!fgets(buf, sizeof(buf), tty)) {
		fclose(tty);
		printf("\n");
		spin_resume(&ctx->spin);
		*answer = strdup("");
		return -EIO;
	}
	fclose(tty);

	buf[strcspn(buf, "\n")] = '\0';

	if (choices && choices_count > 0) {
		int n = atoi(buf);
		if (n >= 1 && n <= choices_count)
			*answer = strdup(choices[n - 1]);
		else
			*answer = strdup(buf);
	} else {
		*answer = strdup(buf);
	}
#endif

	spin_resume(&ctx->spin);
	return 0;
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
#ifdef HAVE_READLINE
	char *rl_input = readline("  [y]es / [n]o / [a]lways: ");
	if (!rl_input) {
		printf("\n");
		return 0;
	}
	int v;
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
		return 0;
	}
	if (!fgets(buf, sizeof(buf), tty)) {
		fclose(tty);
		printf("\n");
		return 0;
	}
	fclose(tty);

	int v;
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
	return v;
}

enum hitl_verdict hitl_approval_callback(const char *tool_name,
						const char *tool_args,
						void *user_data)
{
	struct cli_context *ctx = user_data;
	if (!ctx)
		return HITL_DENY;

	spin_pause(&ctx->spin);

	printf("\r\033[K");
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

	spin_pause(&ctx->spin);

	printf("\r\033[K");
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
