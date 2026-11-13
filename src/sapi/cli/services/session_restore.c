#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/history.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/widgets/feed.h"
#include "runtime/runtime.h"
#include "util/error.h"
#include "util/file.h"
#include <stdlib.h>
#include <string.h>

void cli_session_history_print_resume_hint(struct cli_application *ctx)
{
	struct session session;
	const char *config_path;
	const char *workdir;
	char *resolved_config;

	if (!ctx || !ctx->runtime || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (runtime_session_current(ctx->runtime, &session) != 0 || session.id <= 0)
		return;
	config_path = runtime_config_path_borrow(ctx->runtime);
	resolved_config =
		config_path && config_path[0] ? file_resolve_path_copy(config_path) : NULL;
	workdir = runtime_workdir_borrow(ctx->runtime);
	if (session.workdir[0] && workdir && strcmp(session.workdir, workdir) == 0)
		workdir = NULL;
	cli_history_render_resume_hint(ctx, session.display_id,
				       resolved_config ? resolved_config : config_path,
				       workdir);
	free(resolved_config);
}

int cli_session_history_restore(struct cli_application *ctx)
{
	struct model_history_item *items;
	struct message *messages = NULL;
	int count = 0;
	int rc;

	if (!ctx || !ctx->runtime)
		return -EINVAL;
	/* Session commands retain their existing plain and NDJSON contracts. */
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return 0;
	items = runtime_session_model_history_current_copy(ctx->runtime, 0, &count);
	if (!items)
		messages = runtime_session_messages_current_copy(ctx->runtime, &count);
	cli_feed_history_begin(ctx);
	cli_presentation_reset(ctx);
	rc = cli_history_render_session(ctx, items, messages);
	runtime_session_model_history_free(items);
	runtime_session_messages_free(messages);
	cli_feed_history_end(ctx);
	return rc;
}
