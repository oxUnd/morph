#include "cli/commands/registry.h"

static int cmd_image(struct cli_context *ctx, int argc, char **argv)
{
	const char *path = cli_cmd_arg(argc, argv, 1);
	if (!path) {
		CMD_ERROR("usage: /image <file_path>");
		return -EINVAL;
	}
	char *expanded = file_expand_path(path);
	if (!file_exists(expanded)) {
		CMD_ERROR("file not found: %s", expanded);
		free(expanded);
		return -ENOENT;
	}
	int w = 0, h = 0, ch = 0;
	if (!stbi_info(expanded, &w, &h, &ch)) {
		CMD_ERROR("not a valid image file: %s", expanded);
		free(expanded);
		MORPH_RETURN(MORPH_ERR_FORMAT);
	}
	strncpy(ctx->image_path, expanded, sizeof(ctx->image_path) - 1);
	cli_record_media_credits(ctx, "image_input",
				 credit_image_units_from_size(w, h), 0,
				 ctx->config.models.image.provider,
				 ctx->config.models.image.model, NULL);
	image_render_terminal(expanded);
	CMD_OK("image loaded: %s (%dx%d, %d channels)", expanded, w, h, ch);
	free(expanded);
	return 0;
}

static int cmd_video(struct cli_context *ctx, int argc, char **argv)
{
	if (argc < 2) {
		CMD_ERROR("usage: /video <file_path>");
		return -EINVAL;
	}
	if (!file_exists(argv[1])) {
		CMD_ERROR("file not found: %s", argv[1]);
		return -ENOENT;
	}
	if (video_play(argv[1], ctx->config.render.mpv_args) != 0) {
		CMD_ERROR("failed to play video: %s", argv[1]);
		return -EIO;
	}
	cli_record_media_credits(ctx, "video_input", 0, 1,
				 ctx->config.models.video.provider,
				 ctx->config.models.video.model,
				 "{\"estimated\":true}");
	CMD_OK("video loaded: %s", argv[1]);
	return 0;
}
static int cmd_render(struct cli_context *ctx, int argc, char **argv)
{
	const char *path = cli_cmd_arg(argc, argv, 1);
	if (!path) {
		CMD_ERROR("usage: /render <file_path>");
		return -EINVAL;
	}
	char *expanded = file_expand_path(path);
	if (!file_exists(expanded)) {
		CMD_ERROR("file not found: %s", expanded);
		free(expanded);
		return -ENOENT;
	}
	const char *ext = strrchr(expanded, '.');
	if (ext) ext++;
	if (ext && (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
		    strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
		    strcasecmp(ext, "webm") == 0 || strcasecmp(ext, "flv") == 0)) {
		if (video_play(expanded, ctx->config.render.mpv_args) != 0) {
			CMD_ERROR("failed to play video: %s", expanded);
			free(expanded);
			return -EIO;
		}
		CMD_OK("video: %s", expanded);
	} else if (ext && (strcasecmp(ext, "png") == 0 || strcasecmp(ext, "jpg") == 0 ||
			   strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "gif") == 0 ||
			   strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0 ||
			   strcasecmp(ext, "tga") == 0 || strcasecmp(ext, "hdr") == 0)) {
		int w = 0, h = 0, ch = 0;
		if (!stbi_info(expanded, &w, &h, &ch)) {
			CMD_ERROR("not a valid image file: %s", expanded);
			free(expanded);
			MORPH_RETURN(MORPH_ERR_FORMAT);
		}
		image_render_terminal(expanded);
		CMD_OK("image: %s (%dx%d)", expanded, w, h);
	} else {
		size_t len = 0;
		char *text = file_read_all(expanded, &len);
		if (!text) {
			CMD_ERROR("failed to read file: %s", expanded);
			free(expanded);
			return -EIO;
		}
		cli_markdown_render_ansi(text);
		free(text);
	}
	free(expanded);
	return 0;
}


static const struct cli_command media_commands[] = {
	{ "/image",   cmd_image,   "Inject an image into context",      "/image <file_path>" },
	{ "/img",     cmd_image,   "Alias for /image",                  "/img <file_path>" },
	{ "/video",   cmd_video,   "Inject a video (M3)",               "/video <file_path>" },
	{ "/vid",     cmd_video,   "Alias for /video",                  "/vid <file_path>" },
	{ "/render",  cmd_render,  "Render a file (image/video/markdown)", "/render <file_path>" },
	{ "/r",       cmd_render,  "Alias for /render",                 "/r <file_path>" },
};

int cli_register_media_commands(void)
{
	return cli_command_register_many(media_commands,
		(int)(sizeof(media_commands) / sizeof(media_commands[0])));
}
