#ifndef MORPH_CLI_PRESENTATION_TERMINAL_TEXT_H
#define MORPH_CLI_PRESENTATION_TERMINAL_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t cli_terminal_text_width(const char *text);
const char *cli_terminal_text_advance(const char *text, size_t columns);
char *cli_terminal_text_strip_ansi_dup(const char *text, size_t length, size_t *out_length);

#ifdef __cplusplus
}
#endif

#endif
