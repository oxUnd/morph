#include "sapi/cli/manage.h"
#include "sapi/cli/skill_store.h"
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

struct skills_options {
	const char *action;
	const char *source;
	const char *path;
	const char *ref;
	const char *config;
	const char *names[SKILL_MAX_ENTRIES];
	int name_count;
	int global;
	int list;
	int all;
	int yes;
	int replace;
};

struct skills_source {
	char source[PATH_MAX];
	char temporary[PATH_MAX];
	char root[PATH_MAX];
	char commit[128];
	int git;
};

static void skills_usage(void)
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
	      "Alias: morph skill\n", stdout);
}

enum skills_option_id {
	SKILLS_OPTION_SKILL = 256,
	SKILLS_OPTION_PATH,
	SKILLS_OPTION_REF,
	SKILLS_OPTION_LIST,
	SKILLS_OPTION_ALL,
	SKILLS_OPTION_REPLACE,
};

static const struct option skills_long_options[] = {
	{"skill", required_argument, NULL, SKILLS_OPTION_SKILL},
	{"path", required_argument, NULL, SKILLS_OPTION_PATH},
	{"ref", required_argument, NULL, SKILLS_OPTION_REF},
	{"list", no_argument, NULL, SKILLS_OPTION_LIST},
	{"all", no_argument, NULL, SKILLS_OPTION_ALL},
	{"replace", no_argument, NULL, SKILLS_OPTION_REPLACE},
	{"global", no_argument, NULL, 'g'},
	{"yes", no_argument, NULL, 'y'},
	{"config", required_argument, NULL, 'c'},
	{"help", no_argument, NULL, 'h'},
	{NULL, 0, NULL, 0},
};

static int skills_parse(int argc, char **argv, const char *config,
			struct skills_options *options)
{
	int option;

	memset(options, 0, sizeof(*options));
	options->action = argc ? argv[0] : "--help";
	options->config = config;
	if (strcmp(options->action, "--help") == 0 ||
	    strcmp(options->action, "-h") == 0) {
		options->action = "--help";
		return 0;
	}
	cli_manage_reset_getopt();
	while ((option = getopt_long(argc, argv, "c:gyh",
				     skills_long_options, NULL)) != -1) {
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
		case SKILLS_OPTION_PATH:
			options->path = optarg;
			break;
		case SKILLS_OPTION_REF:
			options->ref = optarg;
			break;
		case SKILLS_OPTION_SKILL:
			if (options->name_count == SKILL_MAX_ENTRIES)
				MORPH_RETURN(-E2BIG);
			options->names[options->name_count++] = optarg;
			break;
		case SKILLS_OPTION_LIST:
			options->list = 1;
			break;
		case SKILLS_OPTION_ALL:
			options->all = 1;
			break;
		case SKILLS_OPTION_REPLACE:
			options->replace = 1;
			break;
		case 'h':
			options->action = "--help";
			return 0;
		default:
			MORPH_RETURN(-EINVAL);
		}
	}
	if (strcmp(options->action, "add") == 0 && optind < argc)
		options->source = argv[optind++];
	while (optind < argc) {
		if (options->name_count == SKILL_MAX_ENTRIES)
			MORPH_RETURN(-E2BIG);
		options->names[options->name_count++] = argv[optind++];
	}
	if (strcmp(options->action, "add") == 0) {
		if (!options->source || (options->all && options->name_count) ||
		    (options->list && (options->all || options->name_count || options->replace)))
			MORPH_RETURN(-EINVAL);
	} else if (options->path || options->ref || options->list || options->all ||
		   options->replace || (strcmp(options->action, "list") == 0 &&
		   options->name_count)) {
		MORPH_RETURN(-EINVAL);
	}
	if (strcmp(options->action, "remove") == 0 && !options->name_count)
		MORPH_RETURN(-EINVAL);
	for (int i = 0; i < options->name_count; i++) {
		if (!cli_manage_name_valid(options->names[i]) ||
		    strlen(options->names[i]) >= SKILL_NAME_MAX)
			MORPH_RETURN(-EINVAL);
	}
	if ((options->ref && (!*options->ref || options->ref[0] == '-')) ||
	    (options->path && !*options->path))
		MORPH_RETURN(-EINVAL);
	return 0;
}

