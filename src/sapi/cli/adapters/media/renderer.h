#ifndef MORPH_CLI_ADAPTERS_MEDIA_RENDERER_H
#define MORPH_CLI_ADAPTERS_MEDIA_RENDERER_H

struct cli_application;
struct morph_tui_media_asset;

void cli_renderer_media(const char *type, const char *path, void *user);
int cli_renderer_media_asset_create_copy(const char *type, const char *path,
	struct morph_tui_media_asset **out);

#endif
