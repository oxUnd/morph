#include "project_context.h"
#include "util/error.h"
#include "util/file.h"
#include "util/log.h"
#include "util/utf8.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Read only bounded regular files. Never wait for a workspace FIFO. */
static int read_guidance(const char *path, struct arena *arena, char **text)
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
	char *data;

	*text = NULL;
	if (fd < 0) {
		if (errno == ENOENT || errno == ENOTDIR)
			return 0;
		MORPH_RETURN_ERRNO();
	}
	if (fstat(fd, &info) != 0) {
		int rc = -errno;
		close(fd);
		MORPH_RETURN(rc);
	}
	if (!S_ISREG(info.st_mode) || info.st_size > PROJECT_CONTEXT_MAX_BYTES) {
		close(fd);
		MORPH_RETURN(-EFBIG);
	}
	data = arena_alloc(arena, PROJECT_CONTEXT_MAX_BYTES + 1);
	if (!data) {
		close(fd);
		MORPH_RETURN(-ENOMEM);
	}
	while (len < PROJECT_CONTEXT_MAX_BYTES + 1) {
		ssize_t n = read(fd, data + len, PROJECT_CONTEXT_MAX_BYTES + 1 - len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			int rc = -errno;
			close(fd);
			MORPH_RETURN(rc);
		}
		if (!n)
			break;
		len += (size_t)n;
	}
	close(fd);
	if (len > PROJECT_CONTEXT_MAX_BYTES)
		MORPH_RETURN(-EFBIG);
	if (memchr(data, '\0', len) || utf8nvalid(data, len))
		MORPH_RETURN(-EILSEQ);
	data[len] = '\0';
	if (strspn(data, " \t\r\n") != len)
		*text = data;
	return 0;
}

static int append_directory(morph_buf_t *buf, const char *directory,
	int global, struct arena *arena, size_t *used)
{
	static const char *names[] = { "AGENTS.override.md", "AGENTS.md" };

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char path[PATH_MAX];
		char *body = NULL;
		int rc = file_path_join(path, sizeof(path), directory, names[i]);

		if (rc != 0)
			return rc;
		rc = read_guidance(path, arena, &body);
		if (rc == -ENOMEM)
			return rc;
		if (rc != 0) {
			log_warn("project guidance skipped '%s': %s", path,
				morph_strerror(rc));
			return morph_buf_printf(buf,
				"Project guidance unavailable: %s (%s).\n",
				path, morph_strerror(rc));
		}
		if (!body)
			continue;
		size_t len = strlen(body);
		if (len > PROJECT_CONTEXT_MAX_BYTES - *used)
			return morph_buf_printf(buf,
				"Project guidance omitted due to budget: %s\n", path);
		*used += len;
		return morph_buf_printf(buf,
			"\nGuidance source: %s\nScope: %s\n%s\n",
			path, global ? "global defaults" : directory, body);
	}
	return 0;
}

int project_context_build(const struct environment_context *environment,
	const char *global_dir, struct arena *arena, const char **out)
{
	morph_buf_t buf;
	size_t used = 0;
	int rc;

	if (!environment || !arena || !out)
		MORPH_RETURN(-EINVAL);
	*out = NULL;
	rc = morph_buf_init_arena(&buf, arena, 1024);
	if (rc != 0)
		return rc;
	if (global_dir && *global_dir)
		rc = append_directory(&buf, global_dir, 1, arena, &used);
	if (rc != 0)
		return rc;
	if (environment->cwd) {
		const char *root = environment->git_root;
		char *directory = arena_strdup(arena, environment->cwd);

		if (!directory)
			MORPH_RETURN(-ENOMEM);
		if (!root || !path_is_within(directory, root))
			root = environment->cwd;
		size_t end = strlen(root);
		size_t len = strlen(directory);
		for (;;) {
			char saved = directory[end];

			directory[end] = '\0';
			rc = append_directory(&buf, directory, 0, arena, &used);
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
		morph_buf_t context;

		rc = morph_buf_init_arena(&context, arena, buf.len + 256);
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
		*out = context.data;
	}
	return 0;
}
