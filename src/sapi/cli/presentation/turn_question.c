#include "sapi/cli/presentation/turn_question.h"

#include "sapi/cli/presentation/style.h"
#include "terminal/text.h"
#include <nb/err.h>
#include <stdio.h>

int cli_turn_question_render(nb_buf *out, const char *question, int columns, int styled, int ultra)
{
	if (!out || !question)
		return NB_ERR_INVAL;

	if (columns < 2)
		return 0;

	char clipped[BUFSIZ];
	morph_terminal_copy_ellipsized_display_width(clipped, sizeof(clipped),
		question, (size_t)columns - 2, 0);
	const char *marker = ultra ? "» " : styled ? "› " : "> ";
	const char *tone = ultra ? CLI_INPUT_ULTRA_COLOR : CLI_INPUT_MARKER_COLOR;
	return nb_buf_printf(out, "%s%s%s%s%s%s", styled ? CLI_INPUT_BACKGROUND : "",
		styled ? tone : "", marker, styled ? ANSI_RESET CLI_INPUT_BACKGROUND : "",
		clipped, styled ? ANSI_RESET : "") ? 0 : nb_buf_status(out);
}
