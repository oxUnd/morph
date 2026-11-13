#include "file_write.h"

#include "agent/tool_context.h"
#include "cJSON.h"
#include "errors.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "nb/file.h"
#include "nb/buf.h"
#include "nb/encoding.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FILE_WRITE_MAX_CONTENT (10 * 1024 * 1024)
#define FILE_WRITE_COPY_BUFSIZ BUFSIZ
#define FILE_WRITE_JSON_INT_MAX 9007199254740991.0

struct file_write_context {
	struct tool_context *tctx;
};

static int json_result(struct tool_result *result, cJSON *obj)
{
	char *s NB_CLEANUP(cJSON_FreeTextOwner) = NULL;

	if (!result || !obj)
		return NB_ERR_INVAL;
	s = cJSON_PrintUnformatted(obj);
	if (!s)
		return NB_ERR_NOMEM;
	return tool_result_success_json_text_copy(result, s);
}

static int json_error_result(struct tool_result *result, int code,
			     const char *message)
{
	cJSON *out;
	int rc;

	out = cJSON_CreateObject();
	if (!out)
		return NB_ERR_NOMEM;
	cJSON_AddBoolToObject(out, "ok", 0);
	cJSON_AddNumberToObject(out, "code", code);
	cJSON_AddStringToObject(out, "error", message ? message : "error");
	rc = json_result(result, out);
	cJSON_Delete(out);
	return rc;
}

static const char *json_string(cJSON *root, const char *name)
{
	cJSON *item;

	item = cJSON_GetObjectItem(root, name);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_bool(cJSON *root, const char *name, int default_value)
{
	cJSON *item;

	item = cJSON_GetObjectItem(root, name);
	if (cJSON_IsBool(item))
		return cJSON_IsTrue(item) ? 1 : 0;
	return default_value;
}

static int json_off_t(cJSON *root, const char *name, int required,
		      off_t default_value, off_t *out)
{
	cJSON *item;
	double value;
	off_t converted;

	if (!root || !name || !out)
		return NB_ERR_INVAL;
	item = cJSON_GetObjectItem(root, name);
	if (!item) {
		if (required)
			return NB_ERR_INVAL;
		*out = default_value;
		return 0;
	}
	if (!cJSON_IsNumber(item))
		return NB_ERR_INVAL;
	value = item->valuedouble;
	if (value < 0 || value > FILE_WRITE_JSON_INT_MAX)
		return NB_ERR_INVAL;
	converted = (off_t)value;
	if ((double)converted != value)
		return NB_ERR_INVAL;
	*out = converted;
	return 0;
}

static int ensure_parent_dir(const char *path)
{
	nb_buf parent NB_CLEANUP(nb_buf_destroy) = {0};
	int rc = nb_path_dirname(&parent, path);
	if (rc != NB_OK)
		return rc;
	return nb_file_mkdir(nb_buf_cstr(&parent), true, NULL);
}

static int stat_regular_or_missing(const char *path, int *exists)
{
	struct stat st;

	if (!path || !exists)
		return NB_ERR_INVAL;
	*exists = 0;
	if (lstat(path, &st) != 0) {
		if (errno == ENOENT)
			return 0;
		return nb_status_from_errno(errno, NULL);
	}
	*exists = 1;
	if (!S_ISREG(st.st_mode))
		return NB_ERR_INVAL;
	return 0;
}

static int stat_file_or_empty_dir(const char *path, int *is_dir)
{
	struct stat st;

	if (!path || !is_dir)
		return NB_ERR_INVAL;
	if (lstat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);
	*is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
		return NB_ERR_INVAL;
	return 0;
}

static int authorize_path(struct tool_context *tctx, enum tool_path_op op,
			  const char *path, char *resolved,
			  size_t resolved_size)
{
	if (!path || !*path)
		return NB_ERR_INVAL;
	if (tctx)
		return tool_context_authorize_path(tctx, op, path, resolved,
						   resolved_size);
	if (!resolved || resolved_size == 0)
		return NB_ERR_INVAL;
	if (strlen(path) + 1 > resolved_size)
		return NB_ERR_NAME_TOO_LONG;
	snprintf(resolved, resolved_size, "%s", path);
	return 0;
}

static int decode_content(const char *content, const char *encoding, nb_buf *out)
{
	if (!content || !out)
		return NB_ERR_INVAL;
	size_t len = strlen(content);
	if (!encoding || strcmp(encoding, "utf8") == 0) {
		if (len > FILE_WRITE_MAX_CONTENT)
			return NB_ERR_TOO_BIG;
		return nb_buf_append(out, content, len) ? NB_OK : NB_ERR_NOMEM;
	}
	if (strcmp(encoding, "base64") != 0 ||
	    !nb_base64_valid(nb_str_from_cstr(content), NB_B64_STD))
		return NB_ERR_INVAL;
	/* Keep the input-size admission bound, including ignored whitespace. */
	if ((len / 4u + 1u) * 3u > FILE_WRITE_MAX_CONTENT)
		return NB_ERR_TOO_BIG;
	return nb_base64_decode(nb_str_from_cstr(content), NB_B64_STD, out, NULL);
}

static void file_write_fd_cleanup(int *fd)
{
	if (*fd >= 0)
		(void)close(*fd);
	*fd = -1;
}

static void file_write_temporary_cleanup(nb_buf *path)
{
	if (nb_buf_len(path))
		(void)nb_file_remove(nb_buf_cstr(path), NULL);
	nb_buf_destroy(path);
}

static int append_file(const char *path, const unsigned char *data, size_t len)
{
	int fd NB_CLEANUP(file_write_fd_cleanup) = -1;
	int rc;

	fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return nb_status_from_errno(errno, NULL);
	rc = nb_fd_write_until(fd, data, len, INT64_MAX, NULL, NULL, NULL);
	if (rc == 0 && fsync(fd) != 0)
		rc = nb_status_from_errno(errno, NULL);
	int closed = fd;
	fd = -1;
	if (close(closed) != 0 && rc == 0)
		rc = nb_status_from_errno(errno, NULL);
	return rc;
}

static int copy_range_fd(int in_fd, int out_fd, off_t len,
			 size_t *bytes_written)
{
	unsigned char buf[FILE_WRITE_COPY_BUFSIZ];
	off_t remaining;

	if (!bytes_written || len < 0)
		return NB_ERR_INVAL;
	remaining = len;
	while (remaining > 0) {
		ssize_t n;
		size_t want;
		int rc;

		want = sizeof(buf);
		if (remaining < (off_t)want)
			want = (size_t)remaining;
		n = read(in_fd, buf, want);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return nb_status_from_errno(errno, NULL);
		}
		if (n == 0)
			return NB_ERR_IO;
		rc = nb_fd_write_until(out_fd, buf, (size_t)n, INT64_MAX, NULL, NULL, NULL);
		if (rc < 0)
			return rc;
		remaining -= (off_t)n;
		*bytes_written += (size_t)n;
	}
	return 0;
}

