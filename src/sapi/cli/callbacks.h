#ifndef MORPH_CLI_CALLBACKS_H
#define MORPH_CLI_CALLBACKS_H

#include <stdio.h>
#ifdef HAVE_READLINE
#include <readline/keymaps.h>
Keymap cli_keymap_copy(Keymap source);
void cli_keymap_free(Keymap map);
#endif

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
char *cli_input_fgets(struct cli_context *ctx, char *buffer, size_t capacity, FILE *stream);
int cli_poll_getc(struct cli_context *ctx, FILE *stream);
char *cli_readline_prompt(struct cli_context *ctx, const char *prompt);
void cli_callbacks_cleanup(struct cli_context *ctx);

#endif
