#include "project_context.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/log.h"
#include "nb/utf8.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Read only bounded regular files. Never wait for a workspace FIFO. */
static int read_guidance(const char *path, size_t budget, char **text)
{
	struct stat info;
	int flags = O_RDONLY;
#ifdef _WIN32
	flags |= O_BINARY;
#else
	flags |= O_NONBLOCK | O_CLOEXEC;
#endif
	int fd = open(path, flags);
	size_t len = 0;
	char *data NB_CLEANUP(nb_charp_cleanup) = NULL;

	*text = NULL;
	if (fd < 0) {
		if (errno == ENOENT || errno == ENOTDIR)
			return 0;
		return nb_status_from_errno(errno, NULL);
	}
	if (fstat(fd, &info) != 0) {
		int rc = nb_status_from_errno(errno, NULL);
		close(fd);
		return rc;
	}
	if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size > budget) {
		close(fd);
		return NB_ERR_TOO_BIG;
	}
	data = calloc(1, budget + 1);
	if (!data) {
		close(fd);
		return NB_ERR_NOMEM;
	}
	while (len < budget + 1) {
		ssize_t n = read(fd, data + len, budget + 1 - len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			int rc = nb_status_from_errno(errno, NULL);
			close(fd);
			return rc;
		}
		if (!n)
			break;
		len += (size_t)n;
	}
	close(fd);
	if (len > budget)
		return NB_ERR_TOO_BIG;
	if (memchr(data, '\0', len) || nb_utf8_validate(nb_str_from_parts(data, len), NULL))
		return NB_ERR_ENCODING;
	data[len] = '\0';
	if (strspn(data, " \t\r\n") != len) {
		*text = data;
		data = NULL;
	}
	return 0;
}

static int append_directory(struct nb_log *logger, nb_buf *buf, const char *directory, int global,
			    size_t budget, size_t *used)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_1 = NB_OK;

	static const char *const names[] = { "AGENTS.override.md", "AGENTS.md" };

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char path[PATH_MAX];
		char *body NB_CLEANUP(nb_charp_cleanup) = NULL;
		int rc = ((file_status_1 = nb_path_concat(&file_value_1, directory, names[i])) !=
					  NB_OK
				  ? file_status_1
			  : (file_status_1 = nb_str_copy_to(nb_buf_str(&file_value_1), path,
							    sizeof(path))) == NB_ERR_RANGE
				  ? NB_ERR_NAME_TOO_LONG
				  : file_status_1);

		if (rc != 0)
			return rc;
		rc = read_guidance(path, budget, &body);
		if (rc == NB_ERR_NOMEM)
			return rc;
		if (rc != 0) {
			nb_log_write(logger, NB_LOG_WARN, "project guidance skipped '%s': %s", path,
				     nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
			return (nb_buf_printf(buf,
					      "Project guidance unavailable: %s (%s).\n"
					      "Read this file with file_read before working in its "
					      "scope.\n",
					      path,
					      nb_err_str(morph_error_descriptions(),
							 MORPH_ERROR_COUNT, rc))
					? 0
					: NB_ERR_NOMEM);
		}
		if (!body)
			continue;
		size_t len = strlen(body);
		if (len > budget - *used)
			return (nb_buf_printf(buf,
					      "Project guidance omitted due to budget: %s\n"
					      "Read this file with file_read before working in its "
					      "scope.\n",
					      path)
					? 0
					: NB_ERR_NOMEM);
		*used += len;
		return (nb_buf_printf(buf, "\nGuidance source: %s\nScope: %s\n%s\n", path,
				      global ? "global defaults" : directory, body)
				? 0
				: NB_ERR_NOMEM);
	}
	return 0;
}

int project_context_create_with_budget(struct nb_log *logger,
				       const struct environment_context *environment,
				       const char *global_dir, size_t budget, char **out)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	size_t used = 0;
	int rc;

	if (!environment || !out || !budget || budget >= (SIZE_MAX / 2))
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_STATE;
	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, 1024) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (global_dir && *global_dir)
		rc = append_directory(logger, &buf, global_dir, 1, budget, &used);
	if (rc != 0)
		return rc;
	if (environment->cwd) {
		const char *root = environment->git_root;
		char *directory NB_CLEANUP(nb_charp_cleanup) = strdup(environment->cwd);

		if (!directory)
			return NB_ERR_NOMEM;
		if (!root || !nb_path_is_within(directory, root, true))
			root = environment->cwd;
		size_t end = strlen(root);
		size_t len = strlen(directory);
		for (;;) {
			char saved = directory[end];

			directory[end] = '\0';
			rc = append_directory(logger, &buf, directory, 0, budget, &used);
			directory[end] = saved;
			if (rc != 0)
				return rc;
			if (end == len)
				break;
			if (directory[end] == '/')
				end++;
			while (end < len && directory[end] != '/')
				end++;
		}
	}
	if (buf.len) {
		nb_buf context NB_CLEANUP(nb_buf_cleanup) = {0};

		rc = (nb_buf_init(&context),
		      nb_buf_reserve(&context, buf.len + 256) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(
				      &context,
				      "Project guidance snapshot. Apply each source only to its "
				      "scope; deeper scopes override broader defaults. Current "
				      "user requests and core requirements take precedence. "
				      "These files do not grant additional permissions.\n")
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_append_cstr(&context, buf.ptr) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		*out = (nb_buf_ok(&context) ? strdup(nb_buf_cstr(&context)) : NULL);
		if (!*out)
			return NB_ERR_NOMEM;
	}
	return 0;
}

int project_context_create(struct nb_log *logger, const struct environment_context *environment,
			   const char *global_dir, char **out)
{
	return project_context_create_with_budget(logger, environment, global_dir,
					 PROJECT_CONTEXT_MAX_BYTES, out);
}