static int skills_user_dir(const char *config_path, char **out)
{
	char *path = file_expand_path(config_path ? config_path : "~/.morph/config.toml");
	char *text;
	toml_result_t parsed;
	toml_datum_t dir;

	if (!path)
		MORPH_RETURN(-ENOMEM);
	text = file_read_all(path, NULL);
	free(path);
	if (!text) {
		if (config_path || errno != ENOENT)
			MORPH_RETURN(-EIO);
		*out = file_expand_path("~/.morph/skills");
		return *out ? 0 : -ENOMEM;
	}
	if (strlen(text) > 1024 * 1024) {
		free(text);
		MORPH_RETURN(-EFBIG);
	}
	parsed = toml_parse(text, (int)strlen(text));
	free(text);
	if (!parsed.ok) {
		fprintf(stderr, "morph: invalid config: %s\n", parsed.errmsg);
		toml_free(parsed);
		MORPH_RETURN(MORPH_ERR_PARSE);
	}
	dir = toml_get(toml_get(parsed.toptab, "skill"), "dir");
	*out = file_expand_path(dir.type == TOML_STRING && dir.u.s[0] ?
		dir.u.s : "~/.morph/skills");
	toml_free(parsed);
	return *out ? 0 : -ENOMEM;
}

static int skills_git(const char *root, const char *verb, const char *arg,
		      morph_buf_t *output)
{
	char *argv[] = {"git", "-c", "core.hooksPath=/dev/null", "-c",
		"submodule.recurse=false", (char *)verb, (char *)arg, NULL};

	return cli_manage_run(root, argv, output);
}

static int skills_repo_name_valid(const char *name)
{
	return morph_name_valid(name, "-_.") && name[0] != '.' && name[0] != '-';
}

static int skills_prepare(const char *source, const char *ref, int force_git,
			  struct skills_source *prepared)
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
			MORPH_RETURN(-ENOTDIR);
		rc = cli_manage_copy_arg(prepared->source, sizeof(prepared->source), resolved);
		if (rc != 0)
			return rc;
		rc = file_path_join(git_dir, sizeof(git_dir), resolved, ".git");
		if (rc != 0)
			return rc;
		prepared->git = force_git || file_exists(git_dir);
		if (!prepared->git) {
			if (ref)
				MORPH_RETURN(-EINVAL);
			return cli_manage_copy_arg(prepared->root,
				sizeof(prepared->root), resolved);
		}
	} else {
		prepared->git = 1;
		if (strstr(source, "://") || strchr(source, '@')) {
			rc = cli_manage_copy_arg(prepared->source,
				sizeof(prepared->source), source);
		} else {
			const char *slash = strchr(source, '/');
			char owner[PATH_MAX];

			if (!slash || slash == source || strchr(slash + 1, '/') ||
			    (size_t)(slash - source) >= sizeof(owner))
				MORPH_RETURN(-ENOENT);
			memcpy(owner, source, (size_t)(slash - source));
			owner[slash - source] = '\0';
			if (!cli_manage_name_valid(owner) || !skills_repo_name_valid(slash + 1))
				MORPH_RETURN(-EINVAL);
			size_t length_source = strlen(source);
			const char *suffix = length_source > 4 &&
				strcmp(source + length_source - 4, ".git") == 0 ? "" : ".git";
			int length = snprintf(prepared->source, sizeof(prepared->source),
				"https://github.com/%s%s", source, suffix);

			rc = length < 0 || length >= (int)sizeof(prepared->source) ?
				-ENAMETOOLONG : 0;
		}
		if (rc != 0)
			return rc;
	}
	if (prepared->source[0] == '-')
		MORPH_RETURN(-EINVAL);
	cache = file_expand_path("~/.morph/cache/skills");
	if (!cache)
		MORPH_RETURN(-ENOMEM);
	rc = file_ensure_dir(cache);
	if (rc == 0)
		rc = file_path_join(prepared->temporary, sizeof(prepared->temporary),
			cache, "checkout-XXXXXX");
	free(cache);
	if (rc != 0)
		return rc;
	if (!mkdtemp(prepared->temporary)) {
		prepared->temporary[0] = '\0';
		MORPH_RETURN_ERRNO();
	}
	rc = file_path_join(prepared->root, sizeof(prepared->root), prepared->temporary, "repo");
	if (rc != 0)
		return rc;
	char *clone[] = {"git", "-c", "core.hooksPath=/dev/null", "clone",
		"--quiet", "--no-checkout", "--", prepared->source, prepared->root, NULL};

	rc = cli_manage_run(NULL, clone, NULL);
	if (rc == 0 && ref) {
		char *fetch[] = {"git", "-c", "core.hooksPath=/dev/null", "fetch", "--quiet",
			"--", "origin", (char *)ref, NULL};

		rc = cli_manage_run(prepared->root, fetch, NULL);
	}
	if (rc == 0) {
		char *checkout[] = {"git", "-c", "core.hooksPath=/dev/null", "-c",
			"submodule.recurse=false", "checkout", "--quiet", "--detach",
			ref ? "FETCH_HEAD" : "HEAD", "--", NULL};

		rc = cli_manage_run(prepared->root, checkout, NULL);
	}
	if (rc == 0)
		rc = morph_buf_init(&output, 128);
	if (rc == 0)
		rc = skills_git(prepared->root, "rev-parse", "HEAD", &output);
	if (rc == 0) {
		char canonical[PATH_MAX];

		if (!realpath(prepared->root, canonical))
			rc = -errno;
		else
			rc = cli_manage_copy_arg(prepared->root, sizeof(prepared->root), canonical);
	}
	if (rc == 0) {
		while (output.len && (output.data[output.len - 1] == '\n' ||
				      output.data[output.len - 1] == '\r'))
			output.data[--output.len] = '\0';
		rc = cli_manage_copy_arg(prepared->commit, sizeof(prepared->commit), output.data);
	}
	morph_buf_cleanup(&output);
	return rc;
}

