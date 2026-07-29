#include "sapi/cli/internal.h"

static int cli_markdown_write(const char *bytes, size_t len, void *user)
{
	size_t i;

	(void)user;
	if (!bytes || len == 0)
		return 0;
	if (cli_color_enabled())
		return fwrite(bytes, 1u, len, stdout) == len ? 0 : -EIO;
	for (i = 0u; i < len;) {
		if ((unsigned char)bytes[i] == 0x1bu &&
		    i + 1u < len && bytes[i + 1u] == '[') {
			size_t end = i + 2u;

			while (end < len &&
			       ((unsigned char)bytes[end] < 0x40u ||
				(unsigned char)bytes[end] > 0x7eu))
				end++;
			if (end < len && bytes[end] == 'm') {
				i = end + 1u;
				continue;
			}
		}
		if (fputc((unsigned char)bytes[i], stdout) == EOF)
			return -EIO;
		i++;
	}
	return 0;
}

static int cli_terminal_supports_kitty(void)
{
	const char *term;

	if (getenv("KITTY_WINDOW_ID"))
		return 1;
	term = getenv("TERM");
	return term && strstr(term, "kitty");
}

static struct morph_md_kitty *cli_markdown_create(unsigned int indent,
						   unsigned int initial_column,
						   int enable_math,
						   cli_markdown_media_cb cb,
						   void *user)
{
	struct morph_md_kitty_options options;

	memset(&options, 0, sizeof(options));
	options.font_path = MORPH_MARKDOWN_FONT_PATH;
	options.features = MORPH_MD_FEATURE_GFM;
	if (enable_math && cli_color_enabled() &&
	    cli_terminal_supports_kitty())
		options.features |= MORPH_MD_FEATURE_MATH;
	options.write = cli_markdown_write;
	options.media = cb;
	options.media_user_data = user;
	options.terminal_fd = STDOUT_FILENO;
	options.content_padding_left_columns = indent;
	options.initial_cursor_column = initial_column;
	return morph_md_kitty_create(&options);
}

static void cli_markdown_render(const char *md, unsigned int indent,
				unsigned int initial_column,
				cli_markdown_media_cb cb, void *user)
{
	struct morph_md_kitty *renderer;
	char *normalized;
	const char *content;
	int rc;

	if (!md)
		return;
	normalized = agent_ui_normalize_markdown(md);
	content = normalized ? normalized : md;
	renderer = cli_markdown_create(indent, initial_column, 1, cb, user);
	if (!renderer) {
		log_warn("failed to initialize Kitty Markdown renderer");
		goto out;
	}
	rc = morph_md_kitty_append(renderer, content, strlen(content), 1);
	if (rc == 0)
		rc = morph_md_kitty_render(renderer);
	if (rc != 0)
		log_warn("Kitty Markdown rendering failed");
	morph_md_kitty_destroy(renderer);
	fflush(stdout);
out:
	free(normalized);
}

int cli_markdown_stream_append(struct cli_context *ctx, const char *delta,
			       int kind)
{
	int rc;

	if (!ctx || !delta || !delta[0])
		return 0;
	if (ctx->markdown_stream && ctx->markdown_stream_kind != kind)
		cli_markdown_stream_reset(ctx, 1);
	if (!ctx->markdown_stream) {
		ctx->markdown_stream = cli_markdown_create(
			2u, 2u, 1, media_callback, ctx);
		if (!ctx->markdown_stream)
			MORPH_RETURN(-ENOMEM);
		ctx->markdown_stream_kind = kind;
		ctx->markdown_stream_visible = 1;
	}
	rc = morph_buf_puts(&ctx->markdown_stream_text, delta);
	if (rc != 0)
		return rc;
	rc = morph_md_kitty_append(ctx->markdown_stream, delta,
				   strlen(delta), 0);
	if (rc != 0)
		return rc;
	return morph_md_kitty_render(ctx->markdown_stream);
}

