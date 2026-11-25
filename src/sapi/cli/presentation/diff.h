#ifndef MORPH_CLI_PRESENTATION_DIFF_H
#define MORPH_CLI_PRESENTATION_DIFF_H

#include <nb/vec.h>

struct nt_view;
/* Render recorded unified patches without reading today's workspace. Hunk
 * entries are document byte offsets, suitable for native cursor navigation. */
int cli_diff_present(struct nt_view *document, const char *patch, nb_vec *hunks, int styled);

#endif
