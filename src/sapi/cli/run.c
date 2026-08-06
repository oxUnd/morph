#include "sapi/cli/internal.h"
#include "sapi/cli/commands/registry.h"
#include "http/client.h"

#define CLI_BANNER_INNER_WIDTH 60
#define CLI_BANNER_CONTENT_WIDTH (CLI_BANNER_INNER_WIDTH - 2)
#define CLI_BANNER_LABEL_WIDTH 12
#define CLI_BANNER_HINT_WIDTH 18

/* ---- sigint ---- */

volatile sig_atomic_t cli_sigint_received = 0;

void cli_sigint_handler(int sig)
{
	(void)sig;
	react_sigint_flag = 1;
	http_cancel_from_signal();
	cli_sigint_received = 1;
	if (write(STDOUT_FILENO, "\n", 1) < 0) {
		/* ignore */
	}
}

#ifdef HAVE_READLINE

static struct cli_context *g_comp_ctx;
static char *g_readline_ready_input;

static void cli_readline_line_ready(char *input)
{
	struct cli_context *ctx = g_comp_ctx;

	if (!ctx) {
		free(input);
		return;
	}
	if (!input) {
		ctx->running = 0;
		return;
	}
	free(g_readline_ready_input);
	g_readline_ready_input = input;
}

static void cli_readline_drain_ui(struct cli_context *ctx)
{
	printf("\r\033[2K");
	fflush(stdout);
	(void)cli_ui_drain(ctx);
	rl_on_new_line();
	rl_forced_update_display();
}

static int cli_readline_insert_newline(int count, int key)
{
	(void)key;
	for (int i = 0; i < count; i++) {
		if (rl_insert_text("\n") != 0)
			return 1;
	}
	rl_redisplay();
	return 0;
}

static int cli_readline_paste_image(int count, int key)
{
	char *path = NULL;
	int rc;

	(void)count;
	(void)key;
	putchar('\n');
	if (!g_comp_ctx) {
		putchar('\a');
		rl_on_new_line();
		rl_redisplay();
		return 1;
	}
	rc = cli_clipboard_save_image(g_comp_ctx, &path);
	if (rc == 0)
		rc = cli_attach_image(g_comp_ctx, path);
	if (rc != 0) {
		if (path)
			(void)unlink(path);
		CMD_ERROR("clipboard does not contain a supported image");
		putchar('\a');
	}
	free(path);
	rl_on_new_line();
	rl_forced_update_display();
	return rc == 0 ? 0 : 1;
}

static void cli_readline_configure(void)
{
	(void)rl_variable_bind("enable-bracketed-paste", "on");
	(void)rl_bind_key('\n', cli_readline_insert_newline);
	(void)rl_bind_key(0x16, cli_readline_paste_image);
#ifdef HAVE_RL_BIND_KEYSEQ
	(void)rl_bind_keyseq("\\e\\C-M", cli_readline_insert_newline);
	(void)rl_bind_keyseq("\033[13;2u", cli_readline_insert_newline);
	(void)rl_bind_keyseq("\033[27;2;13~", cli_readline_insert_newline);
#endif
}

static char *session_completion_generator(const char *text, int state)
{
	static struct session *slist;
	static int scount;
	static int idx;
	static int len;

	if (state == 0) {
		if (slist) {
			free(slist);
			slist = NULL;
		}
		scount = 0;
		idx = 0;
		len = (int)strlen(text);
		if (!g_comp_ctx)
			return NULL;
		(void)runtime_session_list_query(g_comp_ctx->runtime, &slist,
						 &scount, 0, NULL);
	}

	while (idx < scount) {
		struct session *s = &slist[idx];
		idx++;
		if (s->display_id[0] &&
		    strncmp(s->display_id, text, (size_t)len) == 0)
			return strdup(s->display_id);
		if (s->name[0] &&
		    strncmp(s->name, text, (size_t)len) == 0)
			return strdup(s->name);
	}

	if (slist) {
		free(slist);
		slist = NULL;
	}
	scount = 0;
	return NULL;
}

static int is_session_arg_command(const char *cmd)
{
	return (strcmp(cmd, "/switch") == 0 ||
		strcmp(cmd, "/s") == 0 ||
		strcmp(cmd, "/delete") == 0 ||
		strcmp(cmd, "/del") == 0);
}

static char **cmd_completion(const char *text, int start, int end)
{
	(void)end;
	rl_attempted_completion_over = 1;
	if (start == 0)
		return rl_completion_matches(text, cli_command_completion_generator);

	char *cmd = strndup(rl_line_buffer, (size_t)(start - 1));
	int match = is_session_arg_command(cmd);
	free(cmd);
	if (match)
		return rl_completion_matches(text, session_completion_generator);

	return NULL;
}

