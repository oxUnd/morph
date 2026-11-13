#include "sapi/cli/services/session_restore.h"
#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/presentation/history.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/widgets/feed.h"
#include "runtime/runtime.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/cleanup.h"
#include <stdlib.h>
#include <string.h>

void cli_session_history_print_resume_hint(struct cli_application *ctx)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	struct session session;
	const char *config_path;
	const char *workdir;
	char *resolved_config NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (!ctx || !ctx->runtime || ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return;
	if (runtime_session_current(ctx->runtime, &session) != 0 || session.id <= 0)
		return;
	config_path = runtime_config_path_borrow(ctx->runtime);
	resolved_config =
		config_path && config_path[0]
			? (nb_path_expand(&file_value_1, config_path, NULL) == NB_OK &&
					   nb_path_canonical_missing(&file_value_1,
								     nb_buf_cstr(&file_value_1),
								     NULL) == NB_OK
				   ? strdup(nb_buf_cstr(&file_value_1))
				   : NULL)
			: NULL;
	workdir = runtime_workdir_borrow(ctx->runtime);
	cli_history_render_resume_hint(ctx, session.display_id,
				       resolved_config ? resolved_config : config_path,
				       workdir);
}

int cli_session_history_restore(struct cli_application *ctx)
{
	struct model_history_item *items;
	struct message *messages = NULL;
	int count = 0;
	int rc;

	if (!ctx || !ctx->runtime)
		return NB_ERR_INVAL;
	/* Session commands retain their existing plain and NDJSON contracts. */
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return 0;
	/* Submitted commands and existing scrollback remain visible on session changes. */
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
