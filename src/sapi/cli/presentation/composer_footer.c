#include "sapi/cli/presentation/composer_footer.h"
#include <nb/err.h>
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "sapi/cli/services/sessions.h"
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include "terminal/text.h"
#include "sapi/cli/presentation/terminal_text.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cli_composer_footer_field(struct cli_application *ctx, nb_buf *text, const char *value,
				     size_t width, const char *color, int keep_tail)
{
	char clipped[BUFSIZ];
	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		value, strlen(value), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	(void)morph_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped), safe, width,
							   keep_tail);
	int styled = cli_output_color_enabled(ctx);
	return (nb_buf_printf(text, "%s%s%s", styled ? color : "", clipped,
			      styled ? ANSI_RESET : "")
			? 0
			: NB_ERR_NOMEM);
}

static int cli_composer_footer_permissions(struct cli_application *ctx, nb_buf *usage, int columns)
{
	const char *name = ctx->input_status.permissions_ready
				   ? runtime_permission_preset_name(&ctx->input_status.permissions)
				   : "?";
	int rc = (nb_buf_printf(usage, "perm: %s", name) ? 0 : NB_ERR_NOMEM);
	if (!rc && !strcmp(name, "custom") && columns >= 60) {
		const struct permission_policy *current = &ctx->input_status.permissions;
		rc = (nb_buf_printf(usage, " %s/%s/%s", permission_access_name(current->access),
				    permission_approval_name(current->approval),
				    permission_reviewer_name(current->reviewer))
			      ? 0
			      : NB_ERR_NOMEM);
	}

	return rc ? rc : (nb_buf_append_cstr(usage, " · ") ? 0 : NB_ERR_NOMEM);
}

static int cli_composer_footer_tokens(nb_buf *text, int tokens)
{
	if (tokens >= 1000000)
		return (nb_buf_printf(text, "%.3gM", (double)tokens / 1000000.0) ? 0
										 : NB_ERR_NOMEM);

	if (tokens >= 1000)
		return (nb_buf_printf(text, "%.3gk", (double)tokens / 1000.0) ? 0 : NB_ERR_NOMEM);

	return (nb_buf_printf(text, "%d", tokens) ? 0 : NB_ERR_NOMEM);
}

static int cli_composer_footer_usage(struct cli_application *ctx, int columns, nb_buf *usage)
{
	int rc = cli_composer_footer_permissions(ctx, usage, columns);
	int total = runtime_mcp_count(ctx->runtime);
	int ready = 0, pending = 0, failed = 0;
	for (int i = 0; i < total; i++) {
		struct runtime_mcp_status status;
		if (runtime_mcp_info(ctx->runtime, i, &status))
			continue;

		ready += status.state == RUNTIME_MCP_READY;
		failed += status.state == RUNTIME_MCP_FAILED;
		pending += status.state == RUNTIME_MCP_CONNECTING ||
			   status.state == RUNTIME_MCP_DISCOVERING ||
			   status.state == RUNTIME_MCP_REGISTERING;
	}

	if (!rc && total > 0)
		rc = (nb_buf_printf(usage, "MCP %d/%d%s%s · ", ready, total, pending ? " ..." : "",
				    failed ? " !" : "")
			      ? 0
			      : NB_ERR_NOMEM);

	if (!rc)
		rc = (nb_buf_append_cstr(usage, columns >= 100 ? "context " : "ctx ")
			      ? 0
			      : NB_ERR_NOMEM);

	if (!rc)
		rc = cli_composer_footer_tokens(usage, ctx->input_status.tokens);

	if (!rc)
		rc = (nb_buf_append_cstr(usage, " / ") ? 0 : NB_ERR_NOMEM);

	if (!rc)
		rc = cli_composer_footer_tokens(usage, ctx->input_status.limit);

	if (!rc)
		rc = (nb_buf_printf(usage, columns >= 100 ? "  ·  %lld credits" : " · %lld cr",
				    (long long)ctx->input_status.credits)
			      ? 0
			      : NB_ERR_NOMEM);

	if (!rc && ctx->input_status.session_titles_pending > 0)
		rc = (nb_buf_append_cstr(usage, " · Naming session...") ? 0 : NB_ERR_NOMEM);

	if (!rc && ctx->recap.pending)
		rc = (nb_buf_append_cstr(usage, " · Generating recap...") ? 0 : NB_ERR_NOMEM);

	int other = cli_sessions_running_count(ctx) - cli_sessions_active(ctx);
	if (!rc && cli_sessions_open_count(ctx) > 1 && cli_sessions_current_id(ctx) > 0 &&
	    other > 0 && !nb_buf_printf(usage, " · %d other session%s running",
				      other, other == 1 ? "" : "s"))
		rc = nb_buf_status(usage);

	return rc;
}

