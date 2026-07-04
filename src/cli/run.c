#include "cli/internal.h"
#include "cli/commands/registry.h"
#include "http/client.h"

/* ---- sigint ---- */

volatile sig_atomic_t cli_sigint_received = 0;

static void sigint_handler(int sig)
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
		session_list(&g_comp_ctx->database, &slist, &scount, 0, NULL);
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

/* ---- cli_run_once ---- */

static int cli_prepare_one_shot_session(struct cli_context *ctx)
{
	char name[256];
	struct session s;
	time_t now;
	int rc = -EEXIST;

	if (!ctx)
		return -EINVAL;
	now = time(NULL);
	for (int i = 0; i < 100 && rc == -EEXIST; i++) {
		if (i == 0) {
			snprintf(name, sizeof(name), "one_shot_%lld",
				 (long long)now);
		} else {
			snprintf(name, sizeof(name), "one_shot_%lld_%d",
				 (long long)now, i);
		}
		rc = session_create(&ctx->database, name,
				    ctx->config.models.text.model, &s);
	}
	if (rc != 0)
		return rc;
	ctx->current_session = s;
	utf8_sanitize_inplace(ctx->current_session.name);
	session_load_history(ctx);
	cli_update_tool_runtime_context(ctx);
	ctx->session_auto_named = 0;
	return 0;
}

static void emit_trace_json(struct cli_context *ctx, double elapsed)
{
	if (!ctx->react)
		return;
	cJSON *root = cJSON_CreateObject();
	switch (ctx->react->state) {
	case REACT_STATE_DONE:
		cJSON_AddStringToObject(root, "state", "done");
		break;
	case REACT_STATE_ABORT:
		cJSON_AddStringToObject(root, "state", "abort");
		break;
	case REACT_STATE_TOOL_FAIL:
		cJSON_AddStringToObject(root, "state", "tool_fail");
		break;
	default:
		cJSON_AddStringToObject(root, "state", "unknown");
		break;
	}
	if (ctx->react->final_answer)
		cJSON_AddStringToObject(root, "final_answer",
					ctx->react->final_answer);
	else
		cJSON_AddStringToObject(root, "final_answer", "");
	cJSON_AddStringToObject(root, "outcome",
				react_outcome_name(ctx->react->outcome));
	if (ctx->react->last_error_code < 0) {
		cJSON_AddNumberToObject(root, "error_code",
					ctx->react->last_error_code);
		cJSON_AddStringToObject(root, "error",
					morph_strerror(ctx->react->last_error_code));
	}
	if (ctx->react->outcome_reason[0])
		cJSON_AddStringToObject(root, "reason",
					ctx->react->outcome_reason);
	cJSON *steps = cJSON_CreateArray();
	struct react_step *cur = ctx->react->steps;
	while (cur) {
		cJSON *s = cJSON_CreateObject();
		cJSON_AddStringToObject(s, "type",
					react_step_type_name(cur->type));
		if (cur->content)
			cJSON_AddStringToObject(s, "content", cur->content);
		if (cur->tool_name)
			cJSON_AddStringToObject(s, "tool_name", cur->tool_name);
		if (cur->tool_args)
			cJSON_AddStringToObject(s, "tool_args", cur->tool_args);
		cJSON_AddItemToArray(steps, s);
		cur = cur->next;
	}
	cJSON_AddItemToObject(root, "steps", steps);
	cJSON_AddNumberToObject(root, "elapsed_seconds", elapsed);
	char *json = cJSON_PrintUnformatted(root);
	printf("%s\n", json);
	free(json);
	cJSON_Delete(root);
}

void cli_run_once(struct cli_context *ctx, const char *prompt)
{
	int rc;

	if (!ctx || !prompt)
		return;
	rc = cli_prepare_one_shot_session(ctx);
	if (rc != 0) {
		log_err("failed to create one-shot session: %s",
			morph_strerror(rc));
		if (rc == MORPH_ERR_DB && ctx->database.handle) {
			fprintf(stderr,
				"failed to create one-shot session: %s: %s\n",
				morph_strerror(rc),
				sqlite3_errmsg(ctx->database.handle));
		} else {
			fprintf(stderr, "failed to create one-shot session: %s\n",
				morph_strerror(rc));
		}
		return;
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	struct timespec ts_start, ts_end;
	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	cli_sigint_received = 0;
	if (ctx->react)
		react_cancel(ctx->react);
	/*
	 * In trace-json mode, redirect stdout to stderr so that
	 * only the JSON trace appears on stdout for machine parsing.
	 */
	int saved_stdout = -1;
	if (ctx->trace_json) {
		saved_stdout = dup(STDOUT_FILENO);
		dup2(STDERR_FILENO, STDOUT_FILENO);
	}
	cli_handle_command(ctx, prompt);
	if (saved_stdout >= 0) {
		fflush(stdout);
		dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
	}
	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	double elapsed = (double)(ts_end.tv_sec - ts_start.tv_sec)
			 + (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
	if (ctx->trace_json)
		emit_trace_json(ctx, elapsed);
	signal(SIGINT, SIG_DFL);
}

/* ---- cli_run ---- */

void cli_run(struct cli_context *ctx)
{
	if (!ctx)
		return;
	if (cli_scheduler_start(ctx) != 0)
		log_warn("failed to start task scheduler");
	printf("morph v" MORPH_VERSION "  |  " ANSI_DIM "/help 查看命令" ANSI_RESET "\n\n");
	char line[8192];

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

#ifdef HAVE_READLINE
	using_history();
	g_comp_ctx = ctx;
	rl_attempted_completion_function = cmd_completion;
	while (ctx->running) {
		char prompt[512];
		snprintf(prompt, sizeof(prompt), ANSI_GREEN "[%s]" ANSI_RESET " $ ",
			 ctx->current_session.display_id);
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
		printf(ANSI_GREEN "[%s]" ANSI_RESET " $ ", ctx->current_session.display_id);
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
