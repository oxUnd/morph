#ifndef SKILL_PARSE_H
#define SKILL_PARSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "skill.h"

int skill_parse_file(const char *path, struct skill_frontmatter *fm, char **body_out);
int skill_parse(const char *data, size_t len, struct skill_frontmatter *fm, char **body_out);
int skill_parse_frontmatter(const char *path, struct skill_frontmatter *fm);

#ifdef __cplusplus
}
#endif

#endif
