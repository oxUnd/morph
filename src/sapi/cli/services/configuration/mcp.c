#include "sapi/cli/services/configuration/mcp.h"
#include "util/json.h"
#include "sapi/cli/services/configuration/config.h"
#include "sapi/cli/services/configuration/role.h"
#include "sapi/cli/models/setup.h"
#include "util/buf.h"
#include "util/data.h"
#include "util/error.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int cli_mcp_browser_role(int role)
{
	return role == CLI_SETUP_DEVELOPER || role == CLI_SETUP_RESEARCHER;
}

#ifdef __APPLE__
static int cli_mcp_desktop_role(int role)
{
	return role == CLI_SETUP_DEVELOPER || role == CLI_SETUP_PHOTOGRAPHER ||
	       role == CLI_SETUP_PHOTO_EDITOR || role == CLI_SETUP_DESIGNER ||
	       role == CLI_SETUP_VIDEO_CREATOR;
}
#endif

const char *cli_setup_mcp_summary(int role)
{
#ifdef __APPLE__
	if (role == CLI_SETUP_DEVELOPER)
		return "MCP: Exa, Playwright, Computer Use.";
	if (role == CLI_SETUP_DESIGNER)
		return "MCP: Exa, Computer Use, Figma (manual).";
	if (cli_mcp_desktop_role(role))
		return "MCP: Exa, Computer Use.";
#else
	if (role == CLI_SETUP_DESIGNER)
		return "MCP: Exa, Figma (manual).";
#endif
	if (cli_mcp_browser_role(role))
		return "MCP: Exa, Playwright.";
	return "MCP: Exa search and webpage reading.";
}

static int cli_mcp_program_available(const char *name)
{
	const char *search = getenv("PATH");
	if (!search)
		return 0;
	morph_buf_t path = {0};
	int rc = morph_buf_init(&path, 128);
	if (rc != 0)
		return rc;
	for (const char *part = search;;) {
		const char *colon = strchr(part, ':');
		size_t len = colon ? (size_t)(colon - part) : strlen(part);
		morph_buf_reset(&path);
		rc = len ? morph_buf_append(&path, part, len) : morph_buf_putc(&path, '.');
		if (rc == 0)
			rc = morph_buf_printf(&path, "/%s", name);
		if (rc != 0)
			break;
		struct stat st;
		if (stat(morph_buf_cstr(&path), &st) == 0 && S_ISREG(st.st_mode) &&
		    access(morph_buf_cstr(&path), X_OK) == 0) {
			rc = 1;
			break;
		}
		if (!colon)
			break;
		part = colon + 1;
	}
	morph_buf_cleanup(&path);
	return rc;
}

static int cli_mcp_add_server(char **text, const char *server)
{
	char *next = NULL;
	int rc = cli_setup_config_mcp_add(*text, server, &next);
	if (rc != 0)
		return rc;
	free(*text);
	*text = next;
	return 0;
}

#ifdef __APPLE__
static int cli_mcp_add_desktop(char **text)
{
	int python = cli_mcp_program_available("python3");
	int uvx = cli_mcp_program_available("uvx");
	if (python < 0 || uvx < 0)
		return python < 0 ? python : uvx;
	char adapter[PATH_MAX];
	int found = morph_data_find(adapter, sizeof(adapter), "scripts/macos_mcp_adapter.py") == 0;
	morph_buf_t server = {0};
	int rc = morph_buf_init(&server, 0);
	if (rc == 0)
		rc = morph_buf_puts(
			&server,
			"[[mcp.servers]]\nname = 'macos-computer-use'\ntransport = 'stdio'\n");
	if (rc == 0 && found) {
		struct json *value = json_string(adapter);
		char *encoded = value ? json_stringify(value) : NULL;
		json_free(value);
		rc = encoded ? morph_buf_printf(&server,
						"command = 'python3'\nargs = [%s, '--', "
						"'uvx', 'macos-computer-use-kit==0.3.2', 'mcp']\n",
						encoded)
			     : -ENOMEM;
		free(encoded);
	} else if (rc == 0) {
		rc = morph_buf_puts(&server, "command = 'uvx'\n"
					     "args = ['macos-computer-use-kit==0.3.2', 'mcp']\n");
	}
	if (rc == 0)
		rc = morph_buf_printf(&server, "auto_connect = %s\nconnect_timeout = 120\n",
				      uvx && (!found || python) ? "true" : "false");
	if (rc == 0)
		rc = cli_mcp_add_server(text, morph_buf_cstr(&server));
	morph_buf_cleanup(&server);
	return rc;
}
#endif

int cli_setup_mcp_apply(const char *text, int role, char **out)
{
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!text)
		return -EINVAL;
	char *current = strdup(text);
	if (!current)
		return -ENOMEM;
	int rc = cli_mcp_add_server(
		&current,
		"[[mcp.servers]]\nname = 'exa'\ntransport = 'http'\n"
		"url = 'https://mcp.exa.ai/mcp'\nauto_connect = true\nconnect_timeout = 30\n");
	if (rc == 0 && cli_mcp_browser_role(role)) {
		int npx = cli_mcp_program_available("npx");
		if (npx < 0)
			rc = npx;
		else
			rc = cli_mcp_add_server(
				&current,
				npx ? "[[mcp.servers]]\nname='playwright'\ntransport='stdio'"
				      "\ncommand='npx'\n"
				      "args=['-y','@playwright/"
				      "mcp@0.0.83']\nauto_connect=true\nconnect_timeout=120\n"
				    : "[[mcp.servers]]\nname='playwright'\ntransport='stdio'"
				      "\ncommand='npx'\n"
				      "args=['-y','@playwright/"
				      "mcp@0.0.83']\nauto_connect=false\nconnect_timeout=120\n");
	}
#ifdef __APPLE__
	if (rc == 0 && cli_mcp_desktop_role(role))
		rc = cli_mcp_add_desktop(&current);
#endif
	if (rc == 0 && role == CLI_SETUP_DESIGNER)
		rc = cli_mcp_add_server(&current,
					"[[mcp.servers]]\nname='figma-desktop'\ntransport='http'\n"
					"url='http://127.0.0.1:3845/"
					"mcp'\nauto_connect=false\nconnect_timeout=10\n");
	if (rc == 0)
		*out = current;
	else
		free(current);
	return rc;
}

int cli_setup_mcp_notes(int role, morph_buf_t *output)
{
	morph_buf_printf(output, "%s\n", cli_setup_mcp_summary(role));
	if (cli_mcp_browser_role(role) && cli_mcp_program_available("npx") == 0)
		morph_buf_printf(output, "To connect Playwright, install Node.js 18+ with npx, "
					 "then /mcp connect playwright.\n");
#ifdef __APPLE__
	if (cli_mcp_desktop_role(role))
		morph_buf_printf(
			output,
			"Computer Use needs macOS Accessibility and Screen Recording access.\n");
	if (cli_mcp_desktop_role(role) && cli_mcp_program_available("uvx") == 0)
		morph_buf_printf(output, "Install uvx, then /mcp connect macos-computer-use.\n");
	if (cli_mcp_desktop_role(role) && cli_mcp_program_available("python3") == 0)
		morph_buf_printf(output,
				 "Install Python 3 to use the Computer Use launch adapter.\n");
#endif
	if (role == CLI_SETUP_DESIGNER)
		morph_buf_printf(
			output,
			"Enable Figma's desktop MCP server, then /mcp connect figma-desktop.\n");
	return output->failed;
}
