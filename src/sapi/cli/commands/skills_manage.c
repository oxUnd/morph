#include "sapi/cli/commands/skills_manage.h"
#include <limits.h>
#include "nb/cleanup.h"
#include "cJSON.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/services/management.h"
#include "sapi/cli/services/skill_store.h"
#include "runtime/resources.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/str.h"
#include "nb/toml.h"
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct cli_skills_manage_skills_options {
	const char *action;
	const char *source;
	const char *path;
	const char *ref;
	const char *config;
	nb_vec name_storage;
	const char **names;
	int name_count;
	int global;
	int list;
	int all;
	int yes;
	int replace;
};

struct cli_skills_manage_skills_source {
	char source[PATH_MAX];
	char temporary[PATH_MAX];
	char root[PATH_MAX];
	char commit[128];
	int git;
};

static void cli_skills_manage_skills_usage(void)
{
	fputs("Usage: morph skills <add|list|remove|update> [options]\n"
	      "  add SOURCE [--skill NAME ...] [--path DIR] [--ref REF]\n"
	      "  add SOURCE --list          List available skills without installing\n"
	      "  list                       List discovered skills and origins\n"
	      "  remove NAME ...            Remove managed, unmodified skills\n"
	      "  update [NAME ...]          Update managed skills (all if omitted)\n"
	      "Sources: OWNER/REPO, Git HTTPS/SSH/file URL, or local directory\n"
	      "Options:\n"
	      "  --skill NAME               Select by SKILL.md name; repeatable\n"
	      "  --all                      Select every skill in the source\n"
	      "  -g, --global               User scope (default: current project)\n"
	      "  -y, --yes                  Noninteractive; never implies --all\n"
	      "  --replace                  Replace an unmodified managed installation\n"
	      "  -c, --config PATH          Read user skill.dir from this config\n"
	      "Alias: morph skill\n",
	      stdout);
}

enum cli_skills_manage_skills_option_id {
	CLI_SKILLS_MANAGE_SKILLS_OPTION_SKILL = 256,
	CLI_SKILLS_MANAGE_SKILLS_OPTION_PATH,
	CLI_SKILLS_MANAGE_SKILLS_OPTION_REF,
	CLI_SKILLS_MANAGE_SKILLS_OPTION_LIST,
	CLI_SKILLS_MANAGE_SKILLS_OPTION_ALL,
	CLI_SKILLS_MANAGE_SKILLS_OPTION_REPLACE,
};

static const struct option cli_skills_manage_long_options[] = {
	{"skill", required_argument, NULL, CLI_SKILLS_MANAGE_SKILLS_OPTION_SKILL},
	{"path", required_argument, NULL, CLI_SKILLS_MANAGE_SKILLS_OPTION_PATH},
	{"ref", required_argument, NULL, CLI_SKILLS_MANAGE_SKILLS_OPTION_REF},
	{"list", no_argument, NULL, CLI_SKILLS_MANAGE_SKILLS_OPTION_LIST},
	{"all", no_argument, NULL, CLI_SKILLS_MANAGE_SKILLS_OPTION_ALL},
	{"replace", no_argument, NULL, CLI_SKILLS_MANAGE_SKILLS_OPTION_REPLACE},
	{"global", no_argument, NULL, 'g'},
	{"yes", no_argument, NULL, 'y'},
	{"config", required_argument, NULL, 'c'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0},
};

static int cli_skills_manage_skills_parse(int argc, char **argv, const char *config,
					  struct cli_skills_manage_skills_options *options)
{
	int option;

