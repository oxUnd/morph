#ifndef MORPH_TUI_TERMINAL_SIZE_H
#define MORPH_TUI_TERMINAL_SIZE_H

#ifdef __cplusplus
extern "C" {
#endif

/* A terminal descriptor takes precedence over a valid COLUMNS environment value. */
int morph_terminal_columns(int output_fd, int fallback);
void morph_terminal_dimensions(int output_fd, int *columns, int *rows);

#ifdef __cplusplus
}
#endif

#endif