static int skills_scan_source(const struct skills_source *source, const char *subdir,
			      morph_array_t *candidates)
{
	char selected[PATH_MAX];
	char resolved[PATH_MAX];
	int rc = file_path_join(selected, sizeof(selected), source->root,
		subdir ? subdir : ".");

	if (rc != 0)
		return rc;
	if (subdir && file_path_is_absolute(subdir))
		MORPH_RETURN(-EINVAL);
	if (!realpath(selected, resolved))
		MORPH_RETURN_ERRNO();
	if (!path_is_within(resolved, source->root))
		MORPH_RETURN(-EACCES);
	rc = skill_store_scan(resolved, candidates);
	if (rc != 0)
		return rc;
	if (!candidates->nelts)
		MORPH_RETURN(-ENOENT);
	for (size_t i = 0; i < candidates->nelts; i++) {
		const struct skill_candidate *candidate = morph_array_get(candidates, i);

		for (size_t j = 0; j < i; j++) {
			const struct skill_candidate *previous = morph_array_get(candidates, j);

			if (strcmp(candidate->fm.name, previous->fm.name) == 0) {
				fprintf(stderr, "morph: duplicate skill name '%s'; "
					"narrow the source with --path\n",
					candidate->fm.name);
				MORPH_RETURN(-EEXIST);
			}
		}
	}
	return 0;
}

static void skills_candidates_print(const morph_array_t *candidates)
{
	for (size_t i = 0; i < candidates->nelts; i++) {
		const struct skill_candidate *candidate = morph_array_get(candidates, i);

		printf("%zu. %s\t%s\n", i + 1, candidate->fm.name, candidate->fm.description);
	}
}

static int skills_select(const struct skills_options *options,
			 const morph_array_t *candidates, int selected[SKILL_MAX_ENTRIES])
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
				const struct skill_candidate *candidate =
					morph_array_get(candidates, j);

				if (strcmp(candidate->fm.name, options->names[i]) == 0) {
					selected[j] = 1;
					found = 1;
				}
			}
			if (!found) {
				fprintf(stderr, "morph: skill not found in source: %s\n",
					options->names[i]);
				MORPH_RETURN(-ENOENT);
			}
		}
		return 0;
	}
	if (candidates->nelts == 1) {
		selected[0] = 1;
		return 0;
	}
	skills_candidates_print(candidates);
	if (options->yes || !isatty(STDIN_FILENO)) {
		fputs("morph: multiple skills; select with --skill NAME or --all\n", stderr);
		MORPH_RETURN(-EINVAL);
	}
	fputs("Select skill numbers (comma/space separated), or 'all': ", stderr);
	fflush(stderr);
	char input[BUFSIZ];
	char *save = NULL;
	char *token;
	int count = 0;

	if (!fgets(input, sizeof(input), stdin))
		MORPH_RETURN(-ECANCELED);
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
			MORPH_RETURN(-EINVAL);
		selected[index - 1] = 1;
		count++;
	}
	return count ? 0 : -ECANCELED;
}

