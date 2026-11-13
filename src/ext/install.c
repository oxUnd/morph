#include "nb/process.h"
#include "nb/utf8.h"
#include "install.h"
#include "ext.h"
#include "manifest.h"
#include "nb/file.h"
#include "errors.h"
#include "nb/log.h"
#include "nb/str.h"
#include "nb/cleanup.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int valid_subdir(const char *s)
{
	if (!s || !s[0])
		return 1;
	if (s[0] == '/')
		return 0;
	const char *p = s;
	while (*p) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (len == 0)
			return 0;
		if ((len == 1 && p[0] == '.') ||
		    (len == 2 && p[0] == '.' && p[1] == '.'))
			return 0;
		for (size_t i = 0; i < len; i++) {
			char ch = p[i];
			if (!nb_str_is_alnum(nb_str_from_parts(&(ch), 1), "-_.+"))
				return 0;
		}
		if (!slash)
			break;
		p = slash + 1;
	}
	return 1;
}

static int copy_part(char *dst, size_t dst_size,
		     const char *start, size_t len)
{
	if (!dst || dst_size == 0 || !start)
		return NB_ERR_INVAL;
	if (len + 1 > dst_size)
		return NB_ERR_NAME_TOO_LONG;
	memcpy(dst, start, len);
	dst[len] = '\0';
	return 0;
}

int ext_source_parse(const char *source, struct ext_source *out)
{
	const char *prefix = "github:";
	const char *https_prefix = "https://github.com/";
	size_t prefix_len = strlen(prefix);
	size_t https_prefix_len = strlen(https_prefix);

	if (!source || !out)
		return NB_ERR_INVAL;
	memset(out, 0, sizeof(*out));

	if (strncmp(source, https_prefix, https_prefix_len) == 0) {
		const char *spec = source + https_prefix_len;
		const char *owner_end = strchr(spec, '/');
		if (!owner_end)
			return NB_ERR_INVAL;
		const char *repo_start = owner_end + 1;
		const char *repo_end = strchr(repo_start, '/');
		if (!repo_end)
			return NB_ERR_INVAL;
		if (strncmp(repo_end, "/tree/", 6) != 0)
			return NB_ERR_INVAL;
		const char *ref_start = repo_end + 6;
		const char *ref_end = strchr(ref_start, '/');
		if (!ref_end)
			return NB_ERR_INVAL;

		int rc = copy_part(out->owner, sizeof(out->owner),
				   spec, (size_t)(owner_end - spec));
		if (rc < 0)
			return rc;
		rc = copy_part(out->repo, sizeof(out->repo),
			       repo_start, (size_t)(repo_end - repo_start));
		if (rc < 0)
			return rc;
		rc = copy_part(out->ref, sizeof(out->ref),
			       ref_start, (size_t)(ref_end - ref_start));
		if (rc < 0)
			return rc;
		rc = copy_part(out->subdir, sizeof(out->subdir),
			       ref_end + 1, strlen(ref_end + 1));
		if (rc < 0)
			return rc;
		if (!nb_str_is_alnum(nb_str_from_cstr(out->owner), "-_") ||
		    !nb_str_is_alnum(nb_str_from_cstr(out->repo), "-_.") ||
		    !nb_str_is_alnum(nb_str_from_cstr(out->ref), "-_./+") ||
		    !valid_subdir(out->subdir))
			return NB_ERR_INVAL;
		return 0;
	}

	if (strncmp(source, prefix, prefix_len) != 0)
		return NB_ERR_INVAL;

	const char *spec = source + prefix_len;
	const char *sub = strstr(spec, "//");
	size_t main_len = sub ? (size_t)(sub - spec) : strlen(spec);
	if (sub) {
		int rc = copy_part(out->subdir, sizeof(out->subdir),
				   sub + 2, strlen(sub + 2));
		if (rc < 0)
			return rc;
		if (!valid_subdir(out->subdir))
			return NB_ERR_INVAL;
	}

	const char *slash = memchr(spec, '/', main_len);
	if (!slash)
		return NB_ERR_INVAL;
	const char *at = memchr(slash + 1, '@',
				main_len - (size_t)(slash + 1 - spec));
	const char *repo_end = at ? at : spec + main_len;
	if (slash == spec || repo_end == slash + 1)
		return NB_ERR_INVAL;

	int rc = copy_part(out->owner, sizeof(out->owner),
			   spec, (size_t)(slash - spec));
	if (rc < 0)
		return rc;
	rc = copy_part(out->repo, sizeof(out->repo),
		       slash + 1, (size_t)(repo_end - slash - 1));
	if (rc < 0)
		return rc;
	if (at) {
		if (at + 1 == spec + main_len)
			return NB_ERR_INVAL;
		rc = copy_part(out->ref, sizeof(out->ref),
			       at + 1, (size_t)(spec + main_len - at - 1));
		if (rc < 0)
			return rc;
	} else {
		(void)nb_str_copy_to(
			nb_str_from_parts("HEAD",
					  nb_utf8_clamp_bytes("HEAD", sizeof(out->ref) - 1)),
			out->ref, (sizeof(out->ref) - 1) + 1);
	}

	if (!nb_str_is_alnum(nb_str_from_cstr(out->owner), "-_") ||
	    !nb_str_is_alnum(nb_str_from_cstr(out->repo), "-_.") ||
	    !nb_str_is_alnum(nb_str_from_cstr(out->ref), "-_./+"))
		return NB_ERR_INVAL;

	return 0;
}