static int atomic_patch_file(const char *path, off_t offset, off_t old_len,
			     const unsigned char *data, size_t len,
			     off_t *file_size_before, off_t *file_size_after)
{
	nb_buf parent NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf temporary NB_CLEANUP(file_write_temporary_cleanup) = {0};
	int in_fd NB_CLEANUP(file_write_fd_cleanup) = -1;
	int out_fd NB_CLEANUP(file_write_fd_cleanup) = -1;
	struct stat st;
	int rc;
	size_t bytes_written = 0;

	if (!path || offset < 0 || old_len < 0 || !file_size_before || !file_size_after)
		return NB_ERR_INVAL;
	if (lstat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);
	if (!S_ISREG(st.st_mode))
		return NB_ERR_INVAL;
	if (offset > st.st_size || old_len > st.st_size - offset)
		return NB_ERR_RANGE;
	if (st.st_size - old_len > LLONG_MAX - (off_t)len)
		return NB_ERR_TOO_BIG;
	rc = nb_path_dirname(&parent, path);
	if (rc != NB_OK)
		return rc;
	rc = nb_file_temp_in(&temporary, nb_buf_cstr(&parent), &out_fd, NULL);
	if (rc != NB_OK)
		return rc;
	in_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (in_fd < 0)
		return nb_status_from_errno(errno, NULL);
	rc = copy_range_fd(in_fd, out_fd, offset, &bytes_written);
	if (rc == NB_OK)
		rc = nb_fd_write_until(out_fd, data, len, INT64_MAX, NULL, NULL, NULL);
	if (rc == NB_OK && lseek(in_fd, offset + old_len, SEEK_SET) < 0)
		rc = nb_status_from_errno(errno, NULL);
	if (rc == NB_OK)
		rc = copy_range_fd(in_fd, out_fd, st.st_size - offset - old_len, &bytes_written);
	if (rc != NB_OK)
		return rc;
	int closed = out_fd;
	out_fd = -1;
	if (close(closed) != 0)
		return nb_status_from_errno(errno, NULL);
	rc = nb_file_copy(nb_buf_cstr(&temporary), path,
			  &(nb_file_copy_options){.overwrite = true,
						  .nofollow_source = true,
						  .new_file_mode = (uint32_t)st.st_mode & 0777u},
			  NULL);
	if (rc != NB_OK)
		return rc;
	*file_size_before = st.st_size;
	*file_size_after = st.st_size - old_len + (off_t)len;
	return NB_OK;
}

