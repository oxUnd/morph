#include "sapi/cli/internal.h"
#include "sapi/cli/commands/registry.h"
#include "http/client.h"

#define CLI_BANNER_INNER_WIDTH 60

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

static void cli_print_banner_row(const char *text, const char *style)
{
	char clipped[BUFSIZ];

	utf8_copy_sanitized_display_width(
		clipped, sizeof(clipped), text ? text : "",
		CLI_BANNER_INNER_WIDTH - 2);
	printf(ANSI_CYAN "│" ANSI_RESET " %s", style ? style : "");
	print_padded(clipped, CLI_BANNER_INNER_WIDTH - 2);
	printf(ANSI_RESET " " ANSI_CYAN "│" ANSI_RESET "\n");
}

/* ---- cli_run ---- */

void cli_run(struct cli_context *ctx)
{
	const struct config *config;
	const char *workdir;
	struct session current;
	morph_buf_t title;
	morph_buf_t details;

	if (!ctx)
		return;
	if (cli_scheduler_start(ctx) != 0)
		log_warn("failed to start task scheduler");
	config = runtime_config_get(ctx->runtime);
	workdir = runtime_workdir_get(ctx->runtime);
	memset(&current, 0, sizeof(current));
	(void)runtime_session_current(ctx->runtime, &current);
	memset(&title, 0, sizeof(title));
	memset(&details, 0, sizeof(details));
	if (morph_buf_init(&title, 64) != 0)
		return;
	if (morph_buf_init(&details, BUFSIZ) != 0) {
		morph_buf_cleanup(&title);
		return;
	}
	(void)morph_buf_printf(&title, ">_ morph  v%s", MORPH_VERSION);
	(void)morph_buf_printf(&details, "%s  ·  %s  ·  %s",
		config ? config->models.text.model : "model",
		current.display_id[0] ? current.display_id : "session",
		workdir && workdir[0] ? workdir : ".");
	printf("\n" ANSI_CYAN
	       "╭────────────────────────────────────────────────────────────╮\n"
	       ANSI_RESET);
	cli_print_banner_row(morph_buf_cstr(&title), ANSI_BOLD);
	cli_print_banner_row(morph_buf_cstr(&details), ANSI_DIM);
	printf(ANSI_CYAN
	       "╰────────────────────────────────────────────────────────────╯"
	       ANSI_RESET "\n\n");
	morph_buf_cleanup(&title);
	morph_buf_cleanup(&details);
	printf(ANSI_DIM "  Type /help for commands." ANSI_RESET "\n\n");
	char line[8192];

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = cli_sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

#ifdef HAVE_READLINE
	using_history();
	g_comp_ctx = ctx;
	rl_attempted_completion_function = cmd_completion;
	while (ctx->running) {
		const char *prompt = cli_color_enabled() ?
			ANSI_BOLD ANSI_CYAN "› " ANSI_RESET : "> ";
		cli_sigint_received = 0;
		char *input = readline(prompt);
		if (!input) {
			if (cli_sigint_received) {
				cli_sigint_received = 0;
				continue;
			}
			if (feof(stdin))
				break;
			clearerr(stdin);
			printf("\n");
			continue;
		}
		if (input[0] != '\0') {
			add_history(input);
			strncpy(line, input, sizeof(line) - 1);
			line[sizeof(line) - 1] = '\0';
			cli_handle_command(ctx, line);
		}
		free(input);
	}
#else
	while (ctx->running) {
		printf(ANSI_BOLD ANSI_CYAN "› " ANSI_RESET);
		fflush(stdout);
		cli_sigint_received = 0;
		if (!fgets(line, sizeof(line), stdin)) {
			if (cli_sigint_received) {
				cli_sigint_received = 0;
				clearerr(stdin);
				continue;
			}
			if (feof(stdin))
				break;
			clearerr(stdin);
			continue;
		}
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '\0')
			continue;
		cli_handle_command(ctx, line);
	}
#endif
	signal(SIGINT, SIG_DFL);
}