static cJSON *skills_metadata_create(const struct skills_source *source,
				     const struct skill_candidate *candidate,
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

static int skills_add(const struct skills_options *options, const char *root)
{
	struct skills_source source = {0};
	morph_array_t candidates = {0};
	int selected[SKILL_MAX_ENTRIES] = {0};
	int rc = morph_array_init(&candidates, 16, sizeof(struct skill_candidate));

	if (rc != 0)
		return rc;
	rc = skills_prepare(options->source, options->ref, 0, &source);
	if (rc == 0)
		rc = skills_scan_source(&source, options->path, &candidates);
	if (rc == 0 && options->list)
		skills_candidates_print(&candidates);
	if (rc == 0 && !options->list)
		rc = skills_select(options, &candidates, selected);
	/* Check every destination before installing any selected skill. */
	for (size_t i = 0; rc == 0 && !options->list && i < candidates.nelts; i++) {
		const struct skill_candidate *candidate = morph_array_get(&candidates, i);
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
				cJSON *metadata = skill_store_metadata(target);

				rc = skill_store_unmodified(target, metadata);
				cJSON_Delete(metadata);
			}
		}
	}
	for (size_t i = 0; rc == 0 && !options->list && i < candidates.nelts; i++) {
		const struct skill_candidate *candidate = morph_array_get(&candidates, i);
		cJSON *metadata;

		if (!selected[i])
			continue;
		metadata = skills_metadata_create(&source, candidate, options->ref);
		rc = metadata ? skill_store_install(root, candidate, metadata,
			options->replace) : -ENOMEM;
		cJSON_Delete(metadata);
		if (rc == 0)
			printf("Installed '%s' in %s (%s scope).\n", candidate->fm.name,
				root, options->global ? "user" : "project");
	}
	if (source.temporary[0]) {
		int cleanup_rc = skill_store_remove(source.temporary);

		if (rc == 0)
			rc = cleanup_rc;
	}
	morph_array_cleanup(&candidates);
	return rc;
}

