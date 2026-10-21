#include "sapi/cli/manage.h"
#include "config/config.h"
#include "http/client.h"
#include "mcp/mcp.h"
#include "runtime/mcp.h"
#include "util/array.h"
#include "util/error.h"
#include "util/file.h"
#include "util/name.h"
#include "tomlc17.h"
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct mcp_options {
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

static void mcp_usage(void)
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
	      "  -c, --config PATH          Configuration file\n", stdout);
}

enum mcp_option_id {
	MCP_OPTION_URL = 256,
	MCP_OPTION_TOKEN_ENV,
	MCP_OPTION_REPLACE,
	MCP_OPTION_SCOPE,
};

static const struct option mcp_long_options[] = {
	{"url", required_argument, NULL, MCP_OPTION_URL},
	{"transport", required_argument, NULL, 't'},
	{"auth-token-env", required_argument, NULL, MCP_OPTION_TOKEN_ENV},
	{"bearer-token-env-var", required_argument, NULL, MCP_OPTION_TOKEN_ENV},
	{"env", required_argument, NULL, 'e'},
	{"replace", no_argument, NULL, MCP_OPTION_REPLACE},
	{"scope", required_argument, NULL, MCP_OPTION_SCOPE},
	{"config", required_argument, NULL, 'c'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0},
};

static int mcp_parse(int argc, char **argv, const char *path,
		     struct mcp_options *options)
{
	int prefix_argc = argc;
	int option;

