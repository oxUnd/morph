#ifndef MORPH_ENVIRONMENT_CONTEXT_H
#define MORPH_ENVIRONMENT_CONTEXT_H

#include "prompt_context.h"
#include <nb/str.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Collected strings are owned until environment_context_cleanup(). */
struct environment_context {
	const char *cwd;
	const char *morph_executable;
	const char *shell;
	const char *os;
	const char *os_version;
	const char *distribution;
	const char *kernel_release;
	const char *arch;
	const char *runtime;
	const char *runtime_version;
	const char *host_os;
	const char *path_tools;
	const char *current_date;
	const char *timezone;
	const char *git_branch;
	const char *git_root;
	const char *sandbox;
};

enum environment_wsl {
	ENV_WSL_NONE,
	ENV_WSL_UNKNOWN,
	ENV_WSL_1,
	ENV_WSL_2,
};

enum environment_wsl environment_wsl_detect(nb_str release, bool have_interop);
int environment_context_create(struct environment_context *out,
			       const struct prompt_context_input *input);
void environment_context_cleanup(struct environment_context *ctx);
/* Pure serialization; does not read environment variables or the OS. */
int environment_context_render(const struct environment_context *ctx, nb_buf *out);

#ifdef __cplusplus
}
#endif
#endif