static int add_success(cJSON *out, const char *op, const char *path,
		       const char *resolved, const char *dst_path,
		       const char *resolved_dst)
{
	if (!out)
		return NB_ERR_INVAL;
	cJSON_AddBoolToObject(out, "ok", 1);
	cJSON_AddStringToObject(out, "op", op);
	cJSON_AddStringToObject(out, "path", path ? path : "");
	cJSON_AddStringToObject(out, "resolved_path", resolved ? resolved : "");
	if (dst_path)
		cJSON_AddStringToObject(out, "dst_path", dst_path);
	if (resolved_dst)
		cJSON_AddStringToObject(out, "resolved_dst_path", resolved_dst);
	return 0;
}

static int op_write_like(struct file_write_context *ctx, cJSON *root, struct tool_result *result,
			 const char *op, int create_parent_dirs)
{
	const char *path;
	const char *content;
	const char *encoding;
	char resolved[PATH_MAX];
	nb_buf decoded NB_CLEANUP(nb_buf_destroy) = {0};
	int rc;
	int existed;
	cJSON *out NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	path = json_string(root, "path");
	content = json_string(root, "content");
	encoding = json_string(root, "encoding");
	rc = authorize_path(ctx ? ctx->tctx : NULL, TOOL_PATH_WRITE, path,
			    resolved, sizeof(resolved));
	if (rc < 0)
		return json_error_result(result, rc, "write path denied");
	rc = decode_content(content, encoding, &decoded);
	if (rc < 0)
		return json_error_result(result, rc, "invalid content");
	if (create_parent_dirs) {
		rc = ensure_parent_dir(resolved);
		if (rc < 0)
			return json_error_result(result, rc, "file write failed");
	}
	rc = stat_regular_or_missing(resolved, &existed);
	if (rc < 0)
		return json_error_result(result, rc, "file write failed");

	if (strcmp(op, "append") == 0)
		rc = append_file(resolved, (const unsigned char *)nb_buf_cstr(&decoded),
				 nb_buf_len(&decoded));
	else
		rc = nb_file_write_all_atomic(
			resolved, nb_buf_cstr(&decoded), nb_buf_len(&decoded),
			&(nb_file_write_options){.mode = 0600,
						 .create_only = strcmp(op, "write") == 0},
			NULL);
	if (rc < 0)
		return json_error_result(result, rc, "file write failed");

	out = cJSON_CreateObject();
	if (!out) {
		return NB_ERR_NOMEM;
	}
	add_success(out, op, path, resolved, NULL, NULL);
	cJSON_AddNumberToObject(out, "bytes_written", (double)nb_buf_len(&decoded));
	cJSON_AddBoolToObject(out, "created", existed ? 0 : 1);
	cJSON_AddBoolToObject(out, "overwritten",
			      strcmp(op, "overwrite") == 0 && existed);
	rc = json_result(result, out);
	return rc;

}

static int op_mkdir(struct file_write_context *ctx, cJSON *root,
		    struct tool_result *result)
{
	const char *path;
	char resolved[PATH_MAX];
	int rc;
	int existed;
	struct stat st;
	cJSON *out;

	path = json_string(root, "path");
	rc = authorize_path(ctx ? ctx->tctx : NULL, TOOL_PATH_WRITE, path,
			    resolved, sizeof(resolved));
	if (rc < 0)
		return json_error_result(result, rc, "write path denied");
	existed = stat(resolved, &st) == 0 && S_ISDIR(st.st_mode);
	rc = nb_file_mkdir(resolved, true, NULL);
	if (rc < 0)
		return json_error_result(result, rc, "mkdir failed");

