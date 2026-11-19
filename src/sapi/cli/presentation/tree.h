#ifndef MORPH_CLI_PRESENTATION_TREE_H
#define MORPH_CLI_PRESENTATION_TREE_H

#include <nb/buf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Marker and styles are trusted terminal formatting. Labels and descriptions are sanitized. */
const char *cli_tree_branch(int is_last, int continuation);
int cli_tree_prefix(nb_buf *output, const int *ancestors_last, size_t depth, int is_last,
		    int continuation, size_t indent);
int cli_tree_row(nb_buf *output, const char *ancestor, int is_last, const char *marker,
		 const char *label, const char *description, int label_width, int columns,
		 const char *style, const char *reset);

#ifdef __cplusplus
}
#endif
#endif
