#include "sapi/cli/presentation/setup.h"

#include "terminal/text.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/draw.h>
#include <nothing/tui/terminal.h>
#include <nothing/tui/text.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cli_setup_print(FILE *output, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (!isatty(fileno(output))) {
		int written = vfprintf(output, format, args);
		va_end(args);
		return written < 0 ? NB_ERR_IO : 0;
	}

	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {0};
	struct nt_text_layout view;
	nt_text_layout_init(&view);
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_vprintf(&text, format, args) ? 0 : NB_ERR_NOMEM);

	va_end(args);
	char *safe = rc == 0 ? morph_terminal_sanitize_dup(nb_buf_cstr(&text), text.len,
							   MORPH_TERMINAL_TEXT_MULTILINE, NULL)
			     : NULL;
	if (rc == 0 && !safe)
		rc = NB_ERR_NOMEM;

	int columns, rows;
	nt_terminal_dimensions(fileno(output), &columns, &rows);
	size_t width = columns > 4 ? (size_t)columns - 4 : 1;
	if (rc == 0)
		rc = nt_text_layout_build(&view, safe, width, NT_TEXT_WRAP_CHAR);

	if (rc == 0)
		rc = (nb_buf_init(&rendered), nb_buf_reserve(&rendered, 4096) ? 0 : NB_ERR_NOMEM);

	for (size_t i = 0; rc == 0 && i < view.lines.len; i++) {
		const struct nt_text_line *row = nb_vec_at(&view.lines, i);
		if (row->length)
			rc = (nb_buf_append_cstr(&rendered, "  ") ? 0 : NB_ERR_NOMEM);

		if (rc == 0)
			rc = (nb_buf_append(&rendered, safe + row->offset, row->length)
				      ? 0
				      : NB_ERR_NOMEM);

		if (rc == 0)
			rc = (nb_buf_append_cstr(&rendered, "\r\n") ? 0 : NB_ERR_NOMEM);
	}

	if (rc == 0)
		rc = nt_draw_write(output, &rendered);

	nt_text_layout_deinit(&view);
	nb_buf_cleanup(&rendered);
	nb_buf_cleanup(&text);
	free(safe);
	return rc;
}
