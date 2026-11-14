#include "sapi/cli/commands/mcp_manage.h"
#include "nb/cleanup.h"
#include "mcp/mcp.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/services/management.h"
#include "http/client.h"
#include "runtime/mcp.h"
#include "nb/vec.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/str.h"
#include "nb/toml.h"
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct cli_mcp_manage_mcp_options {
	const char *action;
	const char *name;
	const char *url;
	const char *transport;
	const char *token_env;
	const char *config;
	const char *env[MCP_ENV_MAX];
	int env_count;
	int command_index;
	int replace;
};

static void cli_mcp_manage_mcp_usage(void)
{
	fputs("Usage: morph mcp <add|list|get|remove|test> [options]\n"
	      "  add NAME -- COMMAND [ARGS...]\n"
	      "  add NAME --transport http URL\n"
	      "  add NAME --url URL\n"
	      "  list | get NAME | remove NAME | test NAME\n"
	      "Options:\n"
	      "  --env KEY=VALUE             Repeatable stdio environment\n"
	      "  --auth-token-env NAME       HTTP token environment variable\n"
	      "  --bearer-token-env-var NAME Alias for --auth-token-env\n"
	      "  --transport stdio|http     Default: stdio (http with --url)\n"
	      "  --replace                  Replace an existing server\n"
	      "  --scope user               User scope only\n"
	      "  -c, --config PATH          Configuration file\n",
	      stdout);
}

enum cli_mcp_manage_mcp_option_id {
	CLI_MCP_MANAGE_MCP_OPTION_URL = 256,
	CLI_MCP_MANAGE_MCP_OPTION_TOKEN_ENV,
	CLI_MCP_MANAGE_MCP_OPTION_REPLACE,
	CLI_MCP_MANAGE_MCP_OPTION_SCOPE,
};

static const struct option cli_mcp_manage_long_options[] = {
	{"url", required_argument, NULL, CLI_MCP_MANAGE_MCP_OPTION_URL},
	{"transport", required_argument, NULL, 't'},
	{"auth-token-env", required_argument, NULL, CLI_MCP_MANAGE_MCP_OPTION_TOKEN_ENV},
	{"bearer-token-env-var", required_argument, NULL, CLI_MCP_MANAGE_MCP_OPTION_TOKEN_ENV},
	{"env", required_argument, NULL, 'e'},
	{"replace", no_argument, NULL, CLI_MCP_MANAGE_MCP_OPTION_REPLACE},
	{"scope", required_argument, NULL, CLI_MCP_MANAGE_MCP_OPTION_SCOPE},
	{"config", required_argument, NULL, 'c'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0},
};

static int cli_mcp_manage_mcp_parse(int argc, char **argv, const char *path,
				    struct cli_mcp_manage_mcp_options *options)
{
	int prefix_argc = argc;
	int option;

