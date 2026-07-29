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