	int rc = (nb_vec_init(&options->name_storage, sizeof(*options->names)),
		  nb_vec_reserve(&options->name_storage, 16) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	options->action = argc ? argv[0] : "--help";
	options->config = config;
	if (strcmp(options->action, "--help") == 0 || strcmp(options->action, "-h") == 0) {
		options->action = "--help";
		return 0;
	}
	cli_management_reset_getopt();
	while ((option = getopt_long(argc, argv, "c:gyh", cli_skills_manage_long_options, NULL)) !=
	       -1) {
		switch (option) {
		case 'c':
			options->config = optarg;
			break;
		case 'g':
			options->global = 1;
			break;
		case 'y':
			options->yes = 1;
			break;
		case CLI_SKILLS_MANAGE_SKILLS_OPTION_PATH:
			options->path = optarg;
			break;
		case CLI_SKILLS_MANAGE_SKILLS_OPTION_REF:
			options->ref = optarg;
			break;
		case CLI_SKILLS_MANAGE_SKILLS_OPTION_SKILL:
		{
			const char **slot = nb_vec_push(&options->name_storage);
			if (!slot)
				return NB_ERR_NOMEM;
			*slot = optarg;
			options->names = (const char **)((void *)options->name_storage.data);
			options->name_count++;
			break;
		}
		case CLI_SKILLS_MANAGE_SKILLS_OPTION_LIST:
			options->list = 1;
			break;
		case CLI_SKILLS_MANAGE_SKILLS_OPTION_ALL:
			options->all = 1;
			break;
		case CLI_SKILLS_MANAGE_SKILLS_OPTION_REPLACE:
			options->replace = 1;
			break;
		case 'h':
			options->action = "--help";
			return 0;
		default:
			return NB_ERR_INVAL;
		}
	}
	if (strcmp(options->action, "add") == 0 && optind < argc)
		options->source = argv[optind++];
	while (optind < argc) {
		const char **slot = nb_vec_push(&options->name_storage);
		if (!slot)
			return NB_ERR_NOMEM;
		*slot = argv[optind++];
		options->names = (const char **)((void *)options->name_storage.data);
		options->name_count++;
	}
	if (strcmp(options->action, "add") == 0) {
		if (!options->source || (options->all && options->name_count) ||
		    (options->list && (options->all || options->name_count || options->replace)))
			return NB_ERR_INVAL;
	} else if (options->path || options->ref || options->list || options->all ||
		   options->replace ||
		   (strcmp(options->action, "list") == 0 && options->name_count)) {
		return NB_ERR_INVAL;
	}
	if (strcmp(options->action, "remove") == 0 && !options->name_count)
		return NB_ERR_INVAL;
	for (int i = 0; i < options->name_count; i++) {
		if (!cli_management_name_valid(options->names[i]) ||
		    strlen(options->names[i]) >= SKILL_NAME_MAX)
			return NB_ERR_INVAL;
	}
	if ((options->ref && (!*options->ref || options->ref[0] == '-')) ||
	    (options->path && !*options->path))
		return NB_ERR_INVAL;
	return 0;
}

static int cli_skills_manage_skills_user_dir(const char *config_path, char **out)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};

	nb_err parse_error = {0};
	char *path =
		(nb_path_expand(&file_value_1, config_path ? config_path : "~/.morph/config.toml",
				NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_1))
			 : NULL);
	char *text;
	nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) = NULL;
	const nb_toml_value *dir;

	if (!path)
		return NB_ERR_NOMEM;
	text = (nb_file_read_all(path, &file_value_2, 0, NULL) == NB_OK &&
				(file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) + 1)) !=
					NULL
			? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
				  nb_buf_len(&file_value_2) + 1),
			   nb_charp_steal(&file_copy_2))
			: NULL);
	free(path);
	if (!text) {
		if (config_path || errno != ENOENT)
			return NB_ERR_IO;
		*out = (nb_path_expand(&file_value_3, "~/.morph/skills", NULL) == NB_OK
				? strdup(nb_buf_cstr(&file_value_3))
				: NULL);
		return *out ? 0 : NB_ERR_NOMEM;
	}
	if (strlen(text) > 1024 * 1024) {
		free(text);
		return NB_ERR_TOO_BIG;
	}
	parsed = nb_toml_parse(nb_str_from_cstr(text), &parse_error);
	free(text);
	if (!parsed) {
		fprintf(stderr, "morph: invalid config: %s\n", parse_error.msg);
		nb_toml_cleanup(&parsed);
		return NB_ERR_PARSE;
	}
	dir = nb_toml_table_get_cstr(nb_toml_table_get_cstr(nb_toml_root(parsed), "skill"), "dir");
	*out = (nb_path_expand(&file_value_4,
			       nb_toml_type_of(dir) == NB_TOML_STRING && nb_toml_string(dir).ptr[0]
				       ? nb_toml_string(dir).ptr
				       : "~/.morph/skills",
			       NULL) == NB_OK
			? strdup(nb_buf_cstr(&file_value_4))
			: NULL);
	nb_toml_cleanup(&parsed);
	return *out ? 0 : NB_ERR_NOMEM;
}

