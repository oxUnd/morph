#include "install.h"
#include "ext.h"
#include "manifest.h"
#include "util/file.h"
#include "util/error.h"
#include "util/log.h"
#include <ctype.h>
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

static int is_owner_char(int ch)
{
	return isalnum(ch) || ch == '-' || ch == '_';
}

static int is_repo_char(int ch)
{
	return isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
}

static int is_ref_char(int ch)
{
	return isalnum(ch) || ch == '-' || ch == '_' || ch == '.' ||
		ch == '/' || ch == '+';
}

static int valid_component(const char *s, int (*valid)(int))
{
	if (!s || !s[0])
		return 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (!valid(*p))
			return 0;
	}
	return 1;
}

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
			unsigned char ch = (unsigned char)p[i];
			if (!(isalnum(ch) || ch == '-' || ch == '_' ||
			      ch == '.' || ch == '+'))
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
		return -EINVAL;
	if (len + 1 > dst_size)
		return -ENAMETOOLONG;
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
		return -EINVAL;
	memset(out, 0, sizeof(*out));

	if (strncmp(source, https_prefix, https_prefix_len) == 0) {
		const char *spec = source + https_prefix_len;
		const char *owner_end = strchr(spec, '/');
		if (!owner_end)
			return -EINVAL;
		const char *repo_start = owner_end + 1;
		const char *repo_end = strchr(repo_start, '/');
		if (!repo_end)
			return -EINVAL;
		if (strncmp(repo_end, "/tree/", 6) != 0)
			return -EINVAL;
		const char *ref_start = repo_end + 6;
		const char *ref_end = strchr(ref_start, '/');
		if (!ref_end)
			return -EINVAL;

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
		if (!valid_component(out->owner, is_owner_char) ||
		    !valid_component(out->repo, is_repo_char) ||
		    !valid_component(out->ref, is_ref_char) ||
		    !valid_subdir(out->subdir))
			return -EINVAL;
		return 0;
	}

	if (strncmp(source, prefix, prefix_len) != 0)
		return -EINVAL;

	const char *spec = source + prefix_len;
	const char *sub = strstr(spec, "//");
	size_t main_len = sub ? (size_t)(sub - spec) : strlen(spec);
	if (sub) {
		int rc = copy_part(out->subdir, sizeof(out->subdir),
				   sub + 2, strlen(sub + 2));
		if (rc < 0)
			return rc;
		if (!valid_subdir(out->subdir))
			return -EINVAL;
	}

	const char *slash = memchr(spec, '/', main_len);
	if (!slash)
		return -EINVAL;
	const char *at = memchr(slash + 1, '@',
				main_len - (size_t)(slash + 1 - spec));
	const char *repo_end = at ? at : spec + main_len;
	if (slash == spec || repo_end == slash + 1)
		return -EINVAL;

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
			return -EINVAL;
		rc = copy_part(out->ref, sizeof(out->ref),
			       at + 1, (size_t)(spec + main_len - at - 1));
		if (rc < 0)
			return rc;
	} else {
		strncpy(out->ref, "HEAD", sizeof(out->ref) - 1);
	}

	if (!valid_component(out->owner, is_owner_char) ||
	    !valid_component(out->repo, is_repo_char) ||
	    !valid_component(out->ref, is_ref_char))
		return -EINVAL;

	return 0;
}

static int wait_child(pid_t pid)
{
	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		MORPH_RETURN_ERRNO();
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -EIO;
	return 0;
}

static int run_cmd(char *const argv[], const char *cwd)
{
	pid_t pid = fork();
	if (pid < 0)
		MORPH_RETURN_ERRNO();
	if (pid == 0) {
		if (cwd && chdir(cwd) != 0)
			_exit(127);
		execvp(argv[0], argv);
		_exit(127);
	}
	return wait_child(pid);
}

