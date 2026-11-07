#ifndef MORPH_CLI_ADAPTERS_MEDIA_IMAGE_LOADER_H
#define MORPH_CLI_ADAPTERS_MEDIA_IMAGE_LOADER_H

struct cli_application;

/* path must be NULL; success returns a malloc-owned temporary file path.
 * Failure preserves path. The caller owns removal of that file. */
int cli_image_loader_load(const char *url, char **path, void *user);
void cli_image_loader_release(char *path, void *user);

#endif
