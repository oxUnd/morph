#include "prompt_context.h"
#include "environment_context.h"
#include "util/error.h"
#include <errno.h>
#include <string.h>

int prompt_context_build(const struct prompt_context_provider *providers,
	size_t count, const struct prompt_context_input *input,
	struct arena *arena, const char **out)
{
	morph_buf_t buf;
	int rc;

	if (!out || !arena || (!providers && count))
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	for (size_t i = 0; i < count; i++) {
		if (!providers[i].name || !*providers[i].name ||
		    !providers[i].collect_render)
			MORPH_RETURN(-EINVAL);
		for (size_t j = 0; j < i; j++) {
			if (!strcmp(providers[i].name, providers[j].name))
				MORPH_RETURN(-EINVAL);
		}
	}
	rc = morph_buf_init_arena(&buf, arena, 1024);
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < count; i++) {
		rc = providers[i].collect_render(input, arena, &buf);
		if (rc != 0)
			return rc;
	}
	if (buf.failed)
		MORPH_RETURN(buf.failed);
	*out = morph_buf_cstr(&buf);
	return 0;
}

static int collect_environment(const struct prompt_context_input *input,
	struct arena *arena, morph_buf_t *out)
{
	struct environment_context ctx;
	int rc = environment_context_collect(&ctx, input, arena);

	if (rc != 0)
		return rc;
	return environment_context_render(&ctx, out);
}

int prompt_context_build_default(const struct prompt_context_input *input,
	struct arena *arena, const char **out)
{
	static const struct prompt_context_provider providers[] = {
		{ "environment_context", collect_environment },
	};

	return prompt_context_build(providers,
		sizeof(providers) / sizeof(providers[0]), input, arena, out);
}
