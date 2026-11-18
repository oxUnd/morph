#ifndef MORPH_TERMINAL_REPORT_H
#define MORPH_TERMINAL_REPORT_H

#include <nb/buf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared geometry for incremental reports that cannot retain a full model. */
/* Marker and styles are trusted terminal formatting; text is sanitized. */
int morph_terminal_tree_row(nb_buf *output, const char *ancestor, int is_last, const char *marker,
			    const char *label, const char *description, int label_width,
			    int columns, const char *style, const char *reset);
const char *morph_terminal_tree_branch(int is_last, int continuation);
int morph_terminal_tree_prefix(nb_buf *output, const int *ancestors_last, size_t depth, int is_last,
			       int continuation, size_t indent);

#ifdef __cplusplus
}
#endif
#endif
