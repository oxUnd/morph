#include "image_input.h"

#include "agent/tool_result.h"
#include "permission/context.h"
#include "util/error.h"
#include "util/image_util.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static int image_input_error(struct tool_result *result, int rc)
{
	const char *message = "failed to load image";
	const char *code = "image_load_failed";
	int result_rc;

	if (rc == -ENOENT)
		code = "not_found";
	else if (rc == -EPERM || rc == -EACCES) {
		code = "permission_denied";
		message = "read path outside workspace: permission denied";
	} else if (rc != MORPH_ERR_FORMAT) {
		message = morph_strerror(rc);
	}
	result_rc = tool_result_error(result, code, message);
	return result_rc != 0 ? result_rc : rc;
}

int tool_image_load_input(struct tool_context *context, const char *path,
			  struct tool_result *result, struct tool_image_input *image)
{
	char resolved[PATH_MAX];
	int rc;

	if (!path || !result || !image)
		return -EINVAL;
	memset(image, 0, sizeof(*image));
	if (context) {
		rc = tool_context_authorize_path(context, TOOL_PATH_READ, path, resolved,
						 sizeof(resolved));
		if (rc != 0)
			return image_input_error(result, rc);
	} else {
		if (strlen(path) >= sizeof(resolved))
			return image_input_error(result, -ENAMETOOLONG);
		memcpy(resolved, path, strlen(path) + 1);
	}
	image->pixels =
		image_load_bounded(resolved, &image->width, &image->height, &image->channels, 0);
	return image->pixels ? 0 : image_input_error(result, MORPH_ERR_FORMAT);
}
