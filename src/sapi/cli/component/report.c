#include "sapi/cli/component/report.h"
#include "sapi/cli/presentation/messages.h"
#include <nb/cleanup.h>
#include <nothing/tui/views/text_view.h>
#include <nothing/tui/terminal/text.h>
#include <limits.h>
#include <string.h>

int cli_report_measure_document(const nb_buf *document, int width, struct nt_view_size *size)
{
	char *plain NB_CLEANUP(nb_charp_cleanup) =
		nt_terminal_strip_ansi_dup(nb_buf_cstr(document), document->len, NULL);
	if (!plain)
		return NB_ERR_NOMEM;
	char *start = plain + strspn(plain, "\r\n");
	size_t len = strlen(start);
	while (len && (start[len - 1] == '\n' || start[len - 1] == '\r'))
		start[--len] = '\0';
	struct nt_text_layout lines NB_CLEANUP(nt_text_layout_deinit) = {0};
	nt_text_layout_init(&lines);
	int rc = nt_text_layout_build(&lines, start, width > 0 ? (size_t)width : 1,
				      NT_TEXT_WRAP_CHAR);
	if (rc)
		return rc;
	int columns = 0;
	for (size_t i = 0; i < lines.lines.len; i++) {
		const struct nt_text_line *line = nb_vec_at(&lines.lines, i);
		if (line->width > columns)
			columns = line->width;
	}
	if (lines.lines.len > INT_MAX)
		return NB_ERR_RANGE;
	*size = (struct nt_view_size){columns, (int)lines.lines.len};
	return 0;
}

static int cli_report_reflow(struct cli_report *report, int width)
{
	if (report->measured_width == width)
		return 0;
	nb_buf document NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = report->format(report, &document, width);
	if (rc)
		return rc;
	rc = cli_report_measure_document(&document, width, &report->size);
	if (rc)
		return rc;
	report->measured_width = width;
	nb_buf_move(&report->document, &document);
	return 0;
}

static int cli_report_measure(struct nt_view *view, int width, struct nt_view_size *size)
{
	struct cli_report *report = (struct cli_report *)view;
	int rc = cli_report_reflow(report, width);
	if (!rc)
		*size = report->size;
	return rc;
}

static int cli_report_layout(struct nt_view *view)
{
	struct cli_report *report = (struct cli_report *)view;
	struct nt_view_rect rect = nt_view_content_rect(view);
	if (rect.width != report->published_width) {
		int rc = cli_report_reflow(report, rect.width);
		if (!rc)
			rc = cli_messages_set_rendered_view(
				report->text, nb_buf_cstr(&report->document), report->styled);
		if (rc)
			return rc;
		report->published_width = rect.width;
	}
	return nt_view_arrange(report->text, rect);
}

static void cli_report_destroy(struct nt_view *view)
{
	nb_buf_cleanup(&((struct cli_report *)view)->document);
}

const struct nt_view_class cli_report_class = {
	.superclass = &nt_view_base_class,
	.instance_size = sizeof(struct cli_report),
	.kind = "cli_report",
	.measure = cli_report_measure,
	.layout = cli_report_layout,
	.deinit = cli_report_destroy,
};

struct nt_view *cli_report_text_add(struct nt_builder *builder, struct nt_view *parent,
				    const struct nt_view_options *options)
{
	struct nt_view *text = nt_text_view_add(builder, parent, "", options);
	if (text) {
		nt_builder_fail(builder, nt_text_view_set_wrap_mode(text, NT_TEXT_WRAP_CHAR));
		nt_builder_fail(builder, nt_view_set_focusable(text, 0));
	}
	return text;
}

struct cli_report *cli_report_add(struct nt_builder *builder, struct nt_view *parent,
				  const struct nt_view_class *type,
				  const struct nt_view_options *options, int styled,
				  int (*format)(struct cli_report *, nb_buf *, int))
{
	struct cli_report *report =
		(struct cli_report *)nt_view_add(builder, parent, type, options);
	if (!report)
		return NULL;
	report->format = format;
	report->measured_width = report->published_width = -1;
	report->styled = styled;
	report->text = cli_report_text_add(builder, &report->base, NULL);
	if (!report->text)
		return NULL;
	return report;
}

void cli_report_invalidate(struct cli_report *report, int styled)
{
	report->measured_width = report->published_width = -1;
	report->styled = styled;
	nt_view_invalidate(&report->base, 1);
}
