#include "sapi/cli/services/configuration/config.h"
#include "sapi/cli/services/configuration/role.h"
#include "config/edit.h"
#include "nb/toml.h"
#include "nb/cleanup.h"
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
	nb_err parse_error = {0};
	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) =
		nb_toml_parse(nb_str_from_cstr(text), &parse_error);
	const nb_toml_value *role = nb_toml_table_get_cstr(
		nb_toml_table_get_cstr(nb_toml_root(parsed), "setup"), "role");
	int selected = nb_toml_type_of(role) == NB_TOML_STRING
			       ? cli_setup_role_find(nb_toml_string(role).ptr)
			       : CLI_SETUP_GENERAL;
	nb_toml_cleanup(&parsed);
	return selected;
}
