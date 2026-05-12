#include "video_gen.h"
#include "util/log.h"
#include <errno.h>
#include <string.h>

int video_gen_create(struct model *self, const char *prompt,
		    const char *image_path, int duration,
		    struct video_result *result)
{
	(void)self;
	(void)image_path;
	(void)duration;
	if (!prompt || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	log_info("video_gen_create called (stub): %s", prompt);
	return -ENOSYS;
}