	memset(options, 0, sizeof(*options));
	options->action = argc ? argv[0] : "--help";
	options->config = path;
	if (strcmp(options->action, "--help") == 0 ||
	    strcmp(options->action, "-h") == 0) {
		options->action = "--help";
		return 0;
	}
	/* Parse only our prefix; leave server arguments untouched. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			options->command_index = i + 1;
			prefix_argc = i;
			if (i + 1 == argc)
				MORPH_RETURN(-EINVAL);
			break;
		}
	}
	cli_manage_reset_getopt();
	while ((option = getopt_long(prefix_argc, argv, "c:t:e:h",
				     mcp_long_options, NULL)) != -1) {
		switch (option) {
		case 'c':
			options->config = optarg;
			break;
		case 't':
			options->transport = optarg;
			break;
		case 'e':
			if (options->env_count == MCP_ENV_MAX)
				MORPH_RETURN(-E2BIG);
			options->env[options->env_count++] = optarg;
			break;
		case MCP_OPTION_URL:
			options->url = optarg;
			break;
		case MCP_OPTION_TOKEN_ENV:
			options->token_env = optarg;
			break;
		case MCP_OPTION_REPLACE:
			options->replace = 1;
			break;
		case MCP_OPTION_SCOPE:
			if (strcmp(optarg, "user") != 0) {
				fputs("morph: MCP currently supports --scope user only\n",
				      stderr);
				MORPH_RETURN(-EINVAL);
			}
			break;
		case 'h':
			options->action = "--help";
			return 0;
		default:
			MORPH_RETURN(-EINVAL);
		}
	}
	if (optind < prefix_argc)
		options->name = argv[optind++];
	if (optind < prefix_argc && !options->url)
		options->url = argv[optind++];
	if (optind != prefix_argc)
		MORPH_RETURN(-EINVAL);
	if (strcmp(options->action, "list") != 0 &&
	    (!options->name || !cli_manage_name_valid(options->name) ||
	     strlen(options->name) >= MCP_SERVER_NAME_MAX))
		MORPH_RETURN(-EINVAL);
	if (strcmp(options->action, "add") != 0 &&
	    (options->url || options->transport || options->token_env ||
	     options->env_count || options->replace || options->command_index))
		MORPH_RETURN(-EINVAL);
	if (strcmp(options->action, "list") == 0 && options->name)
		MORPH_RETURN(-EINVAL);
	return 0;
}

/* JSON string quoting is also valid TOML basic-string quoting. */
static int mcp_quote(morph_buf_t *buffer, const char *text)
{
	cJSON *item = cJSON_CreateString(text);
	char *quoted = item ? cJSON_PrintUnformatted(item) : NULL;
	int rc;

	cJSON_Delete(item);
	if (!quoted)
		MORPH_RETURN(buffer->failed = -ENOMEM);
	rc = morph_buf_puts(buffer, quoted);
	free(quoted);
	return rc;
}

static int mcp_field(morph_buf_t *buffer, const char *key, const char *value)
{
	morph_buf_printf(buffer, "%s = ", key);
	mcp_quote(buffer, value);
	morph_buf_putc(buffer, '\n');
	return buffer->failed;
}

static int mcp_build(const struct mcp_options *options, int argc, char **argv,
		     morph_buf_t *buffer)
{
	const char *transport = options->transport ? options->transport :
		(options->url ? "http" : "stdio");

	if (strcmp(transport, "http") != 0 && strcmp(transport, "stdio") != 0)
		MORPH_RETURN(-EINVAL);
	if (strcmp(transport, "http") == 0) {
		if (!options->url || options->command_index || options->env_count ||
		    (strncmp(options->url, "https://", 8) != 0 &&
		     strncmp(options->url, "http://", 7) != 0) ||
		    strlen(options->url) >= PATH_MAX)
			MORPH_RETURN(-EINVAL);
	} else if (!options->command_index || options->url || options->token_env) {
		MORPH_RETURN(-EINVAL);
	}
	morph_buf_puts(buffer, "\n[[mcp.servers]]\n");
	mcp_field(buffer, "name", options->name);
	mcp_field(buffer, "transport", transport);
	if (options->url)
		mcp_field(buffer, "url", options->url);
	if (options->token_env) {
		if (!morph_env_name_valid(options->token_env) ||
		    strlen(options->token_env) >= 64)
			MORPH_RETURN(-EINVAL);
		mcp_field(buffer, "auth_token_env", options->token_env);
	}
	if (options->command_index) {
		int index = options->command_index;

		if (!argv[index][0] || strlen(argv[index]) >= 256 ||
		    argc - index - 1 > MCP_CMD_ARGS_MAX)
			MORPH_RETURN(-E2BIG);
		mcp_field(buffer, "command", argv[index++]);
		morph_buf_puts(buffer, "args = [");
		for (int i = index; i < argc; i++) {
			if (strlen(argv[i]) >= MCP_CMD_ARG_LEN_MAX)
				MORPH_RETURN(-E2BIG);
			if (i != index)
				morph_buf_puts(buffer, ", ");
			mcp_quote(buffer, argv[i]);
		}
		morph_buf_puts(buffer, "]\n");
	}
	if (options->env_count) {
		morph_buf_puts(buffer, "env = { ");
		for (int i = 0; i < options->env_count; i++) {
			const char *pair = options->env[i];
			const char *equal = strchr(pair, '=');
			char key[64];

			if (!equal || equal == pair ||
			    (size_t)(equal - pair) >= sizeof(key) ||
			    strlen(equal + 1) >= MCP_ENV_VAL_MAX)
				MORPH_RETURN(-EINVAL);
			memcpy(key, pair, (size_t)(equal - pair));
			key[equal - pair] = '\0';
			if (!morph_env_name_valid(key))
				MORPH_RETURN(-EINVAL);
			if (i)
				morph_buf_puts(buffer, ", ");
			mcp_quote(buffer, key);
			morph_buf_puts(buffer, " = ");
			mcp_quote(buffer, equal + 1);
		}
		morph_buf_puts(buffer, " }\n");
	}
	return buffer->failed;
}

struct mcp_header {
	int line;
	int remove;
};

static const char *mcp_line(const char *text, int line)
{
	while (line > 1 && *text) {
		if (*text++ == '\n')
			line--;
	}
	return text;
}

static int mcp_headers(const toml_datum_t *node, const toml_datum_t *selected,
		       const char *text, int selected_parent, morph_array_t *headers)
{
	int remove = selected_parent || node == selected;

	if (node->type == TOML_TABLE) {
		const char *line = mcp_line(text, node->lineno);

		while (*line == ' ' || *line == '\t')
			line++;
		if (node->lineno > 0 && *line == '[' &&
		    !(node->flag & TOML_FLAG_INLINED)) {
			struct mcp_header *header = morph_array_push(headers);

			if (!header)
				MORPH_RETURN(-ENOMEM);
			header->line = node->lineno;
			header->remove = remove;
		}
		for (int i = 0; i < node->u.tab.size; i++) {
			int rc = mcp_headers(&node->u.tab.value[i], selected,
				text, remove, headers);

			if (rc != 0)
				return rc;
		}
	} else if (node->type == TOML_ARRAY) {
		for (int i = 0; i < node->u.arr.size; i++) {
			int rc = mcp_headers(&node->u.arr.elem[i], selected,
				text, remove, headers);

			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

static int mcp_header_compare(const void *left, const void *right)
{
	const struct mcp_header *a = left;
	const struct mcp_header *b = right;

	if (a->line == b->line)
		return a->remove - b->remove;
	return (a->line > b->line) - (a->line < b->line);
}

static int mcp_without_server(const toml_result_t *parsed,
			      const toml_datum_t *server, const char *text,
			      morph_buf_t *output)
{
	morph_array_t headers = {0};
	const char *cursor = text;
	int rc = morph_array_init(&headers, 16, sizeof(struct mcp_header));

	if (rc != 0)
		return rc;
	rc = mcp_headers(&parsed->toptab, server, text, 0, &headers);
	if (rc == 0) {
		qsort(headers.elts, headers.nelts, headers.size, mcp_header_compare);
		for (size_t i = 0; i < headers.nelts; i++) {
			const struct mcp_header *header = morph_array_get(&headers, i);
			const struct mcp_header *next = morph_array_get(&headers, i + 1);
			const char *start;
			const char *end;

			if (!header->remove || (next && next->line == header->line))
				continue;
			start = mcp_line(text, header->line);
			end = next ? mcp_line(text, next->line) : text + strlen(text);
			if (start < cursor)
				continue;
			morph_buf_append(output, cursor, (size_t)(start - cursor));
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
			morph_buf_append(output, tail, (size_t)(end - tail));
			cursor = end;
		}
		morph_buf_puts(output, cursor);
		rc = output->failed;
	}
	morph_array_cleanup(&headers);
	return rc;
}

static int mcp_read_command(const struct mcp_options *options, const char *path,
			    const toml_datum_t *servers)
{
	int count = servers->type == TOML_ARRAY ? servers->u.arr.size : 0;

	for (int i = 0; i < count; i++) {
		toml_datum_t server = servers->u.arr.elem[i];
		toml_datum_t name = toml_get(server, "name");
		toml_datum_t transport = toml_get(server, "transport");
		toml_datum_t target = toml_get(server,
			transport.type == TOML_STRING &&
			strcmp(transport.u.s, "http") == 0 ? "url" : "command");

		if (name.type != TOML_STRING)
			continue;
		if (strcmp(options->action, "list") == 0) {
			printf("%s\t%s\t%s\n", name.u.s,
				transport.type == TOML_STRING ? transport.u.s : "stdio",
				target.type == TOML_STRING ? target.u.s : "");
			continue;
		}
		if (strcmp(options->name, name.u.s) != 0)
			continue;
		if (strcmp(options->action, "get") == 0) {
			printf("Name: %s\nTransport: %s\nTarget: %s\nConfig: %s\n",
				name.u.s,
				transport.type == TOML_STRING ? transport.u.s : "stdio",
				target.type == TOML_STRING ? target.u.s : "", path);
			toml_datum_t args = toml_get(server, "args");
			toml_datum_t env = toml_get(server, "env");
			toml_datum_t token = toml_get(server, "auth_token_env");

			if (args.type == TOML_ARRAY) {
				for (int j = 0; j < args.u.arr.size; j++) {
					if (args.u.arr.elem[j].type == TOML_STRING)
						printf("Arg: %s\n", args.u.arr.elem[j].u.s);
				}
			}
			if (env.type == TOML_TABLE) {
				for (int j = 0; j < env.u.tab.size; j++)
					printf("Env: %s=<redacted>\n", env.u.tab.key[j]);
			}
			if (token.type == TOML_STRING)
				printf("Token environment: %s\n", token.u.s);
			return 0;
		}
		struct config *config = calloc(1, sizeof(*config));
		struct mcp_server_config connection;
		char message[BUFSIZ];
		int rc;

		if (!config)
			MORPH_RETURN(-ENOMEM);
		config_set_defaults(config);
		rc = config_load(config, path);
		if (rc == 0 && i >= config->mcp.server_count)
			rc = -E2BIG;
		if (rc == 0)
			rc = runtime_mcp_server_config_from_config(
				&config->mcp.servers[i], &connection);
		free(config);
		if (rc != 0)
			return rc;
		http_init();
		rc = mcp_test_connection(&connection, message, sizeof(message));
		http_cleanup();
		printf("%s: %s\n", name.u.s, message);
		return rc;
	}
	if (strcmp(options->action, "list") == 0)
		return 0;
	MORPH_RETURN(-ENOENT);
}

static int mcp_apply(const struct mcp_options *options, int argc, char **argv,
		     const char *path)
{
	char *text = file_read_all(path, NULL);
	toml_result_t parsed = {0};
	toml_datum_t servers = {0};
	const toml_datum_t *selected = NULL;
	morph_buf_t output = {0};
	struct config_validation_error error = {0};
	int add = strcmp(options->action, "add") == 0;
	int remove = strcmp(options->action, "remove") == 0;
	int rc = 0;

	if (!text && errno == ENOENT)
		text = strdup("");
	if (!text)
		MORPH_RETURN(-EIO);
	if (strlen(text) > 1024 * 1024) {
		MORPH_SET_ERR(rc, -EFBIG);
		goto out;
	}
	parsed = toml_parse(text, (int)strlen(text));
	if (!parsed.ok) {
		fprintf(stderr, "morph: invalid config: %s\n", parsed.errmsg);
		MORPH_SET_ERR(rc, MORPH_ERR_PARSE);
		goto out;
	}
	servers = toml_get(toml_get(parsed.toptab, "mcp"), "servers");
	if (!add && !remove) {
		rc = mcp_read_command(options, path, &servers);
		goto out;
	}
	if (servers.type == TOML_ARRAY) {
		if (servers.flag & TOML_FLAG_INLINED) {
			fputs("morph: convert inline MCP servers to "
			      "[[mcp.servers]] before editing\n",
			      stderr);
			MORPH_SET_ERR(rc, -ENOTSUP);
			goto out;
		}
		for (int i = 0; i < servers.u.arr.size; i++) {
			toml_datum_t name = toml_get(servers.u.arr.elem[i], "name");

			if (name.type == TOML_STRING && strcmp(name.u.s, options->name) == 0) {
				if (selected) {
					MORPH_SET_ERR(rc, -EEXIST);
					goto out;
				}
				selected = &servers.u.arr.elem[i];
			}
		}
	}
	if ((add && selected && !options->replace) ||
	    (add && !selected && servers.type == TOML_ARRAY &&
	     servers.u.arr.size >= MCP_SERVER_MAX)) {
		MORPH_SET_ERR(rc, -EEXIST);
		goto out;
	}
	if (remove && !selected) {
		MORPH_SET_ERR(rc, -ENOENT);
		goto out;
	}
	rc = morph_buf_init(&output, 0);
	if (rc != 0)
		goto out;
	if (selected)
		rc = mcp_without_server(&parsed, selected, text, &output);
	else
		rc = morph_buf_puts(&output, text);
	if (rc == 0 && add)
		rc = mcp_build(options, argc, argv, &output);
	if (rc == 0)
		rc = config_validate_text(morph_buf_cstr(&output), &error);
	if (rc != 0 && error.message[0])
		fprintf(stderr, "morph: invalid config: %s\n", error.message);
	if (rc == 0)
		rc = cli_manage_atomic_write(path, morph_buf_cstr(&output), output.len);
	if (rc == 0)
		printf("%s MCP server '%s' in %s (user scope).\n",
			remove ? "Removed" : "Saved", options->name, path);
out:
	morph_buf_cleanup(&output);
	toml_free(parsed);
	free(text);
	return rc;
}

int cli_manage_mcp(int argc, char **argv, const char *config_path)
{
	struct mcp_options options;
	char *path;
	char *lock_path;
	int fd = -1;
	int rc = mcp_parse(argc, argv, config_path, &options);

	if (rc != 0) {
		mcp_usage();
		return 2;
	}
	if (strcmp(options.action, "--help") == 0 || strcmp(options.action, "-h") == 0) {
		mcp_usage();
		return 0;
	}
	if (strcmp(options.action, "add") != 0 && strcmp(options.action, "list") != 0 &&
	    strcmp(options.action, "get") != 0 && strcmp(options.action, "remove") != 0 &&
	    strcmp(options.action, "test") != 0) {
		mcp_usage();
		return 2;
	}
	path = file_expand_path(options.config ? options.config : "~/.morph/config.toml");
	if (!path)
		return cli_manage_error(-ENOMEM, "configuration path");
	lock_path = file_path_append_alloc(path, ".lock");
	if (!lock_path) {
		free(path);
		return cli_manage_error(-ENOMEM, "configuration lock");
	}
	if (strcmp(options.action, "add") == 0 || strcmp(options.action, "remove") == 0) {
		struct stat st;
		char *parent = strdup(path);
		char *slash = parent ? strrchr(parent, '/') : NULL;

		if (!parent)
			rc = -ENOMEM;
		else if (slash) {
			*slash = '\0';
			rc = file_ensure_dir(*parent ? parent : "/");
		}
		free(parent);
		if (rc == 0 && lstat(path, &st) == 0 && !S_ISREG(st.st_mode))
			rc = -ELOOP;
		if (rc == 0) {
			fd = cli_manage_lock(lock_path);
			if (fd < 0)
				rc = fd;
		}
	}
	if (rc == 0)
		rc = mcp_apply(&options, argc, argv, path);
	if (fd >= 0)
		close(fd);
	free(lock_path);
	free(path);
	return rc == 0 ? 0 : cli_manage_error(rc, "MCP command failed");
}
