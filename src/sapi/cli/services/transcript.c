#include "sapi/cli/services/transcript.h"
#include "sapi/cli/application.h"
#include "sapi/cli/models/transcript.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/tool_preview.h"
#include "sapi/cli/widgets/feed.h"
#include "sapi/cli/widgets/transcript.h"
#include "util/error.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double cli_transcript_now(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int cli_transcript_prepare(struct cli_application *ctx)
{
	if (!ctx)
		return -EINVAL;
	if (ctx->transcript)
		return 0;
	return cli_transcript_model_create(&ctx->transcript);
}

void cli_transcript_reset(struct cli_application *ctx)
{
	if (!ctx)
		return;
	cli_transcript_model_destroy(ctx->transcript);
	ctx->transcript = NULL;
}

int cli_transcript_reasoning_append(struct cli_application *ctx, const char *text, size_t length,
				    int begin)
{
	int rc = cli_transcript_prepare(ctx);

	if (rc != 0)
		return rc;
	return cli_transcript_model_reasoning_append(ctx->transcript, text, length, begin);
}

static void cli_transcript_restore_live(struct cli_application *ctx)
{
	morph_buf_t live;

	if (morph_buf_init(&live, 128) != 0)
		return;
	if (cli_transcript_live_text(ctx->transcript, &live, cli_feed_columns(ctx), 0))
		cli_feed_live_set(ctx, morph_buf_cstr_borrow(&live));
	morph_buf_cleanup(&live);
}

static int cli_transcript_capture_preview(struct cli_application *ctx, size_t index)
{
	const struct cli_transcript_tool *tool = morph_array_borrow(&ctx->transcript->tools, index);
	const char *workdir;
	morph_buf_t preview;
	int rc;

	if (!tool || strcmp(tool->name, "apply_patch") != 0)
		return 0;
	workdir = ctx->workdir[0] ? ctx->workdir : runtime_workdir_borrow(ctx->runtime);
	if (morph_buf_init(&preview, 256) != 0)
		return 0;
	rc = cli_tool_patch_preview(tool, workdir, &preview);
	if (rc == 0)
		rc = cli_transcript_model_set_preview(ctx->transcript, index,
						      morph_buf_cstr_borrow(&preview));
	else
		rc = 0;
	morph_buf_cleanup(&preview);
	return rc;
}

int cli_transcript_restore_tool(struct cli_application *ctx, const struct cJSON *call,
				const struct model_history_item *result)
{
	size_t index;
	int rc = cli_transcript_prepare(ctx);

	if (rc != 0)
		return rc;
	rc = cli_transcript_model_restore(ctx->transcript, call, result, &index);
	if (rc != 0 || index == SIZE_MAX)
		return rc;
	const struct cli_transcript_tool *tool = morph_array_borrow(&ctx->transcript->tools, index);

	cli_transcript_render_tool(ctx, tool, !ctx->tool_details);
	if (ctx->tool_details)
		cli_transcript_render_tool_details(ctx, tool, 1);
	return 0;
}

int cli_transcript_event(struct cli_application *ctx, const struct morph_event *event)
{
	struct cli_transcript_change change;
	const struct cli_transcript_tool *tool;
	int rc;

	if (!ctx || !event)
		return -EINVAL;
	if (ctx->presentation_mode != CLI_PRESENT_INTERACTIVE)
		return 0;
	rc = cli_transcript_prepare(ctx);
	if (rc != 0)
		return rc;
	rc = cli_transcript_model_reduce(ctx->transcript, event, cli_transcript_now(), &change);
	if (rc != 0)
		return rc;
	if (change.kind == CLI_TRANSCRIPT_UNCHANGED)
		return 0;
	tool = morph_array_borrow(&ctx->transcript->tools, change.tool_index);
	switch (change.kind) {
	case CLI_TRANSCRIPT_OBSERVATION:
		if (change.observation_added && ctx->tool_details)
			cli_transcript_render_payload(ctx, tool->result);
		break;
	case CLI_TRANSCRIPT_TOOL_STARTED:
		rc = cli_transcript_capture_preview(ctx, change.tool_index);
		if (rc != 0)
			return rc;
		cli_feed_live_clear(ctx);
		cli_presentation_flush_stream(ctx);
		if (!(tool->process_poll && !ctx->tool_details) &&
		    (ctx->tool_details || !isatty(STDOUT_FILENO)))
			cli_transcript_render_tool(ctx, tool, !ctx->tool_details);
		if (ctx->tool_details)
			cli_transcript_render_tool_details(ctx, tool, 1);
		cli_transcript_restore_live(ctx);
		break;
	case CLI_TRANSCRIPT_TOOL_RUNNING:
		break;
	case CLI_TRANSCRIPT_TOOL_STREAMED:
		if (ctx->tool_details) {
			cli_feed_live_clear(ctx);
			cli_transcript_render_stream(ctx, tool, change.stream_offset);
		}
		cli_transcript_restore_live(ctx);
		break;
	case CLI_TRANSCRIPT_TOOL_FINISHED:
		cli_feed_live_clear(ctx);
		if (ctx->tool_details || !tool->process_running)
			cli_transcript_render_tool(ctx, tool, !ctx->tool_details);
		if (ctx->tool_details)
			cli_transcript_render_tool_details(ctx, tool, 0);
		cli_transcript_restore_live(ctx);
		break;
	case CLI_TRANSCRIPT_UNCHANGED:
		break;
	}
	return 1;
}

void cli_transcript_finish(struct cli_application *ctx)
{
	const struct cli_transcript_tool *tool;

	if (!ctx || !ctx->transcript || ctx->transcript->finished)
		return;
	cli_transcript_model_finish(ctx->transcript, cli_transcript_now());
	morph_array_foreach (tool, &ctx->transcript->tools, const struct cli_transcript_tool) {
		if (tool->interrupted)
			cli_transcript_render_tool(ctx, tool, !ctx->tool_details);
	}
}
