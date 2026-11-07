#ifndef MANIFEST_H
#define MANIFEST_H

struct morph_log;
#ifdef __cplusplus
extern "C" {
#endif

#include "ext.h"

/* Borrow input for the call; out must have no owned fields. Success creates an
 * independent manifest owner; failure leaves out unchanged. Occupied: -EEXIST. */
int manifest_parse(struct morph_log *logger, const char *toml_data, struct ext_manifest *out);
int manifest_parse_file(struct morph_log *logger, const char *path, struct ext_manifest *out);

#ifdef __cplusplus
}
#endif

#endif
