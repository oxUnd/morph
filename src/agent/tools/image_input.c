#include "image_input.h"

#include "agent/tool_result.h"
#include "permission/context.h"
#include "errors.h"
#include "media/image.h"
#include "nb/cleanup.h"
#include "stb_image.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static int image_input_error(struct tool_result *result, int rc)
{
	const char *message = "failed to load image";
	const char *code = "image_load_failed";
	int result_rc;

	if (rc == NB_ERR_NOT_FOUND)
		code = "not_found";
	else if (rc == NB_ERR_NOT_PERMITTED || rc == NB_ERR_PERMISSION) {
		code = "permission_denied";
		message = "read path outside workspace: permission denied";
	} else if (rc != NB_ERR_FORMAT) {
		message = nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc);
	}
	result_rc = tool_result_error_copy(result, code, message);
	return result_rc != 0 ? result_rc : rc;
}

void tool_image_input_cleanup(struct tool_image_input *image)
{
	if (!image)
		return;
	stbi_image_free(image->pixels);
	memset(image, 0, sizeof(*image));
}

int tool_image_load_input(struct tool_context *context, const char *path,
			  struct tool_result *result, struct tool_image_input *image)
{
	struct tool_image_input candidate NB_CLEANUP(tool_image_input_cleanup) = {0};
	char resolved[PATH_MAX];
	int rc;

	if (!path || !result || !image)
		return NB_ERR_INVAL;
	if (image->pixels)
		return NB_ERR_STATE;
	if (context) {
		rc = tool_context_authorize_path(context, TOOL_PATH_READ, path, resolved,
						 sizeof(resolved));
		if (rc != 0)
			return image_input_error(result, rc);
	} else {
		if (strlen(path) >= sizeof(resolved))
			return image_input_error(result, NB_ERR_NAME_TOO_LONG);
		memcpy(resolved, path, strlen(path) + 1);
	}
	candidate.pixels = image_load_bounded_copy(resolved, &candidate.width, &candidate.height,
						   &candidate.channels, 0);
	if (!candidate.pixels)
		return image_input_error(result, NB_ERR_FORMAT);
	*image = candidate;
	memset(&candidate, 0, sizeof(candidate));
	return 0;
}
