#ifndef MANIFEST_H
#define MANIFEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "skill.h"

int manifest_parse(const char *toml_data, struct skill_manifest *out);
int manifest_parse_file(const char *path, struct skill_manifest *out);

#ifdef __cplusplus
}
#endif

#endif