static int run_cmd(char *const argv[], const char *cwd)
{
	nb_process_options options = {
		.argv = (const char *const *)argv,
		.cwd = cwd,
		.search_path = true,
		.close_fds = true,
		.stdin_mode = NB_PROCESS_INHERIT,
		.stdout_mode = NB_PROCESS_INHERIT,
		.stderr_mode = NB_PROCESS_INHERIT,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	if (rc != NB_OK)
		return rc;
	int code = 0;
	rc = nb_process_wait(child, -1, &code, NULL, NULL);
	return rc != NB_OK ? rc : code == 0 ? NB_OK : MORPH_ERR_PROCESSING;
}

static int run_cmd_capture(char *const argv[], const char *cwd,
			   char *dst, size_t dst_size)
{
	if (!dst || dst_size == 0)
		return NB_ERR_INVAL;
	dst[0] = '\0';
	nb_process_options options = {
		.argv = (const char *const *)argv,
		.cwd = cwd,
		.search_path = true,
		.close_fds = true,
		.new_group = true,
		.stdin_mode = NB_PROCESS_NULL,
		.stderr_mode = NB_PROCESS_INHERIT,
	};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	if (rc != NB_OK)
		return rc;
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf errors NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_process_exchange_options exchange = {.max_output = dst_size - 1};
	int code = 0;
	rc = nb_process_exchange(child, "", 0, &exchange, &output, &errors, &code, NULL);
	if (rc != NB_OK)
		return rc;
	if (code != 0)
		return MORPH_ERR_PROCESSING;
	nb_str trimmed = nb_str_rtrim(nb_buf_str(&output));
	memcpy(dst, trimmed.ptr, trimmed.len);
	dst[trimmed.len] = '\0';
	return NB_OK;
}

static int git_fetch_source(const struct ext_source *src, const char *repo_dir,
			    const char *base_url, char *resolved_ref,
			    size_t resolved_ref_size)
{
	char url[PATH_MAX];
	const char *base = base_url && base_url[0] ?
		base_url : "https://github.com";
	int n = snprintf(url, sizeof(url), "%s/%s/%s.git",
			 base, src->owner, src->repo);
	if (n < 0 || (size_t)n >= sizeof(url))
		return NB_ERR_NAME_TOO_LONG;

	char *init_argv[] = {"git", "init", (char *)repo_dir, NULL};
	int rc = run_cmd(init_argv, NULL);
	if (rc < 0)
		return rc;
	char *remote_argv[] = {
		"git", "-C", (char *)repo_dir, "remote", "add",
		"origin", url, NULL
	};
	rc = run_cmd(remote_argv, NULL);
	if (rc < 0)
		return rc;
	char *fetch_argv[] = {
		"git", "-C", (char *)repo_dir, "fetch", "--depth", "1",
		"origin", (char *)src->ref, NULL
	};
	rc = run_cmd(fetch_argv, NULL);
	if (rc < 0)
		return rc;
	char *checkout_argv[] = {
		"git", "-C", (char *)repo_dir, "checkout", "--detach",
		"FETCH_HEAD", NULL
	};
	rc = run_cmd(checkout_argv, NULL);
	if (rc < 0)
		return rc;
	char *rev_argv[] = {
		"git", "-C", (char *)repo_dir, "rev-parse", "HEAD", NULL
	};
	return run_cmd_capture(rev_argv, NULL, resolved_ref,
			       resolved_ref_size);
}

static int find_manifest_path(const char *dir, char *dst, size_t dst_size)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	int rc = ((file_status_1 = nb_path_concat(&file_value_1, dir, "manifest.toml")) != NB_OK
			  ? file_status_1
		  : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), dst, dst_size)) ==
				  NB_ERR_RANGE
			  ? NB_ERR_NAME_TOO_LONG
			  : file_status_1);
	if (rc < 0)
		return rc;
	if (nb_file_exists(dst))
		return 0;
	rc = ((file_status_2 = nb_path_concat(&file_value_2, dir, "morph-ext.toml")) != NB_OK
		      ? file_status_2
	      : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), dst, dst_size)) ==
			      NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_2);
	if (rc < 0)
		return rc;
	if (nb_file_exists(dst))
		return 0;
	return NB_ERR_NOT_FOUND;
}

