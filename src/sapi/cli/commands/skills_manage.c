#include "sapi/cli/commands/skills_manage.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/services/management.h"
#include "sapi/cli/services/skill_store.h"
#include "util/data.h"
#include "util/error.h"
#include "util/file.h"
#include "util/name.h"
#include "tomlc17.h"
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
	morph_array_t name_storage;
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

	int rc = morph_array_init(&options->name_storage, MORPH_ARRAY_INIT_CAP,
				   sizeof(*options->names));
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
			const char **slot = morph_array_push_borrow(&options->name_storage);
			if (!slot)
				return -ENOMEM;
			*slot = optarg;
			options->names = options->name_storage.elts;
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
			return -EINVAL;
		}
	}
	if (strcmp(options->action, "add") == 0 && optind < argc)
		options->source = argv[optind++];
	while (optind < argc) {
		const char **slot = morph_array_push_borrow(&options->name_storage);
		if (!slot)
			return -ENOMEM;
		*slot = argv[optind++];
		options->names = options->name_storage.elts;
		options->name_count++;
	}
	if (strcmp(options->action, "add") == 0) {
		if (!options->source || (options->all && options->name_count) ||
		    (options->list && (options->all || options->name_count || options->replace)))
			return -EINVAL;
	} else if (options->path || options->ref || options->list || options->all ||
		   options->replace ||
		   (strcmp(options->action, "list") == 0 && options->name_count)) {
		return -EINVAL;
	}
	if (strcmp(options->action, "remove") == 0 && !options->name_count)
		return -EINVAL;
	for (int i = 0; i < options->name_count; i++) {
		if (!cli_management_name_valid(options->names[i]) ||
		    strlen(options->names[i]) >= SKILL_NAME_MAX)
			return -EINVAL;
	}
	if ((options->ref && (!*options->ref || options->ref[0] == '-')) ||
	    (options->path && !*options->path))
		return -EINVAL;
	return 0;
}

static int cli_skills_manage_skills_user_dir(const char *config_path, char **out)
{
	char *path = file_expand_path_copy(config_path ? config_path : "~/.morph/config.toml");
	char *text;
	toml_result_t parsed;
	toml_datum_t dir;

	if (!path)
		return -ENOMEM;
	text = file_read_all_copy(path, NULL);
	free(path);
	if (!text) {
		if (config_path || errno != ENOENT)
			return -EIO;
		*out = file_expand_path_copy("~/.morph/skills");
		return *out ? 0 : -ENOMEM;
	}
	if (strlen(text) > 1024 * 1024) {
		free(text);
		return -EFBIG;
	}
	parsed = toml_parse(text, (int)strlen(text));
	free(text);
	if (!parsed.ok) {
		fprintf(stderr, "morph: invalid config: %s\n", parsed.errmsg);
		toml_free(parsed);
		return MORPH_ERR_PARSE;
	}
	dir = toml_get(toml_get(parsed.toptab, "skill"), "dir");
	*out = file_expand_path_copy(dir.type == TOML_STRING && dir.u.s[0] ? dir.u.s
									   : "~/.morph/skills");
	toml_free(parsed);
	return *out ? 0 : -ENOMEM;
}

static int cli_skills_manage_skills_git(const char *root, const char *verb, const char *arg,
					morph_buf_t *output)
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
	return morph_name_valid(name, "-_.") && name[0] != '.' && name[0] != '-';
}

static int cli_skills_manage_skills_prepare(const char *source, const char *ref, int force_git,
					    struct cli_skills_manage_skills_source *prepared)
{
	char resolved[PATH_MAX];
	char git_dir[PATH_MAX];
	char *cache;
	struct stat st;
	morph_buf_t output = {0};
	int local = realpath(source, resolved) != NULL;
	int rc;

