#ifndef MORPH_CLI_WIDGETS_BANNER_H
#define MORPH_CLI_WIDGETS_BANNER_H

struct cli_application;

void cli_banner_print(struct cli_application *ctx, const char *model, const char *directory);

#endif
