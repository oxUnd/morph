#include "sapi/cli/presentation/tool_preview.h"
#include "sapi/cli/models/transcript.h"
#include "agent/patch.h"
#include "util/json.h"
#include <errno.h>
#include <string.h>

int cli_tool_patch_preview(const struct cli_transcript_tool *tool, const char *workdir,
			   morph_buf_t *preview)
{
	struct json *args;
	const char *input;
	char error[256];
	int rc;

	if (!tool || !preview || !workdir || !*workdir || tool->historical ||
	    strcmp(tool->name, "apply_patch") != 0)
		return -ENOENT;
	args = json_parse_prefix(tool->args, NULL, NULL);
	input = json_string_value(json_member(args, "input"));
	rc = input ? patch_preview(workdir, input, preview, error, sizeof(error)) : -EINVAL;
	json_free(args);
	return rc;
}
