#ifndef MORPH_CLI_PRESENTATION_TURN_QUESTION_H
#define MORPH_CLI_PRESENTATION_TURN_QUESTION_H

#include <nb/buf.h>

int cli_turn_question_render(nb_buf *, const char *question, int columns, int styled, int ultra);

#endif
