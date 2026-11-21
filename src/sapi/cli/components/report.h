#ifndef MORPH_CLI_COMPONENTS_REPORT_H
#define MORPH_CLI_COMPONENTS_REPORT_H

#include <nothing/tui/view.h>
#include <nb/buf.h>

/* An inline report reflows its domain snapshot above an optional native footer. */
struct cli_report {
	struct nt_view base;
	struct nt_view *text, *footer;
	nb_buf document;
	struct nt_view_size size;
	/* A negative footer height uses the native footer's measurement. */
	struct nt_view_size footer_size;
	int measured_width, published_width, styled;
	/* Formatting caches text and footer geometry without changing native views.
	 * Publish applies those values only at the arranged content width. */
	int (*format)(struct cli_report *, nb_buf *, int width);
	int (*publish)(struct cli_report *);
};

extern const struct nt_view_class cli_report_class;

/* Read-only report text fills each terminal row before wrapping. */
struct nt_view *cli_report_text_add(struct nt_builder *, struct nt_view *parent,
				    const struct nt_view_options *);
struct cli_report *cli_report_add(struct nt_builder *, struct nt_view *,
				  const struct nt_view_class *, const struct nt_view_options *,
				  int styled,
				  int (*format)(struct cli_report *, nb_buf *, int width));
int cli_report_measure_document(const nb_buf *, int width, struct nt_view_size *);
void cli_report_invalidate(struct cli_report *, int styled);

#endif