void cli_markdown_stream_reset(struct cli_context *ctx, int finish_output)
{
	if (!ctx)
		return;
	if (finish_output && ctx->markdown_stream) {
		if (morph_md_kitty_append(
			    ctx->markdown_stream, NULL, 0u, 1) == 0)
			(void)morph_md_kitty_render(ctx->markdown_stream);
	}
	morph_md_kitty_destroy(ctx->markdown_stream);
	ctx->markdown_stream = NULL;
	ctx->markdown_stream_kind = 0;
	ctx->markdown_stream_visible = 0;
	morph_buf_reset(&ctx->markdown_stream_text);
}

void media_callback(const char *type, const char *path, void *user)
{
	struct cli_context *ctx = (struct cli_context *)user;

	if (ctx && path && path[0]) {
		if (morph_strmap_contains(&ctx->rendered_artifacts, path))
			return;
		(void)morph_strmap_set(&ctx->rendered_artifacts, path,
				       (void *)1);
	}
	if (strcmp(type, "image") == 0) {
		image_render_terminal(path);
	} else if (strcmp(type, "video") == 0) {
		const char *mpv = (ctx && (*runtime_config_get(ctx->runtime)).render.mpv_args[0])
				  ? (*runtime_config_get(ctx->runtime)).render.mpv_args : NULL;
		video_play(path, mpv);
	}
}

void cli_markdown_render_ansi(const char *md)
{
	cli_markdown_render(md, 0u, 0u, NULL, NULL);
}

void cli_markdown_render_ansi_with_media(const char *md,
						cli_markdown_media_cb cb,
						void *user)
{
	cli_markdown_render(md, 0u, 0u, cb, user);
}

