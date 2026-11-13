#include "sapi/cli/presentation/composer_footer.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "util/cleanup.h"
#include "util/utf8.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cli_composer_footer_field(struct cli_application *ctx, morph_buf_t *text,
				    const char *value, size_t width, const char *color,
				    int keep_tail)
{
	char clipped[BUFSIZ];
	char *safe MORPH_CLEANUP(morph_charp_cleanup) = utf8_terminal_sanitize_dup(
		value, strlen(value), UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return -ENOMEM;
	(void)utf8_copy_ellipsized_display_width(clipped, sizeof(clipped), safe, width, keep_tail);
	int styled = cli_output_color_enabled(ctx);
	return morph_buf_printf(text, "%s%s%s", styled ? color : "", clipped,
				styled ? ANSI_RESET : "");
}

static int cli_composer_footer_permissions(struct cli_application *ctx, morph_buf_t *usage,
					  int columns)
{
	struct permission_policy policy;
	if (!runtime_permission_get_policy(ctx->runtime, &policy)) {
		ctx->input_status.permissions = policy;
		ctx->input_status.permissions_ready = 1;
	}
	const char *name = ctx->input_status.permissions_ready
				   ? runtime_permission_preset_name(&ctx->input_status.permissions)
				   : "?";
	int rc = morph_buf_printf(usage, "perm: %s", name);
	if (!rc && !strcmp(name, "custom") && columns >= 60) {
		const struct permission_policy *current = &ctx->input_status.permissions;
		rc = morph_buf_printf(usage, " %s/%s/%s", permission_access_name(current->access),
				      permission_approval_name(current->approval),
				      permission_reviewer_name(current->reviewer));
	}
	return rc ? rc : morph_buf_puts(usage, " · ");
}

static int cli_composer_footer_tokens(morph_buf_t *text, int tokens)
{
	if (tokens >= 1000000)
		return morph_buf_printf(text, "%.3gM", (double)tokens / 1000000.0);
	if (tokens >= 1000)
		return morph_buf_printf(text, "%.3gk", (double)tokens / 1000.0);
	return morph_buf_printf(text, "%d", tokens);
}

static int cli_composer_footer_usage(struct cli_application *ctx, int columns,
				    morph_buf_t *usage)
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
		rc = morph_buf_printf(usage, "MCP %d/%d%s%s · ", ready, total,
				      pending ? " ..." : "", failed ? " !" : "");
	if (!rc)
		rc = morph_buf_puts(usage, columns >= 100 ? "context " : "ctx ");
	if (!rc)
		rc = cli_composer_footer_tokens(usage, ctx->input_status.tokens);
	if (!rc)
		rc = morph_buf_puts(usage, " / ");
	if (!rc)
		rc = cli_composer_footer_tokens(usage, ctx->input_status.limit);
	if (!rc)
		rc = morph_buf_printf(usage, columns >= 100 ? "  ·  %lld credits" : " · %lld cr",
				      (long long)ctx->input_status.credits);
	if (!rc && ctx->input_status.session_titles_pending > 0)
		rc = morph_buf_puts(usage, " · Naming session...");
	if (!rc && ctx->recap.pending)
		rc = morph_buf_puts(usage, " · Generating recap...");
	return rc;
}

static int cli_composer_footer_identity(struct cli_application *ctx, size_t remaining,
				       morph_buf_t *text)
{
	struct session session = {0};
	(void)runtime_session_current(ctx->runtime, &session);
	size_t model_width = remaining / 3;
	if (model_width > 24)
		model_width = 24;
	int rc = cli_composer_footer_field(
		ctx, text,
		session.model[0] ? session.model
				 : runtime_config_borrow(ctx->runtime)->models.text.model,
		model_width, ANSI_DIM, 0);
	if (!rc)
		rc = morph_buf_puts(text, " · ");
	const char *workdir = runtime_workdir_borrow(ctx->runtime);
	if (!workdir || !workdir[0])
		workdir = ctx->workdir;
	const char *home = getenv("HOME");
	if (home && home[0] && !strncmp(workdir, home, strlen(home)) &&
	    (workdir[strlen(home)] == '/' || workdir[strlen(home)] == '\0')) {
		if (!rc)
			rc = morph_buf_puts(text,
					    cli_output_color_enabled(ctx) ? ANSI_DIM "~" : "~");
		workdir += strlen(home);
		remaining--;
	}
	if (!rc)
		rc = cli_composer_footer_field(ctx, text, workdir, remaining - model_width,
					      ANSI_DIM, 1);
	return rc ? rc : morph_buf_puts(text, " · ");
}

int cli_composer_footer_render(struct cli_application *ctx, int columns, morph_buf_t *out)
{
	if (!ctx || !ctx->runtime || !out)
		return -EINVAL;
	if (columns < 20)
		return 0;
	if (ctx->input_status.limit <= 0)
		ctx->input_status.limit =
			runtime_config_borrow(ctx->runtime)->models.text.context_limit;
	morph_buf_t usage MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&usage, 64);
	if (!rc)
		rc = cli_composer_footer_usage(ctx, columns, &usage);
	if (rc)
		return rc;
	/* The original footer reserves usage before adding optional identity fields. */
	size_t usage_width = utf8_display_width(morph_buf_cstr_borrow(&usage));
	size_t remaining = (size_t)columns - 4;
	if (remaining >= usage_width + 8) {
		rc = cli_composer_footer_field(ctx, out, "morph", 5, ANSI_BOLD, 0);
		if (!rc)
			rc = morph_buf_puts(out, " · ");
	}
	remaining = (size_t)columns - 4 - utf8_display_width_ansi(morph_buf_cstr_borrow(out));
	if (!rc && remaining > usage_width + 10)
		rc = cli_composer_footer_identity(ctx, remaining - usage_width - 6, out);
	if (!rc)
		rc = cli_composer_footer_field(
			ctx, out, morph_buf_cstr_borrow(&usage),
			(size_t)columns - 4 - utf8_display_width_ansi(morph_buf_cstr_borrow(out)),
			ANSI_DIM, 0);
	(void)runtime_task_count_active(ctx->runtime, &ctx->input_status.tasks);
	if (!rc && (ctx->input_status.tasks.running || ctx->input_status.tasks.pending)) {
		morph_buf_t tasks MORPH_CLEANUP(morph_buf_cleanup) = {0};
		rc = morph_buf_printf(&tasks, "Tasks %lld running · %lld pending",
				      (long long)ctx->input_status.tasks.running,
				      (long long)ctx->input_status.tasks.pending);
		if (!rc)
			rc = morph_buf_putc(out, '\n');
		if (!rc)
			rc = cli_composer_footer_field(ctx, out, morph_buf_cstr_borrow(&tasks),
						      (size_t)columns - 4,
						      ctx->input_status.tasks.running ? ANSI_YELLOW
										      : ANSI_DIM,
						      0);
	}
	return rc;
}