	memset(prepared, 0, sizeof(*prepared));
	if (local) {
		if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
			return -ENOTDIR;
		rc = cli_management_copy_arg(prepared->source, sizeof(prepared->source), resolved);
		if (rc != 0)
			return rc;
		rc = file_path_join(git_dir, sizeof(git_dir), resolved, ".git");
		if (rc != 0)
			return rc;
		prepared->git = force_git || file_exists(git_dir);
		if (!prepared->git) {
			if (ref)
				return -EINVAL;
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
				return -ENOENT;
			memcpy(owner, source, (size_t)(slash - source));
			owner[slash - source] = '\0';
			if (!cli_management_name_valid(owner) ||
			    !cli_skills_manage_skills_repo_name_valid(slash + 1))
				return -EINVAL;
			size_t length_source = strlen(source);
			const char *suffix =
				length_source > 4 && strcmp(source + length_source - 4, ".git") == 0
					? ""
					: ".git";
			int length = snprintf(prepared->source, sizeof(prepared->source),
					      "https://github.com/%s%s", source, suffix);

			rc = length < 0 || length >= (int)sizeof(prepared->source) ? -ENAMETOOLONG
										   : 0;
		}
		if (rc != 0)
			return rc;
	}
	if (prepared->source[0] == '-')
		return -EINVAL;
	cache = file_expand_path_copy("~/.morph/cache/skills");
	if (!cache)
		return -ENOMEM;
	rc = file_ensure_dir(cache);
	if (rc == 0)
		rc = file_path_join(prepared->temporary, sizeof(prepared->temporary), cache,
				    "checkout-XXXXXX");
	free(cache);
	if (rc != 0)
		return rc;
	if (!mkdtemp(prepared->temporary)) {
		prepared->temporary[0] = '\0';
		return morph_error_from_errno(errno);
	}
	rc = file_path_join(prepared->root, sizeof(prepared->root), prepared->temporary, "repo");
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
		rc = morph_buf_init(&output, 128);
	if (rc == 0)
		rc = cli_skills_manage_skills_git(prepared->root, "rev-parse", "HEAD", &output);
	if (rc == 0) {
		char canonical[PATH_MAX];

		if (!realpath(prepared->root, canonical))
			rc = -errno;
		else
			rc = cli_management_copy_arg(prepared->root, sizeof(prepared->root),
						     canonical);
	}
	if (rc == 0) {
		while (output.len &&
		       (output.data[output.len - 1] == '\n' || output.data[output.len - 1] == '\r'))
			output.data[--output.len] = '\0';
		rc = cli_management_copy_arg(prepared->commit, sizeof(prepared->commit),
					     output.data);
	}
	morph_buf_cleanup(&output);
	return rc;
}

static int
cli_skills_manage_skills_scan_source(struct morph_log *logger,
				     const struct cli_skills_manage_skills_source *source,
				     const char *subdir, morph_array_t *candidates)
{
	char selected[PATH_MAX];
	char resolved[PATH_MAX];
	int rc = file_path_join(selected, sizeof(selected), source->root, subdir ? subdir : ".");

	if (rc != 0)
		return rc;
	if (subdir && file_path_is_absolute(subdir))
		return -EINVAL;
	if (!realpath(selected, resolved))
		return morph_error_from_errno(errno);
	if (!path_is_within(resolved, source->root))
		return -EACCES;
	rc = cli_skill_store_scan(logger, resolved, candidates);
	if (rc != 0)
		return rc;
	if (!candidates->nelts)
		return -ENOENT;
	for (size_t i = 0; i < candidates->nelts; i++) {
		const struct cli_skill_candidate *candidate = morph_array_borrow(candidates, i);

		for (size_t j = 0; j < i; j++) {
			const struct cli_skill_candidate *previous =
				morph_array_borrow(candidates, j);

			if (strcmp(candidate->fm.name, previous->fm.name) == 0) {
				fprintf(stderr,
					"morph: duplicate skill name '%s'; "
					"narrow the source with --path\n",
					candidate->fm.name);
				return -EEXIST;
			}
		}
	}
	return 0;
}

static void cli_skills_manage_skills_candidates_print(const morph_array_t *candidates)
{
	for (size_t i = 0; i < candidates->nelts; i++) {
		const struct cli_skill_candidate *candidate = morph_array_borrow(candidates, i);

		printf("%zu. %s\t%s\n", i + 1, candidate->fm.name, candidate->fm.description);
	}
}

