#ifndef MORPH_CLI_ADAPTERS_INPUT_READER_H
#define MORPH_CLI_ADAPTERS_INPUT_READER_H

#include <stdio.h>
#include <stdint.h>

struct cli_application;

char *cli_input_fgets(struct cli_application *ctx, char *buffer, size_t capacity, FILE *stream);
char *cli_input_fgets_until(struct cli_application *ctx, char *buffer, size_t capacity,
			    FILE *stream, int64_t *deadline);
#ifdef HAVE_READLINE
int cli_poll_getc(struct cli_application *ctx, FILE *stream);
char *cli_readline_prompt(struct cli_application *ctx, const char *prompt);
char *cli_reader_prompt_stream(struct cli_application *ctx, const char *prompt, FILE *stream);
void cli_reader_remember(const char *text);
#endif

#endif