static int cli_skills_manage_skills_git(const char *root, const char *verb, const char *arg,
					nb_buf *output)
{
	char *argv[] = {"git",
			"-c",
			"core.hooksPath=/dev/null",
			"-c",
			"submodule.recurse=false",
			(char *)verb,
			(char *)arg,
			NULL};

	return cli_management_run(root, argv, output);
}

static int cli_skills_manage_skills_repo_name_valid(const char *name)
{
	return nb_str_is_alnum(nb_str_from_cstr(name), "-_.") && name[0] != '.' && name[0] != '-';
}

static int cli_skills_manage_skills_prepare(const char *source, const char *ref, int force_git,
					    struct cli_skills_manage_skills_source *prepared)
{
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_5 = NB_OK;
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_8 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_8 = NB_OK;

	char resolved[PATH_MAX];
	char git_dir[PATH_MAX];
	char *cache;
	struct stat st;
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	int local = realpath(source, resolved) != NULL;
	int rc;

	memset(prepared, 0, sizeof(*prepared));
	if (local) {
		if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
			return NB_ERR_NOT_DIR;
		rc = cli_management_copy_arg(prepared->source, sizeof(prepared->source), resolved);
		if (rc != 0)
			return rc;
		rc = ((file_status_5 = nb_path_concat(&file_value_5, resolved, ".git")) != NB_OK
			      ? file_status_5
		      : (file_status_5 = nb_str_copy_to(nb_buf_str(&file_value_5), git_dir,
							sizeof(git_dir))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_5);
		if (rc != 0)
			return rc;
		prepared->git = force_git || nb_file_exists(git_dir);
		if (!prepared->git) {
			if (ref)
				return NB_ERR_INVAL;
			return cli_management_copy_arg(prepared->root, sizeof(prepared->root),
						       resolved);
		}
	} else {
		prepared->git = 1;
		if (strstr(source, "://") || strchr(source, '@')) {
			rc = cli_management_copy_arg(prepared->source, sizeof(prepared->source),
						     source);
		} else {
			const char *slash = strchr(source, '/');
			char owner[PATH_MAX];

			if (!slash || slash == source || strchr(slash + 1, '/') ||
			    (size_t)(slash - source) >= sizeof(owner))
				return NB_ERR_NOT_FOUND;
			memcpy(owner, source, (size_t)(slash - source));
			owner[slash - source] = '\0';
			if (!cli_management_name_valid(owner) ||
			    !cli_skills_manage_skills_repo_name_valid(slash + 1))
				return NB_ERR_INVAL;
			size_t length_source = strlen(source);
			const char *suffix =
				length_source > 4 && strcmp(source + length_source - 4, ".git") == 0
					? ""
					: ".git";
			int length = snprintf(prepared->source, sizeof(prepared->source),
					      "https://github.com/%s%s", source, suffix);

			rc = length < 0 || length >= (int)sizeof(prepared->source)
				     ? NB_ERR_NAME_TOO_LONG
				     : 0;
		}
		if (rc != 0)
			return rc;
	}
	if (prepared->source[0] == '-')
		return NB_ERR_INVAL;
	cache = (nb_path_expand(&file_value_6, "~/.morph/cache/skills", NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_6))
			 : NULL);
	if (!cache)
		return NB_ERR_NOMEM;
	rc = nb_file_mkdir(cache, true, NULL);
	if (rc == NB_OK)
		rc = nb_file_temp_dir(&file_value_7, cache, "checkout-", NULL);
	free(cache);
	if (rc == NB_OK)
		rc = nb_str_copy_to(nb_buf_str(&file_value_7), prepared->temporary,
				    sizeof(prepared->temporary));
	if (rc != NB_OK) {
		if (!nb_buf_empty(&file_value_7))
			(void)nb_file_remove_all(nb_buf_cstr(&file_value_7), 0, NULL);
		prepared->temporary[0] = '\0';
		return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
	}