static int cli_composer_footer_session(const struct session *session, size_t available,
				      size_t usage_width, nb_buf *text)
{
	if (session->id <= 0 || !session->display_id[0])
		return 0;

	char *name NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		session->name, strlen(session->name), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!name)
		return NB_ERR_NOMEM;

	size_t full_width = 7 + strlen(session->display_id) + nb_utf8_width_cstr(name);
	int show_name = *name && available >= usage_width &&
		       full_width <= available - usage_width;
	return nb_buf_printf(text, " · #%s%s%s", session->display_id,
			     show_name ? " . " : "", show_name ? name : "")
		? 0 : nb_buf_status(text);
}

static int cli_composer_footer_identity(struct cli_application *ctx, const struct session *session,
				       size_t remaining, nb_buf *text)
{
	size_t model_width = remaining / 3;
	if (model_width > 24)
		model_width = 24;

	int rc = cli_composer_footer_field(
		ctx, text,
		session->model[0] ? session->model
				 : runtime_config_borrow(ctx->runtime)->models.text.model,
		model_width, ANSI_DIM, 0);
	if (!rc)
		rc = (nb_buf_append_cstr(text, " · ") ? 0 : NB_ERR_NOMEM);

	const char *workdir = session->workdir[0] ? session->workdir : ctx->workdir;
	if (!workdir || !workdir[0])
		workdir = runtime_workdir_borrow(ctx->runtime);

	const char *home = getenv("HOME");
	if (home && home[0] && !strncmp(workdir, home, strlen(home)) &&
	    (workdir[strlen(home)] == '/' || workdir[strlen(home)] == '\0')) {
		if (!rc)
			rc = (nb_buf_append_cstr(text,
						 cli_output_color_enabled(ctx) ? ANSI_DIM "~" : "~")
				      ? 0
				      : NB_ERR_NOMEM);

		workdir += strlen(home);
		remaining--;
	}

	if (!rc)
		rc = cli_composer_footer_field(ctx, text, workdir, remaining - model_width,
					      ANSI_DIM, 1);

	return rc ? rc : (nb_buf_append_cstr(text, " · ") ? 0 : NB_ERR_NOMEM);
}

int cli_composer_footer_render(struct cli_application *ctx, int columns, nb_buf *out,
			      nb_buf *session_text)
{
	if (!ctx || !ctx->runtime || !out || !session_text)
		return NB_ERR_INVAL;

	if (columns < 20)
		return 0;

	if (ctx->input_status.limit <= 0)
		ctx->input_status.limit =
			runtime_config_borrow(ctx->runtime)->models.text.context_limit;

	nb_buf usage NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&usage), nb_buf_reserve(&usage, 64) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = cli_composer_footer_usage(ctx, columns, &usage);

	if (rc)
		return rc;

	struct session session = {0};
	rc = runtime_session_current(ctx->runtime, &session);
	if (rc)
		return rc;

	size_t usage_width = nb_utf8_width_cstr(nb_buf_cstr(&usage));
	size_t available = (size_t)columns - 4;
	int styled = cli_output_color_enabled(ctx);
	/* A permission chip at the start of the row adds one leading padding cell. */
	rc = cli_composer_footer_session(&session, available - (styled ? 1u : 0u),
					usage_width, session_text);
	if (rc)
		return rc;

	/* Reserve the session suffix and usage before optional identity fields. */
	available -= nb_utf8_width_cstr(nb_buf_cstr(session_text));
	size_t remaining = available;
	if (remaining >= usage_width + 8) {
		rc = cli_composer_footer_field(ctx, out, "morph", 5, ANSI_BOLD, 0);
		if (!rc)
			rc = (nb_buf_append_cstr(out, " · ") ? 0 : NB_ERR_NOMEM);
	}

	remaining = available - cli_terminal_text_width(nb_buf_cstr(out));
	if (!rc && remaining > usage_width + 10)
		rc = cli_composer_footer_identity(ctx, &session, remaining - usage_width - 6, out);

	remaining = available - cli_terminal_text_width(nb_buf_cstr(out));
	if (styled && nb_buf_empty(out) && remaining > 0)
		remaining--;

	if (!rc)
		rc = cli_composer_footer_field(
			ctx, out, nb_buf_cstr(&usage), remaining, ANSI_DIM, 0);

	(void)runtime_task_count_active(ctx->runtime, &ctx->input_status.tasks);
	if (!rc && (ctx->input_status.tasks.running || ctx->input_status.tasks.pending)) {
		nb_buf tasks NB_CLEANUP(nb_buf_cleanup) = {0};
		rc = (nb_buf_printf(&tasks, "Tasks %lld running · %lld pending",
				    (long long)ctx->input_status.tasks.running,
				    (long long)ctx->input_status.tasks.pending)
			      ? 0
			      : NB_ERR_NOMEM);
		if (!rc)
			rc = (nb_buf_append_char(out, '\n') ? 0 : NB_ERR_NOMEM);

		if (!rc)
			rc = cli_composer_footer_field(
				ctx, out, nb_buf_cstr(&tasks), (size_t)columns - 4,
				ctx->input_status.tasks.running ? ANSI_YELLOW : ANSI_DIM, 0);
	}

	return rc;
}