	memset(options, 0, sizeof(*options));
	options->action = argc ? argv[0] : "--help";
	options->config = path;
	if (strcmp(options->action, "--help") == 0 || strcmp(options->action, "-h") == 0) {
		options->action = "--help";
		return 0;
	}
	/* Parse only our prefix; leave server arguments untouched. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			options->command_index = i + 1;
			prefix_argc = i;
			if (i + 1 == argc)
				return NB_ERR_INVAL;
			break;
		}
	}
	cli_management_reset_getopt();
	while ((option = getopt_long(prefix_argc, argv, "c:t:e:h", cli_mcp_manage_long_options,
				     NULL)) != -1) {
		switch (option) {
		case 'c':
			options->config = optarg;
			break;
		case 't':
			options->transport = optarg;
			break;
		case 'e':
			if (options->env_count == MCP_ENV_MAX)
				return NB_ERR_TOO_BIG;
			options->env[options->env_count++] = optarg;
			break;
		case CLI_MCP_MANAGE_MCP_OPTION_URL:
			options->url = optarg;
			break;
		case CLI_MCP_MANAGE_MCP_OPTION_TOKEN_ENV:
			options->token_env = optarg;
			break;
		case CLI_MCP_MANAGE_MCP_OPTION_REPLACE:
			options->replace = 1;
			break;
		case CLI_MCP_MANAGE_MCP_OPTION_SCOPE:
			if (strcmp(optarg, "user") != 0) {
				fputs("morph: MCP currently supports --scope user only\n", stderr);
				return NB_ERR_INVAL;
			}
			break;
		case 'h':
			options->action = "--help";
			return 0;
		default:
			return NB_ERR_INVAL;
		}
	}
	if (optind < prefix_argc)
		options->name = argv[optind++];
	if (optind < prefix_argc && !options->url)
		options->url = argv[optind++];
	if (optind != prefix_argc)
		return NB_ERR_INVAL;
	if (strcmp(options->action, "list") != 0 &&
	    (!options->name || !cli_management_name_valid(options->name) ||
	     strlen(options->name) >= MCP_SERVER_NAME_MAX))
		return NB_ERR_INVAL;
	if (strcmp(options->action, "add") != 0 &&
	    (options->url || options->transport || options->token_env || options->env_count ||
	     options->replace || options->command_index))
		return NB_ERR_INVAL;
	if (strcmp(options->action, "list") == 0 && options->name)
		return NB_ERR_INVAL;
	return 0;
}

/* JSON string quoting is also valid TOML basic-string quoting. */
static int cli_mcp_manage_mcp_quote(nb_buf *buffer, const char *text)
{
	cJSON *item = cJSON_CreateString(text);
	char *quoted = item ? cJSON_PrintUnformatted(item) : NULL;
	int rc;

	cJSON_Delete(item);
	if (!quoted) {
		nb_buf_set_error(buffer, NB_ERR_NOMEM);
		return NB_ERR_NOMEM;
	}
	rc = (nb_buf_append_cstr(buffer, quoted) ? 0 : NB_ERR_NOMEM);
	free(quoted);
	return rc;
}

static int cli_mcp_manage_mcp_field(nb_buf *buffer, const char *key, const char *value)
{
	nb_buf_printf(buffer, "%s = ", key);
	cli_mcp_manage_mcp_quote(buffer, value);
	nb_buf_append_char(buffer, '\n');
	return nb_buf_status(buffer);
}

static int cli_mcp_manage_mcp_build(const struct cli_mcp_manage_mcp_options *options, int argc,
				    char **argv, nb_buf *buffer)
{
	const char *transport =
		options->transport ? options->transport : (options->url ? "http" : "stdio");

	if (strcmp(transport, "http") != 0 && strcmp(transport, "stdio") != 0)
		return NB_ERR_INVAL;
	if (strcmp(transport, "http") == 0) {
		if (!options->url || options->command_index || options->env_count ||
		    (strncmp(options->url, "https://", 8) != 0 &&
		     strncmp(options->url, "http://", 7) != 0) ||
		    strlen(options->url) >= PATH_MAX)
			return NB_ERR_INVAL;
	} else if (!options->command_index || options->url || options->token_env) {
		return NB_ERR_INVAL;
	}
	nb_buf_append_cstr(buffer, "\n[[mcp.servers]]\n");
	cli_mcp_manage_mcp_field(buffer, "name", options->name);
	cli_mcp_manage_mcp_field(buffer, "transport", transport);
	if (options->url)
		cli_mcp_manage_mcp_field(buffer, "url", options->url);
	if (options->token_env) {
		if (!(nb_str_is_alnum(nb_str_from_cstr(options->token_env), "_") &&
		      !((options->token_env)[0] >= '0' && (options->token_env)[0] <= '9')) ||
		    strlen(options->token_env) >= 64)
			return NB_ERR_INVAL;
		cli_mcp_manage_mcp_field(buffer, "auth_token_env", options->token_env);
	}
	if (options->command_index) {
		int index = options->command_index;

		if (!argv[index][0] || strlen(argv[index]) >= 256 ||
		    argc - index - 1 > MCP_CMD_ARGS_MAX)
			return NB_ERR_TOO_BIG;
		cli_mcp_manage_mcp_field(buffer, "command", argv[index++]);
		nb_buf_append_cstr(buffer, "args = [");
		for (int i = index; i < argc; i++) {
			if (strlen(argv[i]) >= MCP_CMD_ARG_LEN_MAX)
				return NB_ERR_TOO_BIG;
			if (i != index)
				nb_buf_append_cstr(buffer, ", ");
			cli_mcp_manage_mcp_quote(buffer, argv[i]);
		}
		nb_buf_append_cstr(buffer, "]\n");
	}
	if (options->env_count) {
		nb_buf_append_cstr(buffer, "env = { ");
		for (int i = 0; i < options->env_count; i++) {
			const char *pair = options->env[i];
			const char *equal = strchr(pair, '=');
			char key[64];

			if (!equal || equal == pair || (size_t)(equal - pair) >= sizeof(key) ||
			    strlen(equal + 1) >= MCP_ENV_VAL_MAX)
				return NB_ERR_INVAL;
			memcpy(key, pair, (size_t)(equal - pair));
			key[equal - pair] = '\0';
			if (!(nb_str_is_alnum(nb_str_from_cstr(key), "_") &&
			      !((key)[0] >= '0' && (key)[0] <= '9')))
				return NB_ERR_INVAL;
			if (i)
				nb_buf_append_cstr(buffer, ", ");
			cli_mcp_manage_mcp_quote(buffer, key);
			nb_buf_append_cstr(buffer, " = ");
			cli_mcp_manage_mcp_quote(buffer, equal + 1);
		}
		nb_buf_append_cstr(buffer, " }\n");
	}
	return nb_buf_status(buffer);
}

struct cli_mcp_manage_mcp_header {
	int line;
	int remove;
};

static const char *cli_mcp_manage_mcp_line(const char *text, int line)
{
	while (line > 1 && *text) {
		if (*text++ == '\n')
			line--;
	}
	return text;
}

static int cli_mcp_manage_mcp_headers(const nb_toml_value *node, const nb_toml_value *selected,
				      const char *text, int selected_parent, nb_vec *headers)
{
	int remove = selected_parent || node == selected;

	if (nb_toml_type_of(node) == NB_TOML_TABLE) {
		const char *line = cli_mcp_manage_mcp_line(text, nb_toml_location_of(node).line);

		while (*line == ' ' || *line == '\t')
			line++;
		if (nb_toml_location_of(node).line > 0 && *line == '[' &&
		    !nb_toml_location_of(node).inline_table) {
			struct cli_mcp_manage_mcp_header *header = nb_vec_push(headers);

			if (!header)
				return NB_ERR_NOMEM;
			header->line = nb_toml_location_of(node).line;
			header->remove = remove;
		}
		for (size_t i = 0; i < nb_toml_table_size(node); i++) {
			int rc = cli_mcp_manage_mcp_headers(
				nb_toml_table_value_at(node, (size_t)(i)), selected, text, remove,
				headers);

			if (rc != 0)
				return rc;
		}
	} else if (nb_toml_type_of(node) == NB_TOML_ARRAY) {
		for (size_t i = 0; i < nb_toml_array_size(node); i++) {
			int rc = cli_mcp_manage_mcp_headers(nb_toml_array_get(node, (size_t)(i)),
							    selected, text, remove, headers);

			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int cli_mcp_manage_mcp_header_compare(const void *left, const void *right)
{
	const struct cli_mcp_manage_mcp_header *a = left;
	const struct cli_mcp_manage_mcp_header *b = right;

	if (a->line == b->line)
		return a->remove - b->remove;
	return (a->line > b->line) - (a->line < b->line);
}

static int cli_mcp_manage_mcp_without_server(const nb_toml *parsed, const nb_toml_value *server,
					     const char *text, nb_buf *output)
{
	nb_vec headers = {0};
	const char *cursor = text;
	int rc = (nb_vec_init(&headers, sizeof(struct cli_mcp_manage_mcp_header)),
		  nb_vec_reserve(&headers, 16) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	rc = cli_mcp_manage_mcp_headers(nb_toml_root(parsed), server, text, 0, &headers);
	if (rc == 0) {
		qsort(((void *)headers.data), headers.len, headers.elem_size,
		      cli_mcp_manage_mcp_header_compare);
		for (size_t i = 0; i < headers.len; i++) {
			const struct cli_mcp_manage_mcp_header *header = nb_vec_at(&headers, i);
			const struct cli_mcp_manage_mcp_header *next = nb_vec_at(&headers, i + 1);
			const char *start;
			const char *end;

			if (!header->remove || (next && next->line == header->line))
				continue;
			start = cli_mcp_manage_mcp_line(text, header->line);
			end = next ? cli_mcp_manage_mcp_line(text, next->line)
				   : text + strlen(text);
			if (start < cursor)
				continue;
			nb_buf_append(output, cursor, (size_t)(start - cursor));
			/* Retain comments and whitespace introducing the next section. */
			const char *tail = end;
			while (tail > start) {
				const char *previous = tail - 1;
				const char *content;

				if (*previous == '\n' && previous > start)
					previous--;
				while (previous > start && previous[-1] != '\n')
					previous--;
				content = previous;
				while (*content == ' ' || *content == '\t' || *content == '\r')
					content++;
				if (*content != '#' && *content != '\n' && content != end)
					break;
				tail = previous;
			}
			nb_buf_append(output, tail, (size_t)(end - tail));
			cursor = end;
		}
		nb_buf_append_cstr(output, cursor);
		rc = nb_buf_status(output);
	}
	nb_vec_cleanup(&headers);
	return rc;
}

static int cli_mcp_manage_mcp_read_command(struct nb_log *logger,
					   const struct cli_mcp_manage_mcp_options *options,
					   const char *path, const nb_toml_value *servers)
{
	size_t count = nb_toml_type_of(servers) == NB_TOML_ARRAY ? nb_toml_array_size(servers) : 0;

	for (size_t i = 0; i < count; i++) {
		const nb_toml_value *server = nb_toml_array_get(servers, (size_t)(i));
		const nb_toml_value *name = nb_toml_table_get_cstr(server, "name");
		const nb_toml_value *transport = nb_toml_table_get_cstr(server, "transport");
		const nb_toml_value *target = nb_toml_table_get_cstr(
			server, nb_toml_type_of(transport) == NB_TOML_STRING &&
						strcmp(nb_toml_string(transport).ptr, "http") == 0
					? "url"
					: "command");

		if (nb_toml_type_of(name) != NB_TOML_STRING)
			continue;
		if (strcmp(options->action, "list") == 0) {
			printf("%s\t%s\t%s\n", nb_toml_string(name).ptr,
			       nb_toml_type_of(transport) == NB_TOML_STRING
				       ? nb_toml_string(transport).ptr
				       : "stdio",
			       nb_toml_type_of(target) == NB_TOML_STRING
				       ? nb_toml_string(target).ptr
				       : "");
			continue;
		}
		if (strcmp(options->name, nb_toml_string(name).ptr) != 0)
			continue;
		if (strcmp(options->action, "get") == 0) {
			printf("Name: %s\nTransport: %s\nTarget: %s\nConfig: %s\n",
			       nb_toml_string(name).ptr,
			       nb_toml_type_of(transport) == NB_TOML_STRING
				       ? nb_toml_string(transport).ptr
				       : "stdio",
			       nb_toml_type_of(target) == NB_TOML_STRING
				       ? nb_toml_string(target).ptr
				       : "",
			       path);
			const nb_toml_value *args = nb_toml_table_get_cstr(server, "args");
			const nb_toml_value *env = nb_toml_table_get_cstr(server, "env");
			const nb_toml_value *token =
				nb_toml_table_get_cstr(server, "auth_token_env");

			if (nb_toml_type_of(args) == NB_TOML_ARRAY) {
				for (size_t j = 0; j < nb_toml_array_size(args); j++) {
					if (nb_toml_type_of(nb_toml_array_get(args, (size_t)(j))) ==
					    NB_TOML_STRING)
						printf("Arg: %s\n",
						       nb_toml_string(
							       nb_toml_array_get(args, (size_t)(j)))
							       .ptr);
				}
			}
			if (nb_toml_type_of(env) == NB_TOML_TABLE) {
				for (size_t j = 0; j < nb_toml_table_size(env); j++)
					printf("Env: %s=<redacted>\n",
					       nb_toml_table_key_at(env, (size_t)(j)).ptr);
			}
			if (nb_toml_type_of(token) == NB_TOML_STRING)
				printf("Token environment: %s\n", nb_toml_string(token).ptr);
			return 0;
		}
		struct config *config = calloc(1, sizeof(*config));
		struct mcp_server_config connection;
		char message[BUFSIZ];
		int rc;

		if (!config)
			return NB_ERR_NOMEM;
		config_set_defaults(logger, config);
		rc = config_load(logger, config, path);
		if (rc == 0 && i >= (size_t)config->mcp.server_count)
			rc = NB_ERR_TOO_BIG;
		if (rc == 0)
			rc = runtime_mcp_server_config_from_config(&config->mcp.servers[i],
								   &connection);
		free(config);
		if (rc != 0)
			return rc;
		http_init(logger);
		rc = mcp_test_connection(&connection, message, sizeof(message));
		http_cleanup();
		printf("%s: %s\n", nb_toml_string(name).ptr, message);
		return rc;
	}
	if (strcmp(options->action, "list") == 0)
		return 0;
	return NB_ERR_NOT_FOUND;
}

static int cli_mcp_manage_mcp_apply(struct nb_log *logger,
				    const struct cli_mcp_manage_mcp_options *options, int argc,
				    char **argv, const char *path)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	nb_err parse_error = {0};
	char *text NB_CLEANUP(nb_charp_cleanup) =
		(nb_file_read_all(path, &file_value_1, 0, NULL) == NB_OK &&
				 (file_copy_1 = (char *)malloc(nb_buf_len(&file_value_1) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
				   nb_buf_len(&file_value_1) + 1),
			    nb_charp_steal(&file_copy_1))
			 : NULL);
	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) = {0};
	const nb_toml_value *servers = {0};
	const nb_toml_value *selected = NULL;
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	struct config_validation_error error = {0};
	int add = strcmp(options->action, "add") == 0;
	int remove = strcmp(options->action, "remove") == 0;
	int rc = 0;

	if (!text && errno == ENOENT)
		text = strdup("");
	if (!text)
		return NB_ERR_IO;
	if (strlen(text) > 1024 * 1024) {
		rc = (NB_ERR_TOO_BIG);
		return rc;
	}
	parsed = nb_toml_parse(nb_str_from_cstr(text), &parse_error);
	if (!parsed) {
		fprintf(stderr, "morph: invalid config: %s\n", parse_error.msg);
		rc = (NB_ERR_PARSE);
		return rc;
	}
	servers = nb_toml_table_get_cstr(nb_toml_table_get_cstr(nb_toml_root(parsed), "mcp"),
					 "servers");
	if (!add && !remove) {
		rc = cli_mcp_manage_mcp_read_command(logger, options, path, servers);
		return rc;
	}
	if (nb_toml_type_of(servers) == NB_TOML_ARRAY) {
		if (nb_toml_location_of(servers).line != 0) {
			fputs("morph: convert inline MCP servers to "
			      "[[mcp.servers]] before editing\n",
			      stderr);
			rc = (NB_ERR_UNSUPPORTED);
			return rc;
		}
		for (size_t i = 0; i < nb_toml_array_size(servers); i++) {
			const nb_toml_value *name = nb_toml_table_get_cstr(
				nb_toml_array_get(servers, (size_t)(i)), "name");

			if (nb_toml_type_of(name) == NB_TOML_STRING &&
			    strcmp(nb_toml_string(name).ptr, options->name) == 0) {
				if (selected) {
					rc = (NB_ERR_EXISTS);
					return rc;
				}
				selected = nb_toml_array_get(servers, (size_t)(i));
			}
		}
	}
	if ((add && selected && !options->replace) ||
	    (add && !selected && nb_toml_type_of(servers) == NB_TOML_ARRAY &&
	     nb_toml_array_size(servers) >= MCP_SERVER_MAX)) {
		rc = (NB_ERR_EXISTS);
		return rc;
	}
	if (remove && !selected) {
		rc = (NB_ERR_NOT_FOUND);
		return rc;
	}
	rc = (nb_buf_init(&output), nb_buf_reserve(&output, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (selected)
		rc = cli_mcp_manage_mcp_without_server(parsed, selected, text, &output);
	else
		rc = (nb_buf_append_cstr(&output, text) ? 0 : NB_ERR_NOMEM);
	if (rc == 0 && add)
		rc = cli_mcp_manage_mcp_build(options, argc, argv, &output);
	if (rc == 0)
		rc = config_validate_text(nb_buf_cstr(&output), &error);
	if (rc != 0 && error.message[0])
		fprintf(stderr, "morph: invalid config: %s\n", error.message);
	if (rc == 0)
		rc = nb_file_write_all_atomic(
			path, nb_buf_cstr(&output), output.len,
			&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	if (rc == 0)
		printf("%s MCP server '%s' in %s (user scope).\n", remove ? "Removed" : "Saved",
		       options->name, path);
	return rc;
}

int cli_mcp_manage_run(struct nb_log *logger, int argc, char **argv, const char *config_path)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};

	struct cli_mcp_manage_mcp_options options;
	char *path;
	char *lock_path;
	int fd = -1;
	int rc = cli_mcp_manage_mcp_parse(argc, argv, config_path, &options);

	if (rc != 0) {
		cli_mcp_manage_mcp_usage();
		return 2;
	}
	if (strcmp(options.action, "--help") == 0 || strcmp(options.action, "-h") == 0) {
		cli_mcp_manage_mcp_usage();
		return 0;
	}
	if (strcmp(options.action, "add") != 0 && strcmp(options.action, "list") != 0 &&
	    strcmp(options.action, "get") != 0 && strcmp(options.action, "remove") != 0 &&
	    strcmp(options.action, "test") != 0) {
		cli_mcp_manage_mcp_usage();
		return 2;
	}
	path = (nb_path_expand(&file_value_2,
			       options.config ? options.config : "~/.morph/config.toml",
			       NULL) == NB_OK
			? strdup(nb_buf_cstr(&file_value_2))
			: NULL);
	if (!path)
		return cli_management_error(NB_ERR_NOMEM, "configuration path");
	lock_path =
		(nb_buf_clear(&file_value_3), nb_buf_printf(&file_value_3, "%s%s", path, ".lock")
						      ? strdup(nb_buf_cstr(&file_value_3))
						      : NULL);
	if (!lock_path) {
		free(path);
		return cli_management_error(NB_ERR_NOMEM, "configuration lock");
	}
	if (strcmp(options.action, "add") == 0 || strcmp(options.action, "remove") == 0) {
		struct stat st;
		char *parent = strdup(path);
		char *slash = parent ? strrchr(parent, '/') : NULL;

		if (!parent)
			rc = NB_ERR_NOMEM;
		else if (slash) {
			*slash = '\0';
			rc = nb_file_mkdir(*parent ? parent : "/", true, NULL);
		}
		free(parent);
		if (rc == 0 && lstat(path, &st) == 0 && !S_ISREG(st.st_mode))
			rc = NB_ERR_LOOP;
		if (rc == 0) {
			fd = cli_management_lock(lock_path);
			if (fd < 0)
				rc = fd;
		}
	}
	if (rc == 0)
		rc = cli_mcp_manage_mcp_apply(logger, &options, argc, argv, path);
	if (fd >= 0)
		close(fd);
	free(lock_path);
	free(path);
	return rc == 0 ? 0 : cli_management_error(rc, "MCP command failed");
}
