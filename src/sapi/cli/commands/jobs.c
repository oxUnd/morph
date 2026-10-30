#include "sapi/cli/commands/registry.h"
#include "sapi/cli/jobs_ui.h"

static int cmd_jobs(struct cli_context *ctx, int argc, char **argv)
{
	const char *id = cli_cmd_arg(argc, argv, 1);
	if (id && !strcmp(id, "stop")) {
		id = cli_cmd_arg(argc, argv, 2);
		if (!id) {
			CMD_ERROR(ctx, "usage: /jobs stop <id>");
			return -EINVAL;
		}
		int rc = runtime_process_stop(ctx->runtime, id);
		if (!rc)
			CMD_OK(ctx, "stop requested: %s", id);
		else
			CMD_ERROR(ctx, "%s", morph_strerror(rc));
		return rc;
	}
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && isatty(STDIN_FILENO)) {
		int rc = cli_jobs_view(ctx, id);
		if (rc)
			CMD_ERROR(ctx, "%s", morph_strerror(rc));
		return rc;
	}
	cJSON *data = NULL;
	int rc = id ? runtime_process_read(ctx->runtime, id, &data)
		    : runtime_process_list(ctx->runtime, &data);
	if (!rc) {
		char *json = cJSON_Print(data);
		if (!json)
			rc = -ENOMEM;
		else {
			printf(ctx, "%s\n", json);
			free(json);
		}
	}
	cJSON_Delete(data);
	if (rc)
		CMD_ERROR(ctx, "%s", morph_strerror(rc));
	return rc;
}

struct cli_command_group cli_job_commands(void)
{
	static const struct cli_command commands[] = {
		{"/jobs", cmd_jobs, "Inspect background commands", "/jobs [id|stop id]"},
	};
	return (struct cli_command_group){commands, (int)(sizeof(commands) / sizeof(commands[0])),
					  "Runtime"};
}
