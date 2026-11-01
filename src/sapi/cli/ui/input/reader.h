#ifndef MORPH_CLI_UI_INPUT_READER_H
#define MORPH_CLI_UI_INPUT_READER_H

#include <stdio.h>
#include <stdint.h>

struct cli_context;

char *cli_input_fgets(struct cli_context *ctx, char *buffer, size_t capacity, FILE *stream);
char *cli_input_fgets_until(struct cli_context *ctx, char *buffer, size_t capacity, FILE *stream,
			    int64_t *deadline);
#ifdef HAVE_READLINE
int cli_poll_getc(struct cli_context *ctx, FILE *stream);
char *cli_readline_prompt(struct cli_context *ctx, const char *prompt);
char *cli_reader_prompt_stream(struct cli_context *ctx, const char *prompt, FILE *stream);
void cli_reader_remember(const char *text);
#endif

#endif