static int skills_list(const struct skills_options *options, const char *user_dir)
{
	struct skill_registry *registry = calloc(1, sizeof(*registry));
	int rc;

	if (!registry)
		MORPH_RETURN(-ENOMEM);
	skill_registry_init(registry);
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
			const struct skill_entry *entry = &registry->entries[i];
			cJSON *metadata = skill_store_metadata(entry->skill_dir);
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

static const char *metadata_string(const cJSON *metadata, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(metadata, key);

	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int skills_update_one(const char *root, const char *name)
{
	char target[PATH_MAX];
	struct skills_source source = {0};
	morph_array_t candidates = {0};
	cJSON *metadata = NULL;
	int rc = file_path_join(target, sizeof(target), root, name);

	if (rc != 0)
		return rc;
	metadata = skill_store_metadata(target);
	rc = skill_store_unmodified(target, metadata);
	if (rc != 0)
		goto out;
	const char *origin = metadata_string(metadata, "source");
	const char *kind = metadata_string(metadata, "kind");
	const char *path = metadata_string(metadata, "path");
	const char *ref = metadata_string(metadata, "ref");

	if (!origin || !kind || !path || !ref) {
		MORPH_SET_ERR(rc, MORPH_ERR_FORMAT);
		goto out;
	}
	rc = morph_array_init(&candidates, 1, sizeof(struct skill_candidate));
	if (rc == 0)
		rc = skills_prepare(origin, *ref ? ref : NULL, strcmp(kind, "git") == 0, &source);
	if (rc == 0)
		rc = skills_scan_source(&source, path, &candidates);
	if (rc == 0) {
		const struct skill_candidate *candidate = NULL;

		for (size_t i = 0; i < candidates.nelts; i++) {
			const struct skill_candidate *entry = morph_array_get(&candidates, i);

			if (strcmp(entry->fm.name, name) == 0)
				candidate = entry;
		}
		if (!candidate)
			MORPH_SET_ERR(rc, -ENOENT);
		else {
			cJSON *updated = skills_metadata_create(&source, candidate,
				*ref ? ref : NULL);

			rc = updated ? skill_store_install(root, candidate, updated, 1) : -ENOMEM;
			cJSON_Delete(updated);
			if (rc == 0)
				printf("Updated '%s' in %s.\n", name, root);
		}
	}
out:
	if (source.temporary[0]) {
		int cleanup_rc = skill_store_remove(source.temporary);

		if (rc == 0)
			rc = cleanup_rc;
	}
	morph_array_cleanup(&candidates);
	cJSON_Delete(metadata);
	return rc;
}

static int skills_modify(const struct skills_options *options, const char *root)
{
	char **names = NULL;
	int count = 0;
	int rc = 0;
	int result = 0;

	if (options->name_count) {
		count = options->name_count;
	} else {
		rc = file_list_dirs(root, &names, &count);
		if (rc != 0)
			return rc;
	}
	for (int i = 0; i < count; i++) {
		const char *name = options->name_count ? options->names[i] : names[i];
		char target[PATH_MAX];
		cJSON *metadata;
		struct stat st;

		if (!cli_manage_name_valid(name))
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
		metadata = skill_store_metadata(target);
		if (!metadata && !options->name_count)
			continue;
		if (strcmp(options->action, "remove") == 0) {
			rc = skill_store_unmodified(target, metadata);
			if (rc == 0)
				rc = skill_store_remove(target);
			if (rc == 0)
				printf("Removed '%s' from %s.\n", name, root);
		} else {
			rc = skills_update_one(root, name);
		}
		cJSON_Delete(metadata);
		if (rc != 0) {
			cli_manage_error(rc, name);
			result = rc;
		}
	}
	file_free_list(names, options->name_count ? 0 : count);
	return result ? result : rc;
}

int cli_manage_skills(int argc, char **argv, const char *config_path)
{
	struct skills_options options;
	char *user_dir = NULL;
	char *root = NULL;
	char *lock = NULL;
	char resolved[PATH_MAX];
	int fd = -1;
	int rc = skills_parse(argc, argv, config_path, &options);

	if (rc != 0) {
		skills_usage();
		return 2;
	}
	if (strcmp(options.action, "--help") == 0 || strcmp(options.action, "-h") == 0) {
		skills_usage();
		return 0;
	}
	if (strcmp(options.action, "add") != 0 && strcmp(options.action, "list") != 0 &&
	    strcmp(options.action, "remove") != 0 && strcmp(options.action, "update") != 0) {
		skills_usage();
		return 2;
	}
	rc = skills_user_dir(options.config, &user_dir);
	if (rc != 0)
		goto out;
	if (strcmp(options.action, "list") == 0) {
		rc = skills_list(&options, user_dir);
		goto out;
	}
	root = options.global ? strdup(user_dir) : strdup(".agents/skills");
	if (!root) {
		MORPH_SET_ERR(rc, -ENOMEM);
		goto out;
	}
	if (!options.list) {
		rc = file_ensure_dir(root);
		if (rc != 0)
			goto out;
		if (!realpath(root, resolved)) {
			MORPH_SET_ERR(rc, -errno);
			goto out;
		}
		free(root);
		root = strdup(resolved);
		if (!root) {
			MORPH_SET_ERR(rc, -ENOMEM);
			goto out;
		}
		lock = file_path_join_alloc(root, ".morph-skills.lock");
		if (!lock) {
			MORPH_SET_ERR(rc, -ENOMEM);
			goto out;
		}
		fd = cli_manage_lock(lock);
		if (fd < 0) {
			MORPH_SET_ERR(rc, fd);
			goto out;
		}
	}
	if (strcmp(options.action, "add") == 0)
		rc = skills_add(&options, root);
	else
		rc = skills_modify(&options, root);
out:
	if (fd >= 0)
		close(fd);
	free(lock);
	free(root);
	free(user_dir);
	return rc == 0 ? 0 : cli_manage_error(rc, "skills command failed");
}
