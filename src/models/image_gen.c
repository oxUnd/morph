#include "image_gen.h"
#include "util/log.h"
#include <errno.h>
#include <string.h>

int image_gen_create(struct model *self, const char *prompt, const char *style,
		    const char *size, struct image_result *result)
{
	(void)self;
	(void)style;
	(void)size;
	if (!prompt || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	log_info("image_gen_create called (stub): %s", prompt);
	return -ENOSYS;
}