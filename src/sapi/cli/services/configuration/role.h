#ifndef MORPH_CLI_SERVICES_CONFIGURATION_ROLE_H
#define MORPH_CLI_SERVICES_CONFIGURATION_ROLE_H

#include "sapi/cli/models/setup.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cli_setup_role cli_setup_role_get(int index);
int cli_setup_role_find(const char *id);
int cli_setup_role_requires(int role, int model);
int cli_setup_role_render_copy(int role, char **out);

#ifdef __cplusplus
}
#endif
#endif
