#ifndef MORPH_CLI_COMPONENTS_DISCLOSURE_H
#define MORPH_CLI_COMPONENTS_DISCLOSURE_H

#include <nothing/tui/view.h>

/* Copies title/body; starts collapsed and uses native button activation. */
struct nt_view *cli_disclosure_add(struct nt_builder *, struct nt_view *parent,
	const char *title, const char *body, int styled);

/* The preview stays visible while the full body is collapsed. */
struct nt_view *cli_disclosure_add_preview(struct nt_builder *, struct nt_view *parent,
	const char *title, const char *preview, const char *body, int styled);

#endif
