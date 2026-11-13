#ifndef MORPH_CLI_WIDGETS_NOTICE_H
#define MORPH_CLI_WIDGETS_NOTICE_H

#include "util/buf.h"
#include <morph/tui/view.h>

enum cli_notice_status {
	CLI_NOTICE_PENDING,
	CLI_NOTICE_RUNNING,
	CLI_NOTICE_DONE,
	CLI_NOTICE_FAILED,
	CLI_NOTICE_QUESTION,
	CLI_NOTICE_CANCELLED,
	CLI_NOTICE_WARNING,
	CLI_NOTICE_SYSTEM,
};

struct cli_notice {
	enum cli_notice_status status;
	const char *title;
	const char *detail;
	const char *footer;
	/* Zero is a root row; one/two are middle/last children of the preceding root. */
	int tree_branch;
};

const char *cli_notice_marker(enum cli_notice_status status);
const char *cli_notice_color(enum cli_notice_status status);
int cli_notice_append(morph_buf_t *out, const struct cli_notice *notice, int columns, int styled);
struct morph_tui_view *cli_notice_add_borrow(struct morph_tui_builder *,
	struct morph_tui_view *parent, const struct cli_notice *, int styled);

#endif