static int run_cmd_capture(char *const argv[], const char *cwd,
			   char *dst, size_t dst_size)
{
	if (!dst || dst_size == 0)
		return -EINVAL;
	dst[0] = '\0';

	int fds[2];
	if (pipe(fds) != 0)
		MORPH_RETURN_ERRNO();

	pid_t pid = fork();
	if (pid < 0) {
		int err = errno;
		close(fds[0]);
		close(fds[1]);
		MORPH_RETURN(-err);
	}
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		if (cwd && chdir(cwd) != 0)
			_exit(127);
		execvp(argv[0], argv);
		_exit(127);
	}

	close(fds[1]);
	size_t len = 0;
	while (len + 1 < dst_size) {
		ssize_t n = read(fds[0], dst + len, dst_size - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fds[0]);
			(void)wait_child(pid);
			MORPH_RETURN_ERRNO();
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	dst[len] = '\0';
	close(fds[0]);

	int rc = wait_child(pid);
	if (rc < 0)
		return rc;
	while (len > 0 && (dst[len - 1] == '\n' || dst[len - 1] == '\r' ||
			   dst[len - 1] == ' ' || dst[len - 1] == '\t')) {
		dst[--len] = '\0';
	}
	return 0;
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
		return -ENAMETOOLONG;

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
	int rc = file_path_join(dst, dst_size, dir, "manifest.toml");
	if (rc < 0)
		return rc;
	if (file_exists(dst))
		return 0;
	rc = file_path_join(dst, dst_size, dir, "morph-ext.toml");
	if (rc < 0)
		return rc;
	if (file_exists(dst))
		return 0;
	return -ENOENT;
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

static int remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) {
		if (errno == ENOENT)
			return 0;
		MORPH_RETURN_ERRNO();
	}
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
		if (unlink(path) != 0)
			MORPH_RETURN_ERRNO();
		return 0;
	}

	DIR *dir = opendir(path);
	if (!dir)
		MORPH_RETURN_ERRNO();
	int rc = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0)
			continue;
		char child[PATH_MAX];
		rc = file_path_join(child, sizeof(child), path, ent->d_name);
		if (rc < 0)
			break;
		rc = remove_tree(child);
		if (rc < 0)
			break;
	}
	closedir(dir);
	if (rc < 0)
		return rc;
	if (rmdir(path) != 0)
		MORPH_RETURN_ERRNO();
	return 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode)
{
	int in = open(src, O_RDONLY);
	if (in < 0)
		MORPH_RETURN_ERRNO();
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
	if (out < 0) {
		int err = errno;
		close(in);
		MORPH_RETURN(-err);
	}

	char buf[BUFSIZ];
	int rc = 0;
	while (1) {
		ssize_t n = read(in, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		if (n == 0)
			break;
		char *p = buf;
		ssize_t left = n;
		while (left > 0) {
			ssize_t w = write(out, p, (size_t)left);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				rc = -errno;
				break;
			}
			p += w;
			left -= w;
		}
		if (rc < 0)
			break;
	}
	if (close(in) != 0 && rc == 0)
		rc = -errno;
	if (close(out) != 0 && rc == 0)
		rc = -errno;
	if (rc == 0 && chmod(dst, mode & 0777) != 0)
		rc = -errno;
	return rc;
}

static int copy_symlink(const char *src, const char *dst)
{
	char target[PATH_MAX];
	ssize_t n = readlink(src, target, sizeof(target) - 1);
	if (n < 0)
		MORPH_RETURN_ERRNO();
	target[n] = '\0';
	if (symlink(target, dst) != 0)
		MORPH_RETURN_ERRNO();
	return 0;
}

static int copy_tree(const char *src, const char *dst)
{
	struct stat st;
	if (lstat(src, &st) != 0)
		MORPH_RETURN_ERRNO();

	if (S_ISLNK(st.st_mode))
		return copy_symlink(src, dst);
	if (S_ISREG(st.st_mode))
		return copy_file(src, dst, st.st_mode);
	if (!S_ISDIR(st.st_mode))
		return 0;

	if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST)
		MORPH_RETURN_ERRNO();
	(void)chmod(dst, st.st_mode & 0777);

	DIR *dir = opendir(src);
	if (!dir)
		MORPH_RETURN_ERRNO();
	int rc = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0 ||
		    strcmp(ent->d_name, ".git") == 0)
			continue;
		char child_src[PATH_MAX];
		char child_dst[PATH_MAX];
		rc = file_path_join(child_src, sizeof(child_src),
				    src, ent->d_name);
		if (rc == 0)
			rc = file_path_join(child_dst, sizeof(child_dst),
					    dst, ent->d_name);
		if (rc == 0)
			rc = copy_tree(child_src, child_dst);
		if (rc < 0)
			break;
	}
	closedir(dir);
	return rc;
}

static int validate_ext_name(const char *name)
{
	return valid_component(name, is_repo_char);
}

