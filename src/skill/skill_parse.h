#ifndef SKILL_PARSE_H
#define SKILL_PARSE_H

struct nb_log;
#ifdef __cplusplus
extern "C" {
#endif

#include "skill.h"

/* Input text/path is borrowed for the call. Optional body_out must be NULL;
 * success returns independent malloc-owned text, released with free(). */
int skill_parse_file(struct nb_log *logger, const char *path, struct skill_frontmatter *fm,
		     char **body_out);
int skill_parse(struct nb_log *logger, const char *data, size_t len, struct skill_frontmatter *fm,
		char **body_out);
int skill_parse_frontmatter(struct nb_log *logger, const char *path, struct skill_frontmatter *fm);

#ifdef __cplusplus
}
#endif

#endif