	rc = ((file_status_8 = nb_path_concat(&file_value_8, prepared->temporary, "repo")) != NB_OK
		      ? file_status_8
	      : (file_status_8 = nb_str_copy_to(nb_buf_str(&file_value_8), prepared->root,
						sizeof(prepared->root))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_8);
	if (rc != 0)
		return rc;
	char *clone[] = {
		"git",		 "-c", "core.hooksPath=/dev/null", "clone",	   "--quiet",
		"--no-checkout", "--", prepared->source,	   prepared->root, NULL};

	rc = cli_management_run(NULL, clone, NULL);
	if (rc == 0 && ref) {
		char *fetch[] = {"git",	   "-c",	"core.hooksPath=/dev/null",
				 "fetch",  "--quiet",	"--",
				 "origin", (char *)ref, NULL};

		rc = cli_management_run(prepared->root, fetch, NULL);
	}
	if (rc == 0) {
		char *checkout[] = {"git",
				    "-c",
				    "core.hooksPath=/dev/null",
				    "-c",
				    "submodule.recurse=false",
				    "checkout",
				    "--quiet",
				    "--detach",
				    ref ? "FETCH_HEAD" : "HEAD",
				    "--",
				    NULL};

		rc = cli_management_run(prepared->root, checkout, NULL);
	}
	if (rc == 0)
		rc = (nb_buf_init(&output), nb_buf_reserve(&output, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = cli_skills_manage_skills_git(prepared->root, "rev-parse", "HEAD", &output);
	if (rc == 0) {
		char canonical[PATH_MAX];

		if (!realpath(prepared->root, canonical))
			rc = nb_status_from_errno(errno, NULL);
		else
			rc = cli_management_copy_arg(prepared->root, sizeof(prepared->root),
						     canonical);
	}
	if (rc == 0) {
		while (output.len &&
		       (output.ptr[output.len - 1] == '\n' || output.ptr[output.len - 1] == '\r'))
			output.ptr[--output.len] = '\0';
		rc = cli_management_copy_arg(prepared->commit, sizeof(prepared->commit),
					     output.ptr);
	}
	nb_buf_cleanup(&output);
	return rc;
}

static int
cli_skills_manage_skills_scan_source(struct nb_log *logger,
				     const struct cli_skills_manage_skills_source *source,
				     const char *subdir, nb_vec *candidates)
{
	nb_buf file_value_9 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_9 = NB_OK;

	char selected[PATH_MAX];
	char resolved[PATH_MAX];
	int rc = ((file_status_9 = nb_path_concat(&file_value_9, source->root,
						  subdir ? subdir : ".")) != NB_OK
			  ? file_status_9
		  : (file_status_9 = nb_str_copy_to(nb_buf_str(&file_value_9), selected,
						    sizeof(selected))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : file_status_9);

	if (rc != 0)
		return rc;
	if (subdir && nb_path_is_absolute(subdir))
		return NB_ERR_INVAL;
	if (!realpath(selected, resolved))
		return nb_status_from_errno(errno, NULL);
	if (!nb_path_is_within(resolved, source->root, true))
		return NB_ERR_PERMISSION;
	rc = cli_skill_store_scan(logger, resolved, candidates);
	if (rc != 0)
		return rc;
	if (!candidates->len)
		return NB_ERR_NOT_FOUND;
	for (size_t i = 0; i < candidates->len; i++) {
		const struct cli_skill_candidate *candidate = nb_vec_at(candidates, i);

		for (size_t j = 0; j < i; j++) {
			const struct cli_skill_candidate *previous = nb_vec_at(candidates, j);

			if (strcmp(candidate->fm.name, previous->fm.name) == 0) {
				fprintf(stderr,
					"morph: duplicate skill name '%s'; "
					"narrow the source with --path\n",
					candidate->fm.name);
				return NB_ERR_EXISTS;
			}
		}
	}
	return 0;
}

static void cli_skills_manage_skills_candidates_print(const nb_vec *candidates)
{
	for (size_t i = 0; i < candidates->len; i++) {
		const struct cli_skill_candidate *candidate = nb_vec_at(candidates, i);

		printf("%zu. %s\t%s\n", i + 1, candidate->fm.name, candidate->fm.description);
	}
}

static int cli_skills_manage_skills_select(const struct cli_skills_manage_skills_options *options,
					   const nb_vec *candidates, int *selected)
{
	if (options->all) {
		for (size_t i = 0; i < candidates->len; i++)
			selected[i] = 1;
		return 0;
	}
	if (options->name_count) {
		for (int i = 0; i < options->name_count; i++) {
			int found = 0;

			for (size_t j = 0; j < candidates->len; j++) {
				const struct cli_skill_candidate *candidate =
					nb_vec_at(candidates, j);

				if (strcmp(candidate->fm.name, options->names[i]) == 0) {
					selected[j] = 1;
					found = 1;
				}
			}
			if (!found) {
				fprintf(stderr, "morph: skill not found in source: %s\n",
					options->names[i]);
				return NB_ERR_NOT_FOUND;
			}
		}
		return 0;
	}
	if (candidates->len == 1) {
		selected[0] = 1;
		return 0;
	}
	cli_skills_manage_skills_candidates_print(candidates);
	if (options->yes || !isatty(STDIN_FILENO)) {
		fputs("morph: multiple skills; select with --skill NAME or --all\n", stderr);
		return NB_ERR_INVAL;
	}
	fputs("Select skill numbers (comma/space separated), or 'all': ", stderr);
	fflush(stderr);
	char *input NB_CLEANUP(nb_charp_cleanup) = NULL;
	size_t capacity = 0;
	char *save = NULL;
	char *token;
	int count = 0;

	if (getline(&input, &capacity, stdin) < 0)
		return NB_ERR_CANCELLED;
	for (token = strtok_r(input, ", \t\r\n", &save); token;
	     token = strtok_r(NULL, ", \t\r\n", &save)) {
		char *end;
		unsigned long index;

		if (strcmp(token, "all") == 0) {
			for (size_t i = 0; i < candidates->len; i++)
				selected[i] = 1;
			count++;
			continue;
		}
		errno = 0;
		index = strtoul(token, &end, 10);
		if (errno || *end || index == 0 || index > candidates->len)
			return NB_ERR_INVAL;
		selected[index - 1] = 1;
		count++;
	}
	return count ? 0 : NB_ERR_CANCELLED;
}

static cJSON *
cli_skills_manage_skills_metadata_create(const struct cli_skills_manage_skills_source *source,
					 const struct cli_skill_candidate *candidate,
					 const char *ref)
{
	cJSON *metadata = cJSON_CreateObject();
	const char *path = candidate->path + strlen(source->root);

	while (*path == '/')
		path++;
	if (!metadata)
		return NULL;
	if (!cJSON_AddNumberToObject(metadata, "version", 1) ||
	    !cJSON_AddStringToObject(metadata, "name", candidate->fm.name) ||
	    !cJSON_AddStringToObject(metadata, "kind", source->git ? "git" : "local") ||
	    !cJSON_AddStringToObject(metadata, "source", source->source) ||
	    !cJSON_AddStringToObject(metadata, "path", *path ? path : ".") ||
	    !cJSON_AddStringToObject(metadata, "ref", ref ? ref : "") ||
	    !cJSON_AddStringToObject(metadata, "commit", source->commit)) {
		cJSON_Delete(metadata);
		return NULL;
	}
	return metadata;
}

static int cli_skills_manage_skills_add(struct nb_log *logger,
					const struct cli_skills_manage_skills_options *options,
					const char *root)
{
	nb_buf file_value_10 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_10 = NB_OK;

	struct cli_skills_manage_skills_source source = {0};
	nb_vec candidates = {0};
	nb_vec selection NB_CLEANUP(nb_vec_cleanup) = {0};
	int *selected = NULL;
	int rc = (nb_vec_init(&candidates, sizeof(struct cli_skill_candidate)),
		  nb_vec_reserve(&candidates, 16) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	rc = cli_skills_manage_skills_prepare(options->source, options->ref, 0, &source);
	if (rc == 0)
		rc = cli_skills_manage_skills_scan_source(logger, &source, options->path,
							  &candidates);
	if (rc == 0 && options->list)
		cli_skills_manage_skills_candidates_print(&candidates);
	if (rc == 0 && !options->list) {
		rc = (nb_vec_init(&selection, sizeof(*selected)),
		      nb_vec_reserve(&selection, candidates.len) ? 0 : NB_ERR_NOMEM);
		if (rc == 0 && candidates.len) {
			selected = nb_vec_push_n(&selection, candidates.len);
			if (!selected)
				rc = NB_ERR_NOMEM;
			else
				memset(selected, 0, candidates.len * sizeof(*selected));
		}
	}
	if (rc == 0 && !options->list)
		rc = cli_skills_manage_skills_select(options, &candidates, selected);
	/* Check every destination before installing any selected skill. */
	for (size_t i = 0; rc == 0 && !options->list && i < candidates.len; i++) {
		const struct cli_skill_candidate *candidate = nb_vec_at(&candidates, i);
		char target[PATH_MAX];
		struct stat st;

		if (!selected[i])
			continue;
		rc = ((file_status_10 = nb_path_concat(&file_value_10, root, candidate->fm.name)) !=
				      NB_OK
			      ? file_status_10
		      : (file_status_10 = nb_str_copy_to(nb_buf_str(&file_value_10), target,
							 sizeof(target))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_10);
		if (rc == 0 && lstat(target, &st) == 0) {
			if (!options->replace)
				rc = NB_ERR_EXISTS;
			else if (!S_ISDIR(st.st_mode))
				rc = NB_ERR_LOOP;
			else {
				cJSON *metadata = cli_skill_store_metadata_copy(target);

				rc = cli_skill_store_unmodified(target, metadata);
				cJSON_Delete(metadata);
			}
		}
	}
	for (size_t i = 0; rc == 0 && !options->list && i < candidates.len; i++) {
		const struct cli_skill_candidate *candidate = nb_vec_at(&candidates, i);
		cJSON *metadata;

		if (!selected[i])
			continue;
		metadata =
			cli_skills_manage_skills_metadata_create(&source, candidate, options->ref);
		rc = metadata ? cli_skill_store_install(root, candidate, metadata, options->replace)
			      : NB_ERR_NOMEM;
		cJSON_Delete(metadata);
		if (rc == 0)
			printf("Installed '%s' in %s (%s scope).\n", candidate->fm.name, root,
			       options->global ? "user" : "project");
	}
	if (source.temporary[0]) {
		int cleanup_rc = cli_skill_store_remove(source.temporary);

		if (rc == 0)
			rc = cleanup_rc;
	}
	nb_vec_cleanup(&candidates);
	return rc;
}

static int cli_skills_manage_skills_list(const struct cli_skills_manage_skills_options *options,
					 const char *user_dir)
{
	struct skill_registry *registry = calloc(1, sizeof(*registry));
	int rc;

	if (!registry)
		return NB_ERR_NOMEM;
	skill_registry_init(NULL, registry);
	rc = skill_discover_configured(registry, options->global ? NULL : ".", user_dir);
	if (rc >= 0 && !options->global) {
		char *builtin = morph_data_find_alloc("skills");

		if (builtin) {
			rc = skill_discover(registry, builtin);
			free(builtin);
		}
	}
	if (rc >= 0) {
		for (int i = 0; i < registry->count; i++) {
			const struct skill_entry *entry = skill_entry_borrow_at(registry,
			(size_t)i);
			cJSON *metadata = cli_skill_store_metadata_copy(entry->skill_dir);
			cJSON *source = cJSON_GetObjectItemCaseSensitive(metadata, "source");
			cJSON *commit = cJSON_GetObjectItemCaseSensitive(metadata, "commit");

			printf("%s\t%s\t%s", entry->fm.name, entry->skill_dir,
			       cJSON_IsString(source) ? source->valuestring : "unmanaged");
			if (cJSON_IsString(commit) && commit->valuestring[0])
				printf("\t%.12s", commit->valuestring);
			putchar('\n');
			cJSON_Delete(metadata);
		}
		rc = 0;
	}
	skill_registry_cleanup(registry);
	free(registry);
	return rc;
}

static const char *cli_skills_manage_metadata_string(const cJSON *metadata, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(metadata, key);

	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int cli_skills_manage_skills_update_one(struct nb_log *logger, const char *root,
					       const char *name)
{
	nb_buf file_value_11 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_11 = NB_OK;

	char target[PATH_MAX];
	struct cli_skills_manage_skills_source source = {0};
	nb_vec candidates NB_CLEANUP(nb_vec_cleanup) = {0};
	cJSON *metadata NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int rc = ((file_status_11 = nb_path_concat(&file_value_11, root, name)) != NB_OK
			  ? file_status_11
		  : (file_status_11 = nb_str_copy_to(nb_buf_str(&file_value_11), target,
						     sizeof(target))) == NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : file_status_11);

	if (rc != 0)
		return rc;
	metadata = cli_skill_store_metadata_copy(target);
	rc = cli_skill_store_unmodified(target, metadata);
	if (rc != 0)
		return rc;
	const char *origin = cli_skills_manage_metadata_string(metadata, "source");
	const char *kind = cli_skills_manage_metadata_string(metadata, "kind");
	const char *path = cli_skills_manage_metadata_string(metadata, "path");
	const char *ref = cli_skills_manage_metadata_string(metadata, "ref");

	if (!origin || !kind || !path || !ref) {
		rc = (NB_ERR_FORMAT);
		return rc;
	}
	rc = (nb_vec_init(&candidates, sizeof(struct cli_skill_candidate)),
	      nb_vec_reserve(&candidates, 1) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = cli_skills_manage_skills_prepare(origin, *ref ? ref : NULL,
						      strcmp(kind, "git") == 0, &source);
	if (rc == 0)
		rc = cli_skills_manage_skills_scan_source(logger, &source, path, &candidates);
	if (rc == 0) {
		const struct cli_skill_candidate *candidate = NULL;

		for (size_t i = 0; i < candidates.len; i++) {
			const struct cli_skill_candidate *entry = nb_vec_at(&candidates, i);

			if (strcmp(entry->fm.name, name) == 0)
				candidate = entry;
		}
		if (!candidate)
			rc = (NB_ERR_NOT_FOUND);
		else {
			cJSON *updated = cli_skills_manage_skills_metadata_create(
				&source, candidate, *ref ? ref : NULL);

			rc = updated ? cli_skill_store_install(root, candidate, updated, 1)
				     : NB_ERR_NOMEM;
			cJSON_Delete(updated);
			if (rc == 0)
				printf("Updated '%s' in %s.\n", name, root);
		}
	}
	if (source.temporary[0]) {
		int cleanup_rc = cli_skill_store_remove(source.temporary);

		if (rc == 0)
			rc = cleanup_rc;
	}
	return rc;
}

static int cli_skills_manage_skills_modify(struct nb_log *logger,
					   const struct cli_skills_manage_skills_options *options,
					   const char *root)
{
	int directory_status_1 = NB_OK;

	nb_buf file_value_12 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_12 = NB_OK;

	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int count = 0;
	int rc = 0;
	int result = 0;

	if (options->name_count) {
		count = options->name_count;
	} else {
		rc = ((directory_status_1 = nb_dir_scan(root,
							NB_DIR_SCAN_DIRS | NB_DIR_SCAN_HIDDEN |
								NB_DIR_SCAN_FOLLOW_LINKS,
							&names, NULL)) != NB_OK
			      ? directory_status_1
		      : names.names.len > INT_MAX ? NB_ERR_RANGE
						  : (count = (int)names.names.len, NB_OK));
		if (rc != 0)
			return rc;
	}
	for (int i = 0; i < count; i++) {
		const char *name = options->name_count ? options->names[i]
						       : nb_dir_listing_name(&names, (size_t)(i));
		char target[PATH_MAX];
		cJSON *metadata;
		struct stat st;

		if (!cli_management_name_valid(name))
			continue;
		rc = 0;
		rc = ((file_status_12 = nb_path_concat(&file_value_12, root, name)) != NB_OK
			      ? file_status_12
		      : (file_status_12 = nb_str_copy_to(nb_buf_str(&file_value_12), target,
							 sizeof(target))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_12);
		if (rc != 0)
			break;
		if (lstat(target, &st) != 0)
			rc = nb_status_from_errno(errno, NULL);
		else if (!S_ISDIR(st.st_mode))
			rc = NB_ERR_LOOP;
		if (rc != 0) {
			result = rc;
			continue;
		}
		metadata = cli_skill_store_metadata_copy(target);
		if (!metadata && !options->name_count)
			continue;
		if (strcmp(options->action, "remove") == 0) {
			rc = cli_skill_store_unmodified(target, metadata);
			if (rc == 0)
				rc = cli_skill_store_remove(target);
			if (rc == 0)
				printf("Removed '%s' from %s.\n", name, root);
		} else {
			rc = cli_skills_manage_skills_update_one(logger, root, name);
		}
		cJSON_Delete(metadata);
		if (rc != 0) {
			cli_management_error(rc, name);
			result = rc;
		}
	}
	nb_dir_listing_destroy(&names);
	return result ? result : rc;
}

static void cli_skills_manage_fd_cleanup(int *owner)
{
	if (*owner >= 0)
		close(*owner);
	*owner = -1;
}

static void cli_skills_manage_options_cleanup(struct cli_skills_manage_skills_options *options)
{
	nb_vec_cleanup(&options->name_storage);
}

int cli_skills_manage_run(struct nb_log *logger, int argc, char **argv, const char *config_path)
{
	nb_buf file_value_13 NB_CLEANUP(nb_buf_destroy) = {0};

	struct cli_skills_manage_skills_options options NB_CLEANUP(
		cli_skills_manage_options_cleanup) = {0};
	char *user_dir NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *root NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *lock NB_CLEANUP(nb_charp_cleanup) = NULL;
	char resolved[PATH_MAX];
	int fd NB_CLEANUP(cli_skills_manage_fd_cleanup) = -1;
	int rc = cli_skills_manage_skills_parse(argc, argv, config_path, &options);

	if (rc != 0) {
		cli_skills_manage_skills_usage();
		return 2;
	}
	if (strcmp(options.action, "--help") == 0 || strcmp(options.action, "-h") == 0) {
		cli_skills_manage_skills_usage();
		return 0;
	}
	if (strcmp(options.action, "add") != 0 && strcmp(options.action, "list") != 0 &&
	    strcmp(options.action, "remove") != 0 && strcmp(options.action, "update") != 0) {
		cli_skills_manage_skills_usage();
		return 2;
	}
	rc = cli_skills_manage_skills_user_dir(options.config, &user_dir);
	if (rc != 0)
		return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
	if (strcmp(options.action, "list") == 0) {
		rc = cli_skills_manage_skills_list(&options, user_dir);
		return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
	}
	root = options.global ? strdup(user_dir) : strdup(".agents/skills");
	if (!root) {
		rc = (NB_ERR_NOMEM);
		return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
	}
	if (!options.list) {
		rc = nb_file_mkdir(root, true, NULL);
		if (rc != 0)
			return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
		if (!realpath(root, resolved)) {
			rc = (nb_status_from_errno(errno, NULL));
			return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
		}
		free(root);
		root = strdup(resolved);
		if (!root) {
			rc = (NB_ERR_NOMEM);
			return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
		}
		lock = (nb_path_concat(&file_value_13, root, ".morph-skills.lock") == NB_OK
				? strdup(nb_buf_cstr(&file_value_13))
				: NULL);
		if (!lock) {
			rc = (NB_ERR_NOMEM);
			return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
		}
		fd = cli_management_lock(lock);
		if (fd < 0) {
			rc = (fd);
			return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
		}
	}
	if (strcmp(options.action, "add") == 0)
		rc = cli_skills_manage_skills_add(logger, &options, root);
	else
		rc = cli_skills_manage_skills_modify(logger, &options, root);
	return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
}
