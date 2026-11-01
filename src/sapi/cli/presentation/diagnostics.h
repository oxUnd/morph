#ifndef MORPH_CLI_PRESENTATION_DIAGNOSTICS_H
#define MORPH_CLI_PRESENTATION_DIAGNOSTICS_H

struct cli_application;
struct config_validation_error;

void cli_diagnostics_config(struct cli_application *, int is_warning, const char *path,
			    const struct config_validation_error *);
void cli_diagnostics_config_error(struct cli_application *, const char *config_path, int result);
int cli_diagnostics_preflight(struct cli_application *, const char *config_path);

#endif
