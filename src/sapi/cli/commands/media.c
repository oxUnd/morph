#include "sapi/cli/commands/media.h"
#include <string.h>
#include <stdlib.h>
#include "nb/cleanup.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "sapi/cli/application.h"
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/services/turn.h"
#include "sapi/cli/application_internal.h"
#include "ext/ext.h"
#include "credits.h"
#include "config/config.h"
#include "render/image.h"
#include "render/video.h"
#include "stb_image.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/commands/registry.h"

int cli_media_attach_image(struct cli_application *ctx, const char *path)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_1 = NB_OK;

	char *expanded;
	const struct config *config;
	int w = 0;
	int h = 0;
	int ch = 0;

	if (!ctx || !path || !path[0])
		return NB_ERR_INVAL;
	expanded = ((expand_status_1 = nb_path_expand(&file_value_1, path, NULL)) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_1))
			    : NULL);
	if (!expanded)
		return expand_status_1 != NB_OK ? expand_status_1 : NB_ERR_NOMEM;
	if (!nb_file_exists(expanded)) {
		CMD_ERROR(ctx, "file not found: %s", expanded);
		free(expanded);
		return NB_ERR_NOT_FOUND;
	}
	if (!stbi_info(expanded, &w, &h, &ch)) {
		CMD_ERROR(ctx, "not a valid image file: %s", expanded);
		free(expanded);
		return NB_ERR_FORMAT;
	}
	strncpy(ctx->image_path, expanded, sizeof(ctx->image_path) - 1);
	ctx->image_path[sizeof(ctx->image_path) - 1] = '\0';
	config = runtime_config_borrow(ctx->runtime);
	cli_turn_record_media_credits(ctx, "image_input", credit_image_units_from_size(w, h), 0,
				      config->models.image.provider, config->models.image.model,
				      NULL);
	CMD_OK(ctx, "[IMAGE#1]");
	free(expanded);
	return 0;
}

static int cli_media_command_image(struct cli_application *ctx, int argc, char **argv)
{
	const char *path = cli_cmd_arg_borrow(argc, argv, 1);

	if (!path) {
		CMD_ERROR(ctx, "usage: /image <file_path>");
		return NB_ERR_INVAL;
	}
	return cli_media_attach_image(ctx, path);
}

static int cli_media_path_is_video(const char *path)
{
	const char *ext = strrchr(path, '.');

	if (!ext)
		return 0;
	ext++;
	return strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
	       strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
	       strcasecmp(ext, "webm") == 0 || strcasecmp(ext, "flv") == 0;
}

static int cli_media_play_video(struct cli_application *ctx, const char *path)
{
	const struct config *config = runtime_config_borrow(ctx->runtime);

	if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON &&
	    video_play(ctx ? ctx->logger : NULL, path, config->render.mpv_args) != 0) {
		CMD_ERROR(ctx, "failed to play video: %s", path);
		return NB_ERR_IO;
	}
	cli_turn_record_media_credits(ctx, "video_input", 0, 1, config->models.video.provider,
				      config->models.video.model, "{\"estimated\":true}");
	CMD_OK(ctx, "video loaded: %s", path);
	return 0;
}

