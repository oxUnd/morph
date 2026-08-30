#include "sapi/cli/internal.h"

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
	struct cli_command_job job;
	struct sigaction action;
	struct timespec started;
	struct timespec finished;
	int saved_stdout = -1;
	int job_initialized = 0;
	int job_rc;
	double elapsed;

	if (!ctx || !prompt)
		return;
	memset(&action, 0, sizeof(action));
	action.sa_handler = cli_sigint_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL);
	clock_gettime(CLOCK_MONOTONIC, &started);
	cli_cancel_state_reset();

	/*
	 * Trace mode keeps its historical contract: progress goes to stderr
	 * and the single aggregate JSON document is written to stdout.
	 */
	if (ctx->trace_json) {
		saved_stdout = dup(STDOUT_FILENO);
		if (saved_stdout >= 0)
			(void)dup2(STDERR_FILENO, STDOUT_FILENO);
	}
	cli_turn_begin(ctx);
	job_rc = cli_command_job_init(&job);
	if (job_rc == 0) {
		job_initialized = 1;
		job_rc = cli_command_job_start(&job, ctx, prompt);
	}
	if (job_rc == 0)
		job_rc = cli_command_job_wait(&job);
	(void)cli_ui_drain(ctx);
	cli_turn_finish(ctx, job_rc);
	if (job_initialized)
		cli_command_job_cleanup(&job);
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
