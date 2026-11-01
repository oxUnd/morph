#ifndef MORPH_CLI_UI_INPUT_CALLBACKS_H
#define MORPH_CLI_UI_INPUT_CALLBACKS_H

#include <stdio.h>
#include <stdint.h>

struct cli_context;
struct cli_callback;

/* Instance-owned closures adapt APIs that have no userdata parameter. */
int (*cli_callback_key(struct cli_context *ctx,
		       int (*callback)(struct cli_context *, int, int)))(int, int);
void (*cli_callback_display(struct cli_context *ctx, void (*callback)(struct cli_context *)))(void);
void (*cli_callback_line(struct cli_context *ctx,
			 void (*callback)(struct cli_context *, char *)))(char *);
int (*cli_callback_getc(struct cli_context *ctx,
			int (*callback)(struct cli_context *, FILE *)))(FILE *);
char **(*cli_callback_complete(struct cli_context *ctx,
			       char **(*callback)(struct cli_context *, const char *, int,
						  int)))(const char *, int, int);
char *(*cli_callback_generate(struct cli_context *ctx,
			      char *(*callback)(struct cli_context *, const char *,
						int)))(const char *, int);
void cli_callbacks_cleanup(struct cli_context *ctx);

#endif
