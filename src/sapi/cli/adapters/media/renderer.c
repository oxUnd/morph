#include "sapi/cli/adapters/media/renderer.h"
#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "util/cleanup.h"
#include "sapi/cli/presentation/presenter_state.h"
#include "render/image.h"
#include "render/video.h"
#include "sapi/cli/application_internal.h"
#include "util/image_util.h"
#include "util/error.h"
#include "stb_image.h"
#include <morph/tui/media.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static void cli_renderer_pixels_cleanup(unsigned char **owner)
{
	stbi_image_free(*owner);
	*owner = NULL;
}

static void cli_renderer_preview_cleanup(char **owner)
{
	if (*owner)
		unlink(*owner);
	morph_charp_cleanup(owner);
}

int cli_renderer_media_asset_create_copy(const char *type, const char *path,
	struct morph_tui_media_asset **out)
{
	if (!type || !path || !*path || !out || *out)
		return -EINVAL;
	char *preview MORPH_CLEANUP(cli_renderer_preview_cleanup) = NULL;
	if (!strcmp(type, "video")) {
		int rc = video_preview_create_copy(path, &preview);
		if (rc)
			return rc;
		path = preview;
	} else if (strcmp(type, "image"))
		return -ENOTSUP;
	int width = 0, height = 0, channels = 0;
	unsigned char *pixels MORPH_CLEANUP(cli_renderer_pixels_cleanup) =
		image_load_bounded_copy(path, &width, &height, &channels, 4);
	if (!pixels)
		return MORPH_ERR_FORMAT;
	return morph_tui_media_asset_create_rgba_copy(out, pixels, width, height);
}

void cli_renderer_media(const char *type, const char *path, void *user)
{
	struct cli_application *ctx = (struct cli_application *)user;
	if (cli_chat_active(ctx)) {
		(void)cli_chat_media(ctx, type, path);
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