	out = cJSON_CreateObject();
	if (!out)
		return NB_ERR_NOMEM;
	add_success(out, "mkdir", path, resolved, NULL, NULL);
	cJSON_AddBoolToObject(out, "created", existed ? 0 : 1);
	rc = json_result(result, out);
	cJSON_Delete(out);
	return rc;
}

static int op_patch(struct file_write_context *ctx, cJSON *root,
		    struct tool_result *result)
{
	const char *path;
	const char *content;
	const char *encoding;
	char resolved[PATH_MAX];
	nb_buf decoded NB_CLEANUP(nb_buf_destroy) = {0};
	off_t offset;
	off_t old_len;
	off_t file_size_before;
	off_t file_size_after;
	int rc;
	cJSON *out;

	path = json_string(root, "path");
	content = json_string(root, "content");
	encoding = json_string(root, "encoding");
	rc = authorize_path(ctx ? ctx->tctx : NULL, TOOL_PATH_WRITE, path, resolved,
			    sizeof(resolved));
	if (rc < 0)
		return json_error_result(result, rc, "write path denied");
	rc = json_off_t(root, "offset", 1, 0, &offset);
	if (rc < 0)
		return json_error_result(result, rc, "invalid offset");
	rc = json_off_t(root, "length", 0, 0, &old_len);
	if (rc < 0)
		return json_error_result(result, rc, "invalid length");
	rc = decode_content(content, encoding, &decoded);
	if (rc < 0)
		return json_error_result(result, rc, "invalid content");

	rc = atomic_patch_file(resolved, offset, old_len,
			       (const unsigned char *)nb_buf_cstr(&decoded), nb_buf_len(&decoded),
			       &file_size_before, &file_size_after);
	if (rc < 0) {
		return json_error_result(result, rc, "patch failed");
	}

	out = cJSON_CreateObject();
	if (!out) {
		return NB_ERR_NOMEM;
	}
	add_success(out, "patch", path, resolved, NULL, NULL);
	cJSON_AddNumberToObject(out, "offset", (double)offset);
	cJSON_AddNumberToObject(out, "length", (double)old_len);
	cJSON_AddNumberToObject(out, "bytes_written", (double)nb_buf_len(&decoded));
	cJSON_AddNumberToObject(out, "file_size_before",
				(double)file_size_before);
	cJSON_AddNumberToObject(out, "file_size_after",
				(double)file_size_after);
	rc = json_result(result, out);
	cJSON_Delete(out);
	return rc;
}

static int op_copy(struct file_write_context *ctx, cJSON *root,
		   struct tool_result *result, int create_parent_dirs)
{
	const char *path;
	const char *dst_path;
	char resolved[PATH_MAX];
	char resolved_dst[PATH_MAX];
	int overwrite;
	int rc;
	size_t bytes_written;
	cJSON *out;

	path = json_string(root, "path");
	dst_path = json_string(root, "dst_path");
	overwrite = json_bool(root, "overwrite", 0);
	if (!dst_path)
		return json_error_result(result, NB_ERR_INVAL, "missing dst_path");
	rc = authorize_path(ctx ? ctx->tctx : NULL, TOOL_PATH_READ, path,
			    resolved, sizeof(resolved));
	if (rc < 0)
		return json_error_result(result, rc, "read path denied");
	rc = authorize_path(ctx ? ctx->tctx : NULL, TOOL_PATH_WRITE, dst_path,
			    resolved_dst, sizeof(resolved_dst));
	if (rc < 0)
		return json_error_result(result, rc, "write path denied");
	if (create_parent_dirs) {
		rc = ensure_parent_dir(resolved_dst);
		if (rc < 0)
			return json_error_result(result, rc,
						 "create parent dir failed");
	}
	nb_file_info info;
	rc = nb_file_stat(resolved, false, &info, NULL);
	if (rc == NB_OK && !info.is_regular)
		rc = NB_ERR_INVAL;
	if (rc == NB_OK)
		rc = nb_file_copy(resolved, resolved_dst,
				  &(nb_file_copy_options){.overwrite = overwrite != 0,
							  .nofollow_source = true,
							  .new_file_mode = 0600},
				  NULL);
	if (rc == NB_OK)
		rc = nb_file_stat(resolved_dst, false, &info, NULL);
	if (rc == NB_OK && info.size > SIZE_MAX)
		rc = NB_ERR_TOO_BIG;
	if (rc != NB_OK)
		return json_error_result(result, rc, "copy failed");
	bytes_written = (size_t)info.size;

