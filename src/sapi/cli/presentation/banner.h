#ifndef MORPH_CLI_PRESENTATION_BANNER_H
#define MORPH_CLI_PRESENTATION_BANNER_H

#include <nb/buf.h>
#include <nothing/tui/media.h>

#define CLI_BANNER_PADDING 2u

struct cli_application;

enum nt_media_protocol cli_banner_read_logo(struct cli_application *, nb_buf *bytes);
int cli_banner_render(struct cli_application *, nb_buf *rendered, int width, int has_logo);
void cli_banner_print(struct cli_application *ctx, const char *model, const char *directory);

#endif