#endif

const char *cli_input_prompt(void)
{
	if (!cli_color_enabled())
		return "> ";
	return CLI_RL_IGNORE_START ANSI_BOLD ANSI_CYAN CLI_RL_IGNORE_END
		"› "
		CLI_RL_IGNORE_START ANSI_RESET CLI_RL_IGNORE_END;
}

static void cli_print_banner_title(void)
{
	size_t used;
	int pad;

	used = utf8_display_width(">_ ") + utf8_display_width("morph") +
		2 + utf8_display_width("(v)") +
		utf8_display_width(MORPH_VERSION);
	pad = CLI_BANNER_CONTENT_WIDTH -
		(used > (size_t)INT_MAX ? INT_MAX : (int)used);
	printf(ANSI_CYAN "│" ANSI_RESET ANSI_DIM " >_ "
	       ANSI_RESET ANSI_BOLD "morph" ANSI_RESET
	       "  " ANSI_DIM "(v%s)" ANSI_RESET, MORPH_VERSION);
	for (int i = 0; i < pad; i++)
		putchar(' ');
	printf(ANSI_CYAN " │" ANSI_RESET "\n");
}

static void cli_print_banner_blank(void)
{
	printf(ANSI_CYAN "│" ANSI_RESET);
	for (int i = 0; i < CLI_BANNER_INNER_WIDTH; i++)
		putchar(' ');
	printf(ANSI_CYAN "│" ANSI_RESET "\n");
}

static void cli_print_banner_field(const char *label, const char *value,
				   const char *hint, int keep_tail)
{
	char clipped[BUFSIZ];
	int hint_width = hint ? CLI_BANNER_HINT_WIDTH : 0;
	int value_width = CLI_BANNER_CONTENT_WIDTH -
		CLI_BANNER_LABEL_WIDTH - hint_width;

	(void)utf8_copy_ellipsized_display_width(
		clipped, sizeof(clipped), value, (size_t)value_width,
		keep_tail);
	printf(ANSI_CYAN "│" ANSI_RESET " " ANSI_DIM);
	print_padded(label, CLI_BANNER_LABEL_WIDTH);
	printf(ANSI_RESET ANSI_BOLD);
	print_padded(clipped, value_width);
	printf(ANSI_RESET);
	if (hint) {
		printf(ANSI_DIM);
		print_padded(hint, CLI_BANNER_HINT_WIDTH);
		printf(ANSI_RESET);
	}
	printf(ANSI_CYAN " │" ANSI_RESET "\n");
}

/* ---- cli_run ---- */

