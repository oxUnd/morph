#ifndef MORPH_CLI_COMPONENTS_REPORT_H
#define MORPH_CLI_COMPONENTS_REPORT_H

#include <nothing/tui/view.h>
#include <nb/buf.h>

/* An inline report reflows its domain snapshot into one styled NT text view. */
struct cli_report {
	struct nt_view base;
	struct nt_view *text;
	nb_buf document;
	struct nt_view_size size;
	int measured_width, published_width, styled;
	int (*format)(struct cli_report *, nb_buf *, int width);
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