void cli_markdown_render_ansi_with_media_indented(const char *md,
						  unsigned int indent,
						  cli_markdown_media_cb cb,
						  void *user)
{
	cli_markdown_render(md, indent, indent, cb, user);
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

	cli_presentation_prepare_prompt(ctx);
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
 * scoped - Whether to offer separate session and persistent choices.
 */
static int prompt_approval(const char *subject, int scoped)
{
	static pthread_mutex_t prompt_lock = PTHREAD_MUTEX_INITIALIZER;
	int v;

	pthread_mutex_lock(&prompt_lock);
#ifdef HAVE_READLINE
	char *rl_input = readline(scoped ?
		"  [y]es once / [s]ession / [a]lways / [n]o: " :
		"  [y]es / [n]o / [a]lways: ");
	if (!rl_input) {
		printf("\n");
		pthread_mutex_unlock(&prompt_lock);
		return 0;
	}
	if (rl_input[0] == 'a' || rl_input[0] == 'A')
		v = scoped ? 3 : 2;
	else if (scoped &&
		 (rl_input[0] == 's' || rl_input[0] == 'S'))
		v = 2;
	else if (rl_input[0] == 'y' || rl_input[0] == 'Y')
		v = 1;
	else
		v = 0;
	free(rl_input);
#else
	if (scoped)
		printf("  [" ANSI_GREEN "y" ANSI_RESET "]es once / "
		       "[s]ession / [a]lways / ["
		       ANSI_RED "n" ANSI_RESET "]o: ");
	else
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
		v = scoped ? 3 : 2;
	else if (scoped && (buf[0] == 's' || buf[0] == 'S'))
		v = 2;
	else if (buf[0] == 'y' || buf[0] == 'Y')
		v = 1;
	else
		v = 0;
#endif

	if (v > 0)
		printf(ANSI_BOLD ANSI_GREEN "  ✓ Approved" ANSI_RESET " (%s%s)\n",
		       subject ? subject : "",
		       v == 2 ? ", session" : (v == 3 ? ", always" : ""));
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

	cli_presentation_prepare_prompt(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("approval: %s\n", tool_name);
	} else {
		printf("\n" ANSI_BOLD ANSI_YELLOW
		       "╭─ Approval required ─────────────────────────────"
		       ANSI_RESET "\n");
		printf(ANSI_YELLOW "│" ANSI_RESET " Tool     "
		       ANSI_BOLD "%s" ANSI_RESET "\n", tool_name);
	}

	if (tool_args && *tool_args && strcmp(tool_args, "{}") != 0) {
		char display_args[512];
		utf8_copy_sanitized_display_width(display_args,
						  sizeof(display_args),
						  tool_args, 200);
		if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
			printf("args: %s\n", display_args);
		else
			printf(ANSI_YELLOW "│" ANSI_RESET " Args     "
			       ANSI_DIM "%s" ANSI_RESET "\n", display_args);
	}
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
		printf(ANSI_YELLOW
		       "╰─────────────────────────────────────────────────"
		       ANSI_RESET "\n");

	int v = prompt_approval(tool_name, 0);
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

	cli_presentation_prepare_prompt(ctx);
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN) {
		printf("approval: %s\n", operation_label(op->kind));
	} else {
		printf("\n" ANSI_BOLD ANSI_YELLOW
		       "╭─ %s ─────────────────────────────"
		       ANSI_RESET "\n", operation_label(op->kind));
	}
	if (op->tool_name && *op->tool_name)
		printf("%sTool     " ANSI_BOLD "%s" ANSI_RESET "\n",
		       ctx->presentation_mode == CLI_PRESENT_INTERACTIVE ?
		       ANSI_YELLOW "│ " ANSI_RESET : "",
		       op->tool_name);
	if (op->principal && *op->principal)
		printf("%sSubject  " ANSI_BOLD "%s" ANSI_RESET "\n",
		       ctx->presentation_mode == CLI_PRESENT_INTERACTIVE ?
		       ANSI_YELLOW "│ " ANSI_RESET : "",
		       op->principal);
	if (op->kind == TOOL_OP_COMMAND) {
		const char *command = op->action;
		if (command) {
			char display[512];
			utf8_copy_sanitized_display_width(display,
							  sizeof(display),
							  command, 380);
			printf("%sCommand  " ANSI_BOLD "%s" ANSI_RESET "\n",
			       ctx->presentation_mode ==
				       CLI_PRESENT_INTERACTIVE ?
			       ANSI_YELLOW "│ " ANSI_RESET : "",
			       display);
		}
	} else if (op->target && *op->target) {
		printf("%sTarget   " ANSI_BOLD "%s" ANSI_RESET "\n",
		       ctx->presentation_mode == CLI_PRESENT_INTERACTIVE ?
		       ANSI_YELLOW "│ " ANSI_RESET : "",
		       op->target);
	}
	if (op->scope && *op->scope)
		printf("%s%-8s " ANSI_DIM "%s" ANSI_RESET "\n",
		       ctx->presentation_mode == CLI_PRESENT_INTERACTIVE ?
		       ANSI_YELLOW "│ " ANSI_RESET : "",
		       operation_scope_label(op->kind), op->scope);
	if (op->kind == TOOL_OP_COMMAND)
		printf("%s" ANSI_DIM "'session' trusts this program and cwd "
		       "until exit; 'always' remembers it for this project."
		       ANSI_RESET "\n",
		       ctx->presentation_mode == CLI_PRESENT_INTERACTIVE ?
		       ANSI_YELLOW "│ " ANSI_RESET : "");
	else
		printf("%s" ANSI_DIM "'session' trusts this scope until exit; "
		       "'always' remembers it for this project."
		       ANSI_RESET "\n",
		       ctx->presentation_mode == CLI_PRESENT_INTERACTIVE ?
		       ANSI_YELLOW "│ " ANSI_RESET : "");
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE)
		printf(ANSI_YELLOW
		       "╰─────────────────────────────────────────────────"
		       ANSI_RESET "\n");

	int v = prompt_approval(operation_subject(op->kind), 1);
	if (v == 3)
		return TOOL_OP_ALWAYS;
	if (v == 2)
		return TOOL_OP_SESSION;
	if (v == 1)
		return TOOL_OP_ALLOW;
	return TOOL_OP_DENY;
}
