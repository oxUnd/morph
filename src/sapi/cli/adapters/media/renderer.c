#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "util/cleanup.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "render/image.h"
#include "render/video.h"
#include "sapi/cli/application_internal.h"

void cli_renderer_media(const char *type, const char *path, void *user)
{
	struct cli_application *ctx = (struct cli_application *)user;
	if (cli_chat_active(ctx)) {
		morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
		if (!morph_buf_printf(&text, "%s: %s", type ? type : "media", path ? path : ""))
			(void)cli_chat_output(ctx, morph_buf_cstr_borrow(&text));
		return;
	}

	/*
	 * Markdown invokes this adapter at the media node's document position.
	 * Artifact/observation events only announce paths and never call it.
	 */
	if (ctx && ctx->presenter && path && path[0]) {
		if (morph_strmap_contains(&ctx->presenter->rendered_artifacts, path))
			return;
		(void)morph_strmap_set_borrow(&ctx->presenter->rendered_artifacts, path, (void *)1);
	}
	if (strcmp(type, "image") == 0) {
		image_render_terminal(ctx ? ctx->logger : NULL, path);
	} else if (strcmp(type, "video") == 0) {
		(void)video_render_terminal_preview(ctx ? ctx->logger : NULL, path);
	}
}