static int prompt_confirm(const struct ext_install_options *opts,
			  const struct ext_manifest *m)
{
	FILE *in = opts && opts->in ? opts->in : stdin;
	FILE *out = opts && opts->out ? opts->out : stdout;
	char answer[16];

	if (opts && opts->yes)
		return 1;
	fprintf(out, "Extension '%s' requires build:\n", m->name);
	fprintf(out, "  %s\n", m->build_command);
	fprintf(out, "Run build command? [y/N] ");
	fflush(out);
	if (!fgets(answer, sizeof(answer), in))
		return 0;
	return answer[0] == 'y' || answer[0] == 'Y';
}

static int run_build_command(const char *package_dir, const char *command)
{
	char *argv[] = {"/bin/sh", "-c", (char *)command, NULL};
	return run_cmd(argv, package_dir);
}

static nb_status ext_package_copy_filter(const char *source, const char *destination,
					 const nb_file_copy_entry *entry, bool *include,
					 void *context, nb_err *err)
{
	(void)destination;
	(void)context;
	(void)err;
	*include = !(entry->depth > 0 && !strcmp(nb_path_basename(source), ".git")) &&
		   (entry->info.is_regular || entry->info.is_dir || entry->info.is_symlink);
	return NB_OK;
}

static int validate_ext_name(const char *name)
{
	return nb_str_is_alnum(nb_str_from_cstr(name), "-_.");
}

static int ext_install_package(struct nb_log *logger, const struct ext_source *src,
			       const char *directory, const struct ext_install_options *opts,
			       struct ext_install_result *result)
{
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_6 = NB_OK;
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_7 = NB_OK;
	nb_buf file_value_8 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_8 = NB_OK;
	nb_buf file_value_9 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_10 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_10 = NB_OK;

	char repo_dir[PATH_MAX];
	char package_dir[PATH_MAX];
	char manifest_path[PATH_MAX];
	char entry_path[PATH_MAX];
	char target_dir[PATH_MAX];
	char target_tmp[PATH_MAX];
	char resolved_ref[64] = {0};
	struct ext_manifest m NB_CLEANUP(ext_manifest_cleanup) = {0};
	int rc = 0;
	const char *install_dir = opts && opts->install_dir ?
		opts->install_dir : "~/.morph/exts";