int cli_media_handle_path(struct cli_application *ctx, const char *input, int *handled)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_2 = NB_OK;

	char *copy;
	char *expanded;
	char *argv[3];
	int argc;
	int width;
	int height;
	int channels;
	int rc = 0;

	if (!ctx || !input || !handled)
		return NB_ERR_INVAL;
	*handled = 0;
	copy = strdup(input);
	if (!copy)
		return NB_ERR_NOMEM;
	argc = cli_argv_split(copy, argv, 3);
	if (argc != 1) {
		free(copy);
		return 0;
	}
	expanded = ((expand_status_2 = nb_path_expand(&file_value_2, argv[0], NULL)) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_2))
			    : NULL);
	free(copy);
	if (!expanded)
		return expand_status_2 != NB_OK ? expand_status_2 : NB_ERR_NOMEM;
	if (!nb_file_exists(expanded)) {
		free(expanded);
		return 0;
	}
	if (stbi_info(expanded, &width, &height, &channels)) {
		*handled = 1;
		rc = cli_media_attach_image(ctx, expanded);
	} else if (cli_media_path_is_video(expanded)) {
		*handled = 1;
		rc = cli_media_play_video(ctx, expanded);
	}
	free(expanded);
	if (rc != 0)
		return rc;
	return 0;
}
static int cli_media_command_render(struct cli_application *ctx, int argc, char **argv)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_4 NB_CLEANUP(nb_charp_cleanup) = NULL;

	const char *path = cli_cmd_arg_borrow(argc, argv, 1);
	if (!path) {
		CMD_ERROR(ctx, "usage: /render <file_path>");
		return NB_ERR_INVAL;
	}
	char *expanded = (nb_path_expand(&file_value_3, path, NULL) == NB_OK
				  ? strdup(nb_buf_cstr(&file_value_3))
				  : NULL);
	if (!nb_file_exists(expanded)) {
		CMD_ERROR(ctx, "file not found: %s", expanded);
		free(expanded);
		return NB_ERR_NOT_FOUND;
	}
	const char *ext = strrchr(expanded, '.');
	if (ext)
		ext++;
	if (ext && (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
		    strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
		    strcasecmp(ext, "webm") == 0 || strcasecmp(ext, "flv") == 0)) {
		const struct config *config = runtime_config_borrow(ctx->runtime);
		if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON &&
		    video_play(ctx ? ctx->logger : NULL, expanded, config->render.mpv_args) != 0) {
			CMD_ERROR(ctx, "failed to play video: %s", expanded);
			free(expanded);
			return NB_ERR_IO;
		}
		CMD_OK(ctx, "video: %s", expanded);
	} else if (ext && (strcasecmp(ext, "png") == 0 || strcasecmp(ext, "jpg") == 0 ||
			   strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "gif") == 0 ||
			   strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0 ||
			   strcasecmp(ext, "tga") == 0 || strcasecmp(ext, "hdr") == 0)) {
		int w = 0, h = 0, ch = 0;
		if (!stbi_info(expanded, &w, &h, &ch)) {
			CMD_ERROR(ctx, "not a valid image file: %s", expanded);
			free(expanded);
			return NB_ERR_FORMAT;
		}
		if (cli_chat_active(ctx)) {
			int rc = cli_chat_media(ctx, "image", expanded);
			if (rc) {
				free(expanded);
				return rc;
			}
		} else if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON) {
			(void)cli_output_result_begin(ctx);
			image_render_terminal(ctx ? ctx->logger : NULL, expanded);
		}
		CMD_OK(ctx, "image: %s (%dx%d)", expanded, w, h);
	} else {
		size_t len = 0;
		char *text = (nb_file_read_all(expanded, &file_value_4, 0, NULL) == NB_OK &&
					      (file_copy_4 = (char *)malloc(
						       nb_buf_len(&file_value_4) + 1)) != NULL
				      ? (memcpy(file_copy_4, nb_buf_cstr(&file_value_4),
						nb_buf_len(&file_value_4) + 1),
					 *(&len) = nb_buf_len(&file_value_4),
					 nb_charp_steal(&file_copy_4))
				      : NULL);
		if (!text) {
			CMD_ERROR(ctx, "failed to read file: %s", expanded);
			free(expanded);
			return NB_ERR_IO;
		}
		if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
			cli_output_printf(ctx, "%s", text);
		else
			cli_markdown_render_ansi(ctx, text);
		free(text);
	}
	free(expanded);
	return 0;
}

static const struct cli_command cli_media_entries[] = {
	{"/image", cli_media_command_image, "Inject an image into context", "/image <file_path>"},
	{"/render", cli_media_command_render, "Render a file (image/video/markdown)",
	 "/render <file_path>"},
	{"/r", cli_media_command_render, "Alias for /render", "/r <file_path>"},
};

struct cli_command_group cli_media_commands(void)
{
	return (struct cli_command_group){
		.entries = cli_media_entries,
		.count = (int)(sizeof(cli_media_entries) / sizeof(cli_media_entries[0])),
		.category = "Media",
	};
}
