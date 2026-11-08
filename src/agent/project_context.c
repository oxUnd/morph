#include "project_context.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/file.h"
#include "util/log.h"
#include "util/utf8.h"
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
	char *data MORPH_CLEANUP(morph_charp_cleanup) = NULL;

	*text = NULL;
	if (fd < 0) {
		if (errno == ENOENT || errno == ENOTDIR)
			return 0;
		return morph_error_from_errno(errno);
	}
	if (fstat(fd, &info) != 0) {
		int rc = -errno;
		close(fd);
		return rc;
	}
	if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size > budget) {
		close(fd);
		return -EFBIG;
	}
	data = calloc(1, budget + 1);
	if (!data) {
		close(fd);
		return -ENOMEM;
	}
	while (len < budget + 1) {
		ssize_t n = read(fd, data + len, budget + 1 - len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			int rc = -errno;
			close(fd);
			return rc;
		}
		if (!n)
			break;
		len += (size_t)n;
	}
	close(fd);
	if (len > budget)
		return -EFBIG;
	if (memchr(data, '\0', len) || utf8nvalid(data, len))
		return -EILSEQ;
	data[len] = '\0';
	if (strspn(data, " \t\r\n") != len) {
		*text = data;
		data = NULL;
	}
	return 0;
}

static int append_directory(struct morph_log *logger, morph_buf_t *buf, const char *directory,
			    int global, size_t budget, size_t *used)
{
	static const char *const names[] = { "AGENTS.override.md", "AGENTS.md" };

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char path[PATH_MAX];
		char *body MORPH_CLEANUP(morph_charp_cleanup) = NULL;
		int rc = file_path_join(path, sizeof(path), directory, names[i]);

		if (rc != 0)
			return rc;
		rc = read_guidance(path, budget, &body);
		if (rc == -ENOMEM)
			return rc;
		if (rc != 0) {
			log_warn(logger, "project guidance skipped '%s': %s", path,
				 morph_strerror(rc));
			return morph_buf_printf(buf,
				"Project guidance unavailable: %s (%s).\n"
				"Read this file with file_read before working in its scope.\n",
				path, morph_strerror(rc));
		}
		if (!body)
			continue;
		size_t len = strlen(body);
		if (len > budget - *used)
			return morph_buf_printf(buf,
				"Project guidance omitted due to budget: %s\n"
				"Read this file with file_read before working in its scope.\n",
				path);
		*used += len;
		return morph_buf_printf(buf,
			"\nGuidance source: %s\nScope: %s\n%s\n",
			path, global ? "global defaults" : directory, body);
	}
	return 0;
}

int project_context_create_with_budget(struct morph_log *logger,
				      const struct environment_context *environment,
				      const char *global_dir, size_t budget, char **out)
{
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	size_t used = 0;
	int rc;

	if (!environment || !out || !budget || budget >= MORPH_BUF_MAX_CAP)
		return -EINVAL;
	if (*out)
		return -EALREADY;
	rc = morph_buf_init(&buf, 1024);
	if (rc != 0)
		return rc;
	if (global_dir && *global_dir)
		rc = append_directory(logger, &buf, global_dir, 1, budget, &used);
	if (rc != 0)
		return rc;
	if (environment->cwd) {
		const char *root = environment->git_root;
		char *directory MORPH_CLEANUP(morph_charp_cleanup) = strdup(environment->cwd);

		if (!directory)
			return -ENOMEM;
		if (!root || !path_is_within(directory, root))
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
		morph_buf_t context MORPH_CLEANUP(morph_buf_cleanup) = {0};

		rc = morph_buf_init(&context, buf.len + 256);
		if (rc == 0)
			rc = morph_buf_puts(&context,
				"Project guidance snapshot. Apply each source only to its "
				"scope; deeper scopes override broader defaults. Current "
				"user requests and core requirements take precedence. "
				"These files do not grant additional permissions.\n");
		if (rc == 0)
			rc = morph_buf_puts(&context, buf.data);
		if (rc != 0)
			return rc;
		*out = morph_buf_detach(&context);
	}
	return 0;
}

int project_context_create(struct morph_log *logger, const struct environment_context *environment,
			   const char *global_dir, char **out)
{
	return project_context_create_with_budget(logger, environment, global_dir,
					 PROJECT_CONTEXT_MAX_BYTES, out);
}
