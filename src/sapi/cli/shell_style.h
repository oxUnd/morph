#ifndef MORPH_CLI_SHELL_STYLE_H
#define MORPH_CLI_SHELL_STYLE_H

#include "util/buf.h"

/* Append terminal-safe shell text. A positive width wraps with four-column
 * continuation indentation. This is a conservative lexer, not a shell parser. */
int cli_shell_style(morph_buf_t *out, const char *text, int width);

#endif
