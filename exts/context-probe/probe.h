#ifndef MORPH_CONTEXT_PROBE_H
#define MORPH_CONTEXT_PROBE_H

#include <morph/ext.h>

#ifdef __cplusplus
extern "C" {
#endif

int ext_run_v2(const struct ext_context *context, const char *args_json, char **result_json);

#ifdef __cplusplus
}
#endif

#endif
