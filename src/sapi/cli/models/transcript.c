#include "sapi/cli/models/transcript.h"
#include "util/error.h"

#include <stdlib.h>
#include <stdio.h>

int cli_transcript_model_create(struct cli_transcript_model **out)
{
	struct cli_transcript_model *model;
	struct cli_transcript_tool tool;
	struct cli_transcript_reasoning reasoning;
	int rc;

	if (!out)
		return -EINVAL;
	*out = NULL;
	model = calloc(1, sizeof(*model));
	if (!model)
		return -ENOMEM;
	model->arena = arena_create(BUFSIZ);
	if (!model->arena) {
		free(model);
		return -ENOMEM;
	}
	rc = morph_array_init(&model->tools, MORPH_ARRAY_INIT_CAP, sizeof(tool));
	if (!rc)
		rc = morph_array_init(&model->reasoning, MORPH_ARRAY_INIT_CAP, sizeof(reasoning));
	if (rc) {
		cli_transcript_model_destroy(model);
		return rc;
	}
	*out = model;
	return 0;
}

void cli_transcript_model_destroy(struct cli_transcript_model *model)
{
	struct cli_transcript_reasoning *reasoning;

	if (!model)
		return;
	morph_array_foreach (reasoning, &model->reasoning, struct cli_transcript_reasoning)
		morph_buf_cleanup(&reasoning->text);
	morph_array_cleanup(&model->reasoning);
	morph_array_cleanup(&model->tools);
	arena_destroy(model->arena);
	free(model);
}
