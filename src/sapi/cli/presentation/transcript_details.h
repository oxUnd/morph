#ifndef MORPH_CLI_PRESENTATION_TRANSCRIPT_DETAILS_H
#define MORPH_CLI_PRESENTATION_TRANSCRIPT_DETAILS_H

#include <nb/buf.h>

struct cli_transcript_model;

int cli_transcript_details_append(nb_buf *, const struct cli_transcript_model *,
	int columns, int styled, int raw);

#endif
