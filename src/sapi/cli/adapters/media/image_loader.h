#ifndef MORPH_CLI_ADAPTERS_MEDIA_IMAGE_LOADER_H
#define MORPH_CLI_ADAPTERS_MEDIA_IMAGE_LOADER_H

struct cli_application;

int cli_image_loader_load(const char *url, char **path, void *user);
void cli_image_loader_release(char *path, void *user);

#endif
