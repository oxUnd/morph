#include "sapi/cli/services/configuration/config.h"
#include "sapi/cli/services/configuration/role.h"
#include "config/edit.h"
#include "tomlc17.h"
#include <string.h>

int cli_setup_config_merge_copy(const char *old, const char *patch, char **out)
{
	return config_text_merge_copy(old, patch, out);
}

int cli_setup_config_credentials_copy(const char *text, char **out)
{
	return config_text_credentials_copy(text, out);
}

int cli_setup_config_mcp_add_copy(const char *text, const char *server, char **out)
{
	return config_text_mcp_add_copy(text, server, out);
}

int cli_setup_config_role(const char *text)
{
	toml_result_t parsed = toml_parse(text, (int)strlen(text));
	toml_datum_t role = toml_get(toml_get(parsed.toptab, "setup"), "role");
	int selected = role.type == TOML_STRING ? cli_setup_role_find(role.u.s) : CLI_SETUP_GENERAL;
	toml_free(parsed);
	return selected;
}
