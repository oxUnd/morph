#ifndef MORPH_PROMPT_TEMPLATE_H
#define MORPH_PROMPT_TEMPLATE_H

#include <limits.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MORPH_PROMPT_TEMPLATE_VERSION 2
#define MORPH_PROMPT_TEMPLATE_FILE_MAX (256u * 1024u)
#define MORPH_PROMPT_TEMPLATE_TOTAL_MAX (1024u * 1024u)
#define MORPH_PROMPT_TEMPLATE_OUTPUT_MAX (1024u * 1024u)
#define MORPH_PROMPT_TEMPLATE_VARIABLES_MAX 256u
#define MORPH_PROMPT_TEMPLATE_FILES_MAX 128u
#define MORPH_PROMPT_TEMPLATE_INCLUDE_DEPTH 16u
#define MORPH_PROMPT_TEMPLATE_SECTION_DEPTH 32u

enum morph_prompt_template_type {
	MORPH_PROMPT_TEMPLATE_STRING,
	MORPH_PROMPT_TEMPLATE_BOOL
};

struct morph_prompt_template_variable {
	const char *name;
	enum morph_prompt_template_type type;
	const char *string;
	int boolean;
};

struct morph_prompt_template_error {
	char path[PATH_MAX];
	size_t line;
	size_t column;
	char message[256];
};

/* Render an in-memory syntax v2 template, including all v1 forms.
 * Partials require render_file instead.
 * Bindings are borrowed; free(*out) on success. Failure leaves *out NULL.
 * error is optional. Input and string bindings are NUL-terminated UTF-8. */
int morph_prompt_template_render(const char *text,
		const struct morph_prompt_template_variable *variables, size_t count,
		char **out, struct morph_prompt_template_error *error);

/* Render a syntax v2 entry below root, including all v1 forms, with rooted partials.
 * Bindings are borrowed; free(*out) on success.
 * All branches are validated. Failure leaves *out NULL and never writes files.
 * error is optional; see docs/prompt-templates.md for the full contract. */
int morph_prompt_template_render_file(const char *root, const char *entry,
		const struct morph_prompt_template_variable *variables, size_t count,
		char **out, struct morph_prompt_template_error *error);

#ifdef __cplusplus
}
#endif

#endif
