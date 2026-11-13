#include "sapi/cli/presentation/tool_preview.h"
#include "nb/err.h"
#include "sapi/cli/models/transcript.h"
#include "agent/patch.h"
#include "cJSON.h"
#include <errno.h>
#include <string.h>

int cli_tool_patch_preview(const struct cli_transcript_tool *tool, const char *workdir,
			   nb_buf *preview)
{
	cJSON *args;
	const char *input;
	char error[256];
	int rc;

	if (!tool || !preview || !workdir || !*workdir || tool->historical ||
	    strcmp(tool->name, "apply_patch") != 0)
		return NB_ERR_NOT_FOUND;
	args = cJSON_Parse(tool->args);
	input = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "input"));
	rc = input ? patch_preview(workdir, input, preview, error, sizeof(error)) : NB_ERR_INVAL;
	cJSON_Delete(args);
	return rc;
}