	rc = ((file_status_6 = nb_path_concat(&file_value_6, directory, "repo")) != NB_OK
		      ? file_status_6
	      : (file_status_6 = nb_str_copy_to(nb_buf_str(&file_value_6), repo_dir,
						sizeof(repo_dir))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_6);
	if (rc < 0)
		return rc;
	rc = git_fetch_source(src, repo_dir, opts ? opts->github_base_url : NULL,
			      resolved_ref, sizeof(resolved_ref));
	if (rc < 0)
		return rc;

	if (src->subdir[0])
		rc = ((file_status_7 = nb_path_concat(&file_value_7, repo_dir, src->subdir)) !=
				      NB_OK
			      ? file_status_7
		      : (file_status_7 = nb_str_copy_to(nb_buf_str(&file_value_7), package_dir,
							sizeof(package_dir))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_7);
	else
		rc = copy_part(package_dir, sizeof(package_dir),
			       repo_dir, strlen(repo_dir));
	if (rc < 0)
		return rc;

	rc = find_manifest_path(package_dir, manifest_path,
				sizeof(manifest_path));
	if (rc < 0)
		return rc;
	rc = manifest_parse_file(logger, manifest_path, &m);
	if (rc < 0)
		return rc;
	if (!m.name[0] || !m.entry[0] || !validate_ext_name(m.name) ||
	    !valid_subdir(m.entry)) {
		rc = NB_ERR_INVAL;
		return rc;
	}

	if (m.build_command[0]) {
		if (!prompt_confirm(opts, &m)) {
			rc = NB_ERR_CANCELLED;
			return rc;
		}
		rc = run_build_command(package_dir, m.build_command);
		if (rc < 0)
			return rc;
	}

	rc = ((file_status_8 = nb_path_concat(&file_value_8, package_dir, m.entry)) != NB_OK
		      ? file_status_8
	      : (file_status_8 = nb_str_copy_to(nb_buf_str(&file_value_8), entry_path,
						sizeof(entry_path))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_8);
	if (rc < 0)
		return rc;
	if (!nb_file_exists(entry_path)) {
		rc = NB_ERR_NOT_FOUND;
		return rc;
	}

	char *expanded_install NB_CLEANUP(nb_charp_cleanup) =
		(nb_path_expand(&file_value_9, install_dir, NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_9))
			 : NULL);
	if (!expanded_install) {
		rc = NB_ERR_NOMEM;
		return rc;
	}
	rc = nb_file_mkdir(expanded_install, true, NULL);
	if (rc == 0)
		rc = ((file_status_10 = nb_path_concat(&file_value_10, expanded_install, m.name)) !=
				      NB_OK
			      ? file_status_10
		      : (file_status_10 = nb_str_copy_to(nb_buf_str(&file_value_10), target_dir,
							 sizeof(target_dir))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_10);
	nb_charp_cleanup(&expanded_install);
	if (rc < 0)
		return rc;
	if (nb_file_exists(target_dir)) {
		rc = NB_ERR_EXISTS;
		return rc;
	}
	int n = snprintf(target_tmp, sizeof(target_tmp), "%s.tmp.%ld",
			 target_dir, (long)getpid());
	if (n < 0 || (size_t)n >= sizeof(target_tmp)) {
		rc = NB_ERR_NAME_TOO_LONG;
		return rc;
	}
	(void)nb_file_remove_all(target_tmp, 0, NULL);
	rc = nb_file_copy(package_dir, target_tmp,
			  &(nb_file_copy_options){.recursive = true,
						  .preserve_mode = true,
						  .copy_symlinks = true,
						  .overwrite = true,
						  .filter = ext_package_copy_filter},
			  NULL);
	if (rc < 0)
		return rc;
	if (rename(target_tmp, target_dir) != 0) {
		rc = nb_status_from_errno(errno, NULL);
		(void)nb_file_remove_all(target_tmp, 0, NULL);
		return rc;
	}

	if (result) {
		(void)nb_str_copy_to(
			nb_str_from_parts(m.name,
					  nb_utf8_clamp_bytes(m.name, sizeof(result->name) - 1)),
			result->name, (sizeof(result->name) - 1) + 1);
		(void)nb_str_copy_to(
			nb_str_from_parts(
				target_dir,
				nb_utf8_clamp_bytes(target_dir, sizeof(result->path) - 1)),
			result->path, (sizeof(result->path) - 1) + 1);
		(void)nb_str_copy_to(
			nb_str_from_parts(resolved_ref,
					  nb_utf8_clamp_bytes(resolved_ref,
							      sizeof(result->resolved_ref) - 1)),
			result->resolved_ref, (sizeof(result->resolved_ref) - 1) + 1);
	}

	return rc;
}

int ext_install_source(struct nb_log *logger, const char *source,
		       const struct ext_install_options *opts, struct ext_install_result *result)
{
	nb_buf file_value_11 NB_CLEANUP(nb_buf_destroy) = {0};

	struct ext_source src;
	char temporary[PATH_MAX];
	const char *tmp_base = getenv("TMPDIR");
	int rc;

	if (!source)
		return NB_ERR_INVAL;
	if (result)
		memset(result, 0, sizeof(*result));
	rc = ext_source_parse(source, &src);
	if (rc < 0)
		return rc;
	if (!tmp_base || !tmp_base[0])
		tmp_base = "/tmp";
	rc = nb_file_temp_dir(&file_value_11, tmp_base, "morph-ext-", NULL);
	if (rc == NB_OK)
		rc = nb_str_copy_to(nb_buf_str(&file_value_11), temporary, sizeof(temporary));
	if (rc != NB_OK) {
		if (!nb_buf_empty(&file_value_11))
			(void)nb_file_remove_all(nb_buf_cstr(&file_value_11), 0, NULL);
		return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
	}

	rc = ext_install_package(logger, &src, temporary, opts, result);
	(void)nb_file_remove_all(temporary, 0, NULL);
	if (rc < 0)
		nb_log_write(logger, NB_LOG_ERROR, "ext install failed for %s: %s", source,
			     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return rc;
}