void cli_run(struct cli_context *ctx)
{
	const struct config *config;
	const char *workdir;
	const char *home;
	struct session current;
	morph_buf_t directory;

	if (!ctx)
		return;
	if (cli_scheduler_start(ctx) != 0)
		log_warn("failed to start task scheduler");
	config = runtime_config_get(ctx->runtime);
	workdir = runtime_workdir_get(ctx->runtime);
	memset(&current, 0, sizeof(current));
	(void)runtime_session_current(ctx->runtime, &current);
	memset(&directory, 0, sizeof(directory));
	if (morph_buf_init(&directory, BUFSIZ) != 0)
		return;
	home = getenv("HOME");
	if (workdir && workdir[0] && home && home[0] &&
	    strncmp(workdir, home, strlen(home)) == 0 &&
	    (workdir[strlen(home)] == '\0' ||
	     workdir[strlen(home)] == '/')) {
		(void)morph_buf_putc(&directory, '~');
		(void)morph_buf_puts(&directory, workdir + strlen(home));
	} else {
		(void)morph_buf_puts(&directory,
				    workdir && workdir[0] ? workdir : ".");
	}
	printf("\n" ANSI_CYAN
	       "╭───────────────"
	       "───────────────"
	       "───────────────"
	       "───────────────╮\n"
	       ANSI_RESET);
	cli_print_banner_title();
	cli_print_banner_blank();
	cli_print_banner_field("model:",
		config ? config->models.text.model : "model", NULL, 0);
	cli_print_banner_field("session:",
		current.display_id[0] ? current.display_id : "session",
		"/switch to change", 0);
	cli_print_banner_field("directory:", morph_buf_cstr(&directory), NULL, 1);
	printf(ANSI_CYAN
	       "╰───────────────"
	       "───────────────"
	       "───────────────"
	       "───────────────╯"
	       ANSI_RESET "\n\n");
	morph_buf_cleanup(&directory);
	printf(ANSI_DIM "  /help · ./image.png attach · Ctrl+J/Alt+Enter newline\n"
	       "  Ctrl+V image · Esc/Ctrl-C cancel" ANSI_RESET "\n\n");
#ifndef HAVE_READLINE
	char line[BUFSIZ];
#endif

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = cli_sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

#ifdef HAVE_READLINE
	using_history();
	int callback_installed = 1;

	g_comp_ctx = ctx;
	g_readline_ready_input = NULL;
	rl_attempted_completion_function = cmd_completion;
	cli_readline_configure();
	rl_callback_handler_install(cli_input_prompt(),
				    cli_readline_line_ready);
	while (ctx->running) {
		struct pollfd fds[2];
		int nfds = 1;
		int wake_fd = cli_ui_wake_fd(ctx);
		int rc;

		fds[0].fd = STDIN_FILENO;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		if (wake_fd >= 0) {
			fds[1].fd = wake_fd;
			fds[1].events = POLLIN;
			fds[1].revents = 0;
			nfds = 2;
		}
		rc = poll(fds, (nfds_t)nfds, -1);
		if (rc < 0) {
			if (errno == EINTR) {
				if (cli_sigint_received) {
					cli_sigint_received = 0;
					rl_replace_line("", 0);
					rl_on_new_line();
					rl_forced_update_display();
				}
				continue;
			}
			CMD_ERROR("input polling failed: %s", strerror(errno));
			break;
		}
		if (nfds == 2 && (fds[1].revents & POLLIN))
			cli_readline_drain_ui(ctx);
		if (fds[0].revents & (POLLIN | POLLHUP)) {
			cli_cancel_state_reset();
			rl_callback_read_char();
			if (g_readline_ready_input) {
				char *input = g_readline_ready_input;

				g_readline_ready_input = NULL;
				rl_callback_handler_remove();
				callback_installed = 0;
				printf("\r\033[2K");
				fflush(stdout);
				if (input[0] != '\0') {
					add_history(input);
					(void)cli_handle_command(ctx, input);
					(void)cli_ui_drain(ctx);
				}
				free(input);
				if (ctx->running) {
					rl_callback_handler_install(
						cli_input_prompt(),
						cli_readline_line_ready);
					callback_installed = 1;
				}
			}
		}
	}
	if (g_readline_ready_input) {
		free(g_readline_ready_input);
		g_readline_ready_input = NULL;
	}
	if (callback_installed)
		rl_callback_handler_remove();
	g_comp_ctx = NULL;
#else
	morph_buf_t input;
	if (morph_buf_init(&input, BUFSIZ) != 0)
		return;
	while (ctx->running) {
		int complete = 0;

		(void)cli_ui_drain(ctx);
		morph_buf_clear(&input);
		while (!complete) {
			size_t len;
			int has_newline;
			int continuation;

			printf(input.len == 0 ?
				ANSI_BOLD ANSI_CYAN "› " ANSI_RESET :
				ANSI_DIM "… " ANSI_RESET);
			fflush(stdout);
			cli_cancel_state_reset();
			if (!fgets(line, sizeof(line), stdin)) {
				if (cli_sigint_received) {
					cli_sigint_received = 0;
					clearerr(stdin);
					morph_buf_clear(&input);
					break;
				}
				if (feof(stdin)) {
					ctx->running = 0;
					break;
				}
				clearerr(stdin);
				continue;
			}
			len = strlen(line);
			has_newline = len > 0 && line[len - 1] == '\n';
			if (has_newline)
				line[--len] = '\0';
			if (len > 0 && line[len - 1] == '\r')
				line[--len] = '\0';
			continuation = has_newline && len > 0 &&
				line[len - 1] == '\\';
			if (continuation)
				len--;
			if (morph_buf_append(&input, line, len) != 0) {
				CMD_ERROR("input is too large");
				morph_buf_clear(&input);
				break;
			}
			if (continuation) {
				if (morph_buf_putc(&input, '\n') != 0) {
					CMD_ERROR("input is too large");
					morph_buf_clear(&input);
					break;
				}
			} else if (has_newline) {
				complete = 1;
			}
		}
		if (input.len > 0)
			cli_handle_command(ctx, morph_buf_cstr(&input));
		(void)cli_ui_drain(ctx);
	}
	morph_buf_cleanup(&input);
#endif
	signal(SIGINT, SIG_DFL);
}
