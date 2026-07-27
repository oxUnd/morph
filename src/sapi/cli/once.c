#include "sapi/cli/internal.h"

static int cli_prepare_one_shot_session(struct cli_context *ctx)
{
	char name[256];
	struct session session;
	time_t now;
	int rc = -EEXIST;

	if (!ctx)
		MORPH_RETURN(-EINVAL);
	now = time(NULL);
	for (int i = 0; i < 100 && rc == -EEXIST; i++) {
		if (i == 0) {
			snprintf(name, sizeof(name), "one_shot_%lld",
				 (long long)now);
		} else {
			snprintf(name, sizeof(name), "one_shot_%lld_%d",
				 (long long)now, i);
		}
		rc = runtime_session_create_and_select(ctx->runtime, name,
						      &session);
	}
	if (rc != 0)
		return rc;
	utf8_sanitize_inplace(session.name);
	ctx->session_auto_named = 0;
	return 0;
}

static void emit_trace_json(struct cli_context *ctx, double elapsed)
{
	struct runtime_turn_status status;
	cJSON *root;
	cJSON *steps;
	char *json;

	if (runtime_turn_status_get(ctx->runtime, &status) != 0)
		return;
	root = cJSON_CreateObject();
	if (!root) {
		runtime_turn_status_cleanup(&status);
		return;
	}
	switch (status.state) {
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
	cJSON_AddStringToObject(root, "final_answer",
				status.final_answer ? status.final_answer : "");
	cJSON_AddStringToObject(root, "outcome",
				react_outcome_name(status.outcome));
	if (status.last_error_code < 0) {
		cJSON_AddNumberToObject(root, "error_code",
					status.last_error_code);
		cJSON_AddStringToObject(root, "error",
					morph_strerror(status.last_error_code));
	}
	if (status.outcome_reason && status.outcome_reason[0])
		cJSON_AddStringToObject(root, "reason",
					status.outcome_reason);
	steps = cJSON_CreateArray();
	if (steps) {
		for (int i = 0; i < status.step_count; i++) {
			struct runtime_trace_step *cur = &status.steps[i];
			cJSON *step = cJSON_CreateObject();

			if (!step)
				continue;
			cJSON_AddStringToObject(step, "type",
						react_step_type_name(cur->type));
			if (cur->content)
				cJSON_AddStringToObject(step, "content",
							cur->content);
			if (cur->tool_name)
				cJSON_AddStringToObject(step, "tool_name",
							cur->tool_name);
			if (cur->tool_args)
				cJSON_AddStringToObject(step, "tool_args",
							cur->tool_args);
			cJSON_AddItemToArray(steps, step);
		}
		cJSON_AddItemToObject(root, "steps", steps);
	}
	cJSON_AddNumberToObject(root, "elapsed_seconds", elapsed);
	json = cJSON_PrintUnformatted(root);
	if (json) {
		printf("%s\n", json);
		free(json);
	}
	cJSON_Delete(root);
	runtime_turn_status_cleanup(&status);
}

void cli_run_once(struct cli_context *ctx, const char *prompt)
{
	struct sigaction action;
	struct timespec started;
	struct timespec finished;
	int saved_stdout = -1;
	int rc;
	double elapsed;

	if (!ctx || !prompt)
		return;
	rc = cli_prepare_one_shot_session(ctx);
	if (rc != 0) {
		log_err("failed to create one-shot session: %s",
			morph_strerror(rc));
		fprintf(stderr, "failed to create one-shot session: %s\n",
			morph_strerror(rc));
		return;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = cli_sigint_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);
	clock_gettime(CLOCK_MONOTONIC, &started);
	cli_sigint_received = 0;
	runtime_cancel_turn(ctx->runtime);

	/*
	 * Trace mode keeps its historical contract: progress goes to stderr
	 * and the single aggregate JSON document is written to stdout.
	 */
	if (ctx->trace_json) {
		saved_stdout = dup(STDOUT_FILENO);
		if (saved_stdout >= 0)
			(void)dup2(STDERR_FILENO, STDOUT_FILENO);
	}
	(void)cli_handle_command(ctx, prompt);
	if (saved_stdout >= 0) {
		fflush(stdout);
		(void)dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
	}
	clock_gettime(CLOCK_MONOTONIC, &finished);
	elapsed = (double)(finished.tv_sec - started.tv_sec) +
		(double)(finished.tv_nsec - started.tv_nsec) / 1e9;
	if (ctx->trace_json)
		emit_trace_json(ctx, elapsed);
	signal(SIGINT, SIG_DFL);
}