int ext_install_source(const char *source,
		       const struct ext_install_options *opts,
		       struct ext_install_result *result)
{
	struct ext_source src;
	char tmpl[PATH_MAX];
	char repo_dir[PATH_MAX];
	char package_dir[PATH_MAX];
	char manifest_path[PATH_MAX];
	char entry_path[PATH_MAX];
	char target_dir[PATH_MAX];
	char target_tmp[PATH_MAX];
	char resolved_ref[64];
	struct ext_manifest m;
	int rc = 0;
	int cleanup_tmp = 0;
	const char *install_dir = opts && opts->install_dir ?
		opts->install_dir : "~/.morph/exts";

	if (!source)
		return -EINVAL;
	if (result)
		memset(result, 0, sizeof(*result));
	memset(&m, 0, sizeof(m));
	memset(resolved_ref, 0, sizeof(resolved_ref));

	rc = ext_source_parse(source, &src);
	if (rc < 0)
		return rc;

	const char *tmp_base = getenv("TMPDIR");
	if (!tmp_base || !tmp_base[0])
		tmp_base = "/tmp";
	rc = file_path_join(tmpl, sizeof(tmpl), tmp_base, "morph-ext-XXXXXX");
	if (rc < 0)
		return rc;
	if (!mkdtemp(tmpl))
		MORPH_RETURN_ERRNO();
	cleanup_tmp = 1;

	rc = file_path_join(repo_dir, sizeof(repo_dir), tmpl, "repo");
	if (rc < 0)
		goto out;
	rc = git_fetch_source(&src, repo_dir, opts ? opts->github_base_url : NULL,
			      resolved_ref, sizeof(resolved_ref));
	if (rc < 0)
		goto out;

	if (src.subdir[0])
		rc = file_path_join(package_dir, sizeof(package_dir),
				    repo_dir, src.subdir);
	else
		rc = copy_part(package_dir, sizeof(package_dir),
			       repo_dir, strlen(repo_dir));
	if (rc < 0)
		goto out;

	rc = find_manifest_path(package_dir, manifest_path,
				sizeof(manifest_path));
	if (rc < 0)
		goto out;
	rc = manifest_parse_file(manifest_path, &m);
	if (rc < 0)
		goto out;
	if (!m.name[0] || !m.entry[0] || !validate_ext_name(m.name) ||
	    !valid_subdir(m.entry)) {
		rc = -EINVAL;
		goto out;
	}

	if (m.build_command[0]) {
		if (!prompt_confirm(opts, &m)) {
			rc = -ECANCELED;
			goto out;
		}
		rc = run_build_command(package_dir, m.build_command);
		if (rc < 0)
			goto out;
	}

	rc = file_path_join(entry_path, sizeof(entry_path),
			    package_dir, m.entry);
	if (rc < 0)
		goto out;
	if (!file_exists(entry_path)) {
		rc = -ENOENT;
		goto out;
	}

	char *expanded_install = file_expand_path(install_dir);
	if (!expanded_install) {
		rc = -ENOMEM;
		goto out;
	}
	rc = file_ensure_dir(expanded_install);
	if (rc == 0)
		rc = file_path_join(target_dir, sizeof(target_dir),
				    expanded_install, m.name);
	free(expanded_install);
	if (rc < 0)
		goto out;
	if (file_exists(target_dir)) {
		rc = -EEXIST;
		goto out;
	}
	int n = snprintf(target_tmp, sizeof(target_tmp), "%s.tmp.%ld",
			 target_dir, (long)getpid());
	if (n < 0 || (size_t)n >= sizeof(target_tmp)) {
		rc = -ENAMETOOLONG;
		goto out;
	}
	(void)remove_tree(target_tmp);
	rc = copy_tree(package_dir, target_tmp);
	if (rc < 0)
		goto out;
	if (rename(target_tmp, target_dir) != 0) {
		rc = -errno;
		(void)remove_tree(target_tmp);
		goto out;
	}

	if (result) {
		strncpy(result->name, m.name, sizeof(result->name) - 1);
		strncpy(result->path, target_dir, sizeof(result->path) - 1);
		strncpy(result->resolved_ref, resolved_ref,
			sizeof(result->resolved_ref) - 1);
	}

out:
	if (cleanup_tmp)
		(void)remove_tree(tmpl);
	ext_manifest_cleanup(&m);
	if (rc < 0)
		log_err("ext install failed for %s: %s",
			source, morph_strerror(rc));
	return rc;
}