	out = cJSON_CreateObject();
	if (!out)
		return NB_ERR_NOMEM;
	add_success(out, "copy", path, resolved, dst_path, resolved_dst);
	cJSON_AddNumberToObject(out, "bytes_written", (double)bytes_written);
	cJSON_AddBoolToObject(out, "overwritten", overwrite);
	rc = json_result(result, out);
	cJSON_Delete(out);
	return rc;
}

static int op_rename(struct file_write_context *ctx, cJSON *root,
		     struct tool_result *result, int create_parent_dirs)
{
	const char *path;
	const char *dst_path;
	char resolved[PATH_MAX];
	char resolved_dst[PATH_MAX];
	int overwrite;
	int exists;
	int rc;
	cJSON *out;

	path = json_string(root, "path");
	dst_path = json_string(root, "dst_path");
	overwrite = json_bool(root, "overwrite", 0);
	if (!dst_path)
		return json_error_result(result, NB_ERR_INVAL, "missing dst_path");
	rc = authorize_path(ctx ? ctx->tctx : NULL, TOOL_PATH_WRITE, path,
			    resolved, sizeof(resolved));
	if (rc < 0)
		return json_error_result(result, rc, "source path denied");
	rc = authorize_path(ctx ? ctx->tctx : NULL, TOOL_PATH_WRITE, dst_path,
			    resolved_dst, sizeof(resolved_dst));
	if (rc < 0)
		return json_error_result(result, rc, "destination path denied");
	rc = stat_regular_or_missing(resolved, &exists);
	if (rc < 0 || !exists)
		return json_error_result(result, rc < 0 ? rc : NB_ERR_NOT_FOUND,
					 "source is not a regular file");
	rc = stat_regular_or_missing(resolved_dst, &exists);
	if (rc < 0)
		return json_error_result(result, rc,
					 "destination is not a regular file");
	if (exists && !overwrite)
		return json_error_result(result, NB_ERR_EXISTS, "destination exists");
	if (create_parent_dirs) {
		rc = ensure_parent_dir(resolved_dst);
		if (rc < 0)
			return json_error_result(result, rc,
						 "create parent dir failed");
	}
	if (rename(resolved, resolved_dst) != 0)
		return json_error_result(result, nb_status_from_errno(errno, NULL),
					 "rename failed");

	out = cJSON_CreateObject();
	if (!out)
		return NB_ERR_NOMEM;
	add_success(out, "rename", path, resolved, dst_path, resolved_dst);
	cJSON_AddBoolToObject(out, "overwritten", exists);
	rc = json_result(result, out);
	cJSON_Delete(out);
	return rc;
}

static int op_delete(struct file_write_context *ctx, cJSON *root,
		     struct tool_result *result)
{
	const char *path;
	char resolved[PATH_MAX];
	int rc;
	int is_dir;
	cJSON *out;

	path = json_string(root, "path");
	rc = authorize_path(ctx ? ctx->tctx : NULL, TOOL_PATH_WRITE, path,
			    resolved, sizeof(resolved));
	if (rc < 0)
		return json_error_result(result, rc, "write path denied");
	rc = stat_file_or_empty_dir(resolved, &is_dir);
	if (rc < 0)
		return json_error_result(result, rc,
					 "delete target must be file or dir");
	if (is_dir) {
		if (rmdir(resolved) != 0)
			return json_error_result(result, nb_status_from_errno(errno, NULL),
						 "delete directory failed");
	} else if (unlink(resolved) != 0) {
		return json_error_result(result, nb_status_from_errno(errno, NULL),
					 "delete file failed");
	}

	out = cJSON_CreateObject();
	if (!out)
		return NB_ERR_NOMEM;
	add_success(out, "delete", path, resolved, NULL, NULL);
	cJSON_AddBoolToObject(out, "deleted", 1);
	cJSON_AddBoolToObject(out, "directory", is_dir);
	rc = json_result(result, out);
	cJSON_Delete(out);
	return rc;
}