static int cli_skills_manage_skills_select(const struct cli_skills_manage_skills_options *options,
					   const morph_array_t *candidates,
					   int *selected)
{
	if (options->all) {
		for (size_t i = 0; i < candidates->nelts; i++)
			selected[i] = 1;
		return 0;
	}
	if (options->name_count) {
		for (int i = 0; i < options->name_count; i++) {
			int found = 0;

			for (size_t j = 0; j < candidates->nelts; j++) {
				const struct cli_skill_candidate *candidate =
					morph_array_borrow(candidates, j);

				if (strcmp(candidate->fm.name, options->names[i]) == 0) {
					selected[j] = 1;
					found = 1;
				}
			}
			if (!found) {
				fprintf(stderr, "morph: skill not found in source: %s\n",
					options->names[i]);
				return -ENOENT;
			}
		}
		return 0;
	}
	if (candidates->nelts == 1) {
		selected[0] = 1;
		return 0;
	}
	cli_skills_manage_skills_candidates_print(candidates);
	if (options->yes || !isatty(STDIN_FILENO)) {
		fputs("morph: multiple skills; select with --skill NAME or --all\n", stderr);
		return -EINVAL;
	}
	fputs("Select skill numbers (comma/space separated), or 'all': ", stderr);
	fflush(stderr);
	char *input MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	size_t capacity = 0;
	char *save = NULL;
	char *token;
	int count = 0;

	if (getline(&input, &capacity, stdin) < 0)
		return -ECANCELED;
	for (token = strtok_r(input, ", \t\r\n", &save); token;
	     token = strtok_r(NULL, ", \t\r\n", &save)) {
		char *end;
		unsigned long index;

		if (strcmp(token, "all") == 0) {
			for (size_t i = 0; i < candidates->nelts; i++)
				selected[i] = 1;
			count++;
			continue;
		}
		errno = 0;
		index = strtoul(token, &end, 10);
		if (errno || *end || index == 0 || index > candidates->nelts)
			return -EINVAL;
		selected[index - 1] = 1;
		count++;
	}
	return count ? 0 : -ECANCELED;
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

static int cli_skills_manage_skills_add(struct morph_log *logger,
					const struct cli_skills_manage_skills_options *options,
					const char *root)
{
	struct cli_skills_manage_skills_source source = {0};
	morph_array_t candidates = {0};
	morph_array_t selection MORPH_CLEANUP(morph_array_cleanup) = {0};
	int *selected = NULL;
	int rc = morph_array_init(&candidates, 16, sizeof(struct cli_skill_candidate));

	if (rc != 0)
		return rc;
	rc = cli_skills_manage_skills_prepare(options->source, options->ref, 0, &source);
	if (rc == 0)
		rc = cli_skills_manage_skills_scan_source(logger, &source, options->path,
							  &candidates);
	if (rc == 0 && options->list)
		cli_skills_manage_skills_candidates_print(&candidates);
	if (rc == 0 && !options->list) {
		rc = morph_array_init(&selection, candidates.nelts, sizeof(*selected));
		if (rc == 0 && candidates.nelts) {
			selected = morph_array_push_n_borrow(&selection, candidates.nelts);
			if (!selected)
				rc = -ENOMEM;
		}
	}
	if (rc == 0 && !options->list)
		rc = cli_skills_manage_skills_select(options, &candidates, selected);
	/* Check every destination before installing any selected skill. */
	for (size_t i = 0; rc == 0 && !options->list && i < candidates.nelts; i++) {
		const struct cli_skill_candidate *candidate = morph_array_borrow(&candidates, i);
		char target[PATH_MAX];
		struct stat st;

		if (!selected[i])
			continue;
		rc = file_path_join(target, sizeof(target), root, candidate->fm.name);
		if (rc == 0 && lstat(target, &st) == 0) {
			if (!options->replace)
				rc = -EEXIST;
			else if (!S_ISDIR(st.st_mode))
				rc = -ELOOP;
			else {
				cJSON *metadata = cli_skill_store_metadata_copy(target);

				rc = cli_skill_store_unmodified(target, metadata);
				cJSON_Delete(metadata);
			}
		}
	}
	for (size_t i = 0; rc == 0 && !options->list && i < candidates.nelts; i++) {
		const struct cli_skill_candidate *candidate = morph_array_borrow(&candidates, i);
		cJSON *metadata;

		if (!selected[i])
			continue;
		metadata =
			cli_skills_manage_skills_metadata_create(&source, candidate, options->ref);
		rc = metadata ? cli_skill_store_install(root, candidate, metadata, options->replace)
			      : -ENOMEM;
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
	morph_array_cleanup(&candidates);
	return rc;
}

static int cli_skills_manage_skills_list(const struct cli_skills_manage_skills_options *options,
					 const char *user_dir)
{
	struct skill_registry *registry = calloc(1, sizeof(*registry));
	int rc;

	if (!registry)
		return -ENOMEM;
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

static int cli_skills_manage_skills_update_one(struct morph_log *logger, const char *root,
					       const char *name)
{
	char target[PATH_MAX];
	struct cli_skills_manage_skills_source source = {0};
	morph_array_t candidates MORPH_CLEANUP(morph_array_cleanup) = {0};
	cJSON *metadata MORPH_CLEANUP(morph_json_cleanup) = NULL;
	int rc = file_path_join(target, sizeof(target), root, name);

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
		rc = (MORPH_ERR_FORMAT);
		return rc;
	}
	rc = morph_array_init(&candidates, 1, sizeof(struct cli_skill_candidate));
	if (rc == 0)
		rc = cli_skills_manage_skills_prepare(origin, *ref ? ref : NULL,
						      strcmp(kind, "git") == 0, &source);
	if (rc == 0)
		rc = cli_skills_manage_skills_scan_source(logger, &source, path, &candidates);
	if (rc == 0) {
		const struct cli_skill_candidate *candidate = NULL;

		for (size_t i = 0; i < candidates.nelts; i++) {
			const struct cli_skill_candidate *entry =
				morph_array_borrow(&candidates, i);

			if (strcmp(entry->fm.name, name) == 0)
				candidate = entry;
		}
		if (!candidate)
			rc = (-ENOENT);
		else {
			cJSON *updated = cli_skills_manage_skills_metadata_create(
				&source, candidate, *ref ? ref : NULL);

			rc = updated ? cli_skill_store_install(root, candidate, updated, 1)
				     : -ENOMEM;
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

static int cli_skills_manage_skills_modify(struct morph_log *logger,
					   const struct cli_skills_manage_skills_options *options,
					   const char *root)
{
	char **names = NULL;
	int count = 0;
	int rc = 0;
	int result = 0;

	if (options->name_count) {
		count = options->name_count;
	} else {
		rc = file_list_dirs_copy(root, &names, &count);
		if (rc != 0)
			return rc;
	}
	for (int i = 0; i < count; i++) {
		const char *name = options->name_count ? options->names[i] : names[i];
		char target[PATH_MAX];
		cJSON *metadata;
		struct stat st;

		if (!cli_management_name_valid(name))
			continue;
		rc = 0;
		rc = file_path_join(target, sizeof(target), root, name);
		if (rc != 0)
			break;
		if (lstat(target, &st) != 0)
			rc = -errno;
		else if (!S_ISDIR(st.st_mode))
			rc = -ELOOP;
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
	file_free_list(names, options->name_count ? 0 : count);
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
	morph_array_cleanup(&options->name_storage);
}

int cli_skills_manage_run(struct morph_log *logger, int argc, char **argv, const char *config_path)
{
	struct cli_skills_manage_skills_options options
	MORPH_CLEANUP(cli_skills_manage_options_cleanup) = {0};
	char *user_dir MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *root MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char *lock MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	char resolved[PATH_MAX];
	int fd MORPH_CLEANUP(cli_skills_manage_fd_cleanup) = -1;
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
		rc = (-ENOMEM);
		return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
	}
	if (!options.list) {
		rc = file_ensure_dir(root);
		if (rc != 0)
			return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
		if (!realpath(root, resolved)) {
			rc = (-errno);
			return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
		}
		free(root);
		root = strdup(resolved);
		if (!root) {
			rc = (-ENOMEM);
			return rc == 0 ? 0 : cli_management_error(rc, "skills command failed");
		}
		lock = file_path_join_alloc(root, ".morph-skills.lock");
		if (!lock) {
			rc = (-ENOMEM);
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
