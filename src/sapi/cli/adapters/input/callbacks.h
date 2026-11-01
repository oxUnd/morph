#ifndef MORPH_CLI_ADAPTERS_INPUT_CALLBACKS_H
#define MORPH_CLI_ADAPTERS_INPUT_CALLBACKS_H

#include <stdio.h>
#include <stdint.h>

struct cli_application;
struct cli_callback;

/* Instance-owned closures adapt APIs that have no userdata parameter. */
int (*cli_callback_key(struct cli_application *ctx,
		       int (*callback)(struct cli_application *, int, int)))(int, int);
void (*cli_callback_display(struct cli_application *ctx,
			    void (*callback)(struct cli_application *)))(void);
void (*cli_callback_line(struct cli_application *ctx,
			 void (*callback)(struct cli_application *, char *)))(char *);
int (*cli_callback_getc(struct cli_application *ctx,
			int (*callback)(struct cli_application *, FILE *)))(FILE *);
char **(*cli_callback_complete(struct cli_application *ctx,
			       char **(*callback)(struct cli_application *, const char *, int,
						  int)))(const char *, int, int);
char *(*cli_callback_generate(struct cli_application *ctx,
			      char *(*callback)(struct cli_application *, const char *,
						int)))(const char *, int);
void cli_callbacks_cleanup(struct cli_application *ctx);

#endif
