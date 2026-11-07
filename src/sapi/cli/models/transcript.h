#ifndef MORPH_CLI_MODELS_TRANSCRIPT_H
#define MORPH_CLI_MODELS_TRANSCRIPT_H

#include "util/arena.h"
#include "util/array.h"
#include "util/buf.h"
#include "util/utf8.h"

struct cli_transcript_tool {
	const char *id;
	const char *name;
	const char *args;
	const char *subject;
	const char *result;
	const char *error;
	const char *summary;
	const char *patch_preview;
	morph_buf_t stream;
	struct utf8_terminal_sanitizer sanitizer;
	int state;
	int timed_out;
	int historical;
	double started;
	double ended;
};

struct cli_transcript_reasoning {
	size_t after_tools;
	morph_buf_t text;
};

struct cli_transcript_model {
	struct arena *arena;
	morph_array_t tools;
	morph_array_t reasoning;
	int finished;
	int allocation_failed;
};

int cli_transcript_model_create(struct cli_transcript_model **out);
void cli_transcript_model_destroy(struct cli_transcript_model *model);

#endif
