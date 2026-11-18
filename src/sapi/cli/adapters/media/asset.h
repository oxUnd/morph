#ifndef MORPH_CLI_ADAPTERS_MEDIA_ASSET_H
#define MORPH_CLI_ADAPTERS_MEDIA_ASSET_H

#include <nothing/tui/media.h>

int cli_media_asset_from_png(struct nt_media_asset **out, const unsigned char *png, size_t length);
int cli_media_native_available(void);

#endif