static int file_write_exec(const char *args_json, struct tool_result *result,
			   void *user_data)
{
	struct file_write_context *ctx = user_data;
	cJSON *root;
	const char *op;
	int create_parent_dirs;
	int rc;

	if (!result)
		return NB_ERR_INVAL;
	root = cJSON_Parse(args_json ? args_json : "{}");
	if (!root)
		return json_error_result(result, NB_ERR_INVAL, "invalid JSON");
	op = json_string(root, "op");
	if (!op) {
		cJSON_Delete(root);
		return json_error_result(result, NB_ERR_INVAL, "missing op");
	}
	if (!json_string(root, "path")) {
		cJSON_Delete(root);
		return json_error_result(result, NB_ERR_INVAL, "missing path");
	}

	create_parent_dirs = json_bool(root, "create_parent_dirs", 1);
	if (strcmp(op, "write") == 0 || strcmp(op, "overwrite") == 0 ||
	    strcmp(op, "append") == 0) {
		rc = op_write_like(ctx, root, result, op, create_parent_dirs);
	} else if (strcmp(op, "patch") == 0) {
		rc = op_patch(ctx, root, result);
	} else if (strcmp(op, "mkdir") == 0) {
		rc = op_mkdir(ctx, root, result);
	} else if (strcmp(op, "copy") == 0) {
		rc = op_copy(ctx, root, result, create_parent_dirs);
	} else if (strcmp(op, "rename") == 0) {
		rc = op_rename(ctx, root, result, create_parent_dirs);
	} else if (strcmp(op, "delete") == 0) {
		rc = op_delete(ctx, root, result);
	} else {
		rc = json_error_result(result, NB_ERR_INVAL, "unknown op");
	}
	cJSON_Delete(root);
	return rc;
}

int file_write_tool_init(struct tool_registry *reg, struct tool_context *tctx)
{
	struct file_write_context *ctx;
	const char *schema =
		"{\"type\":\"object\",\"properties\":{"
		"\"op\":{\"type\":\"string\",\"enum\":[\"write\",\"overwrite\","
		"\"append\",\"patch\",\"mkdir\",\"rename\",\"copy\",\"delete\"]},"
		"\"path\":{\"type\":\"string\"},"
		"\"dst_path\":{\"type\":\"string\"},"
		"\"content\":{\"type\":\"string\","
		"\"description\":\"For write, overwrite, append, and patch. Decoded content is limited to 10 MiB per call.\"},"
		"\"encoding\":{\"type\":\"string\",\"enum\":[\"utf8\",\"base64\"],"
		"\"default\":\"utf8\"},"
		"\"offset\":{\"type\":\"number\",\"minimum\":0,"
		"\"description\":\"Byte offset for patch in an existing regular file.\"},"
		"\"length\":{\"type\":\"number\",\"minimum\":0,\"default\":0,"
		"\"description\":\"Byte count to replace for patch; 0 inserts content.\"},"
		"\"create_parent_dirs\":{\"type\":\"boolean\",\"default\":true},"
		"\"overwrite\":{\"type\":\"boolean\",\"default\":false}},"
		"\"required\":[\"op\",\"path\"]}";

	if (!reg)
		return NB_ERR_INVAL;
	struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_EXT,
		.name = "file_write",
		.description =
			"Create, overwrite, append, patch, mkdir, rename, copy, or delete files. "
			"Decoded content for write/overwrite/append/patch is limited to 10 MiB per "
			"call; "
			"larger files can be built with repeated append or copied/patch-read "
			"streamingly. "
			"Relative write paths resolve under output_dir; outside paths require "
			"approval.",
		.input_schema = schema,
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = file_write_exec,
		.user_data_destroy = free,
	};

	spec.user_data = calloc(1, sizeof(*ctx));
	if (!spec.user_data)
		return NB_ERR_NOMEM;
	ctx = spec.user_data;
	ctx->tctx = tctx;
	return tool_register_take(reg, &spec);
}

int ext_run(const char *args_json, char **result_json)
{
	(void)args_json;
	if (!result_json)
		return NB_ERR_INVAL;
	*result_json = strdup("{\"ok\":false,\"code\":-95,"
			      "\"error\":\"file_write requires embedded "
			      "tool_context registration\"}");
	return NB_ERR_UNSUPPORTED;
}
