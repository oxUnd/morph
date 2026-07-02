#include "file_write.h"

#include "agent/tool_context.h"
#include "cJSON.h"
#include "util/error.h"
#include "util/file.h"

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

struct file_write_context {
	struct tool_context *tctx;
};

struct decoded_content {
	unsigned char *data;
	size_t len;
};

static int json_result(struct tool_result *result, cJSON *obj)
{
	char *s;

	if (!result || !obj)
		MORPH_RETURN(-EINVAL);
	s = cJSON_PrintUnformatted(obj);
	if (!s)
		MORPH_RETURN(-ENOMEM);
	return tool_result_take_json(result, s);
}

static int json_error_result(struct tool_result *result, int code,
			     const char *message)
{
	cJSON *out;
	int rc;

	out = cJSON_CreateObject();
	if (!out)
		MORPH_RETURN(-ENOMEM);
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

static int path_parent(char *dst, size_t dst_size, const char *path)
{
	char *slash;

	if (!dst || dst_size == 0 || !path || !*path)
		MORPH_RETURN(-EINVAL);
	if (strlen(path) + 1 > dst_size)
		MORPH_RETURN(-ENAMETOOLONG);
	snprintf(dst, dst_size, "%s", path);
	slash = strrchr(dst, '/');
	if (!slash)
		snprintf(dst, dst_size, ".");
	else if (slash == dst)
		*(slash + 1) = '\0';
	else
		*slash = '\0';
	return 0;
}

static int ensure_parent_dir(const char *path)
{
	char parent[PATH_MAX];
	int rc;

	rc = path_parent(parent, sizeof(parent), path);
	if (rc < 0)
		return rc;
	return file_ensure_dir(parent);
}

static int stat_regular_or_missing(const char *path, int *exists)
{
	struct stat st;

	if (!path || !exists)
		MORPH_RETURN(-EINVAL);
	*exists = 0;
	if (lstat(path, &st) != 0) {
		if (errno == ENOENT)
			return 0;
		MORPH_RETURN_ERRNO();
	}
	*exists = 1;
	if (!S_ISREG(st.st_mode))
		MORPH_RETURN(-EINVAL);
	return 0;
}

static int stat_file_or_empty_dir(const char *path, int *is_dir)
{
	struct stat st;

	if (!path || !is_dir)
		MORPH_RETURN(-EINVAL);
	if (lstat(path, &st) != 0)
		MORPH_RETURN_ERRNO();
	*is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
		MORPH_RETURN(-EINVAL);
	return 0;
}

static int authorize_path(struct tool_context *tctx, enum tool_path_op op,
			  const char *path, char *resolved,
			  size_t resolved_size)
{
	if (!path || !*path)
		MORPH_RETURN(-EINVAL);
	if (tctx)
		return tool_context_authorize_path(tctx, op, path, resolved,
						   resolved_size);
	if (!resolved || resolved_size == 0)
		MORPH_RETURN(-EINVAL);
	if (strlen(path) + 1 > resolved_size)
		MORPH_RETURN(-ENAMETOOLONG);
	snprintf(resolved, resolved_size, "%s", path);
	return 0;
}

static int b64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return (int)(c - 'A');
	if (c >= 'a' && c <= 'z')
		return (int)(c - 'a') + 26;
	if (c >= '0' && c <= '9')
		return (int)(c - '0') + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

static int decode_base64(const char *src, struct decoded_content *out)
{
	size_t len;
	size_t cap;
	size_t clean_len;
	int pad_count;
	unsigned char *buf;
	unsigned int accum;
	int bits;
	size_t n;
	int seen_pad;

	if (!src || !out)
		MORPH_RETURN(-EINVAL);
	len = strlen(src);
	clean_len = 0;
	pad_count = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
		if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
			continue;
		if (*p == '=') {
			pad_count++;
		} else if (pad_count > 0 || b64_value(*p) < 0) {
			MORPH_RETURN(-EINVAL);
		}
		clean_len++;
	}
	if (clean_len % 4 == 1 || pad_count > 2)
		MORPH_RETURN(-EINVAL);
	cap = (len / 4 + 1) * 3;
	if (cap > FILE_WRITE_MAX_CONTENT)
		MORPH_RETURN(-EFBIG);
	buf = malloc(cap ? cap : 1);
	if (!buf)
		MORPH_RETURN(-ENOMEM);

	accum = 0;
	bits = 0;
	n = 0;
	seen_pad = 0;
	for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
		int v;

		if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
			continue;
		if (*p == '=') {
			seen_pad = 1;
			continue;
		}
		if (seen_pad) {
			free(buf);
			MORPH_RETURN(-EINVAL);
		}
		v = b64_value(*p);
		if (v < 0) {
			free(buf);
			MORPH_RETURN(-EINVAL);
		}
		accum = (accum << 6) | (unsigned int)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= FILE_WRITE_MAX_CONTENT) {
				free(buf);
				MORPH_RETURN(-EFBIG);
			}
			buf[n++] = (unsigned char)((accum >> bits) & 0xffu);
		}
	}

	out->data = buf;
	out->len = n;
	return 0;
}

static int decode_content(const char *content, const char *encoding,
			  struct decoded_content *out)
{
	size_t len;

	if (!out)
		MORPH_RETURN(-EINVAL);
	out->data = NULL;
	out->len = 0;
	if (!content)
		MORPH_RETURN(-EINVAL);
	if (!encoding || strcmp(encoding, "utf8") == 0) {
		len = strlen(content);
		if (len > FILE_WRITE_MAX_CONTENT)
			MORPH_RETURN(-EFBIG);
		out->data = malloc(len ? len : 1);
		if (!out->data)
			MORPH_RETURN(-ENOMEM);
		if (len > 0)
			memcpy(out->data, content, len);
		out->len = len;
		return 0;
	}
	if (strcmp(encoding, "base64") == 0)
		return decode_base64(content, out);
	MORPH_RETURN(-EINVAL);
}

static int write_full_fd(int fd, const unsigned char *data, size_t len)
{
	size_t off;

	off = 0;
	while (off < len) {
		ssize_t n;

		n = write(fd, data + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			MORPH_RETURN_ERRNO();
		}
		if (n == 0)
			MORPH_RETURN(-EIO);
		off += (size_t)n;
	}
	return 0;
}

static int atomic_write_file(const char *path, const unsigned char *data,
			     size_t len)
{
	char dir[PATH_MAX];
	char tmp[PATH_MAX];
	int fd;
	int rc;

	rc = path_parent(dir, sizeof(dir), path);
	if (rc < 0)
		return rc;
	rc = snprintf(tmp, sizeof(tmp), "%s/.morph-write-%ld-%lld.tmp",
		      dir, (long)getpid(), (long long)time(NULL));
	if (rc < 0 || (size_t)rc >= sizeof(tmp))
		MORPH_RETURN(-ENAMETOOLONG);

	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		MORPH_RETURN_ERRNO();

	rc = write_full_fd(fd, data, len);
	if (rc == 0 && fsync(fd) != 0)
		rc = -errno;
	if (close(fd) != 0 && rc == 0)
		rc = -errno;
	if (rc < 0) {
		(void)unlink(tmp);
		return rc;
	}
	if (rename(tmp, path) != 0) {
		rc = -errno;
		(void)unlink(tmp);
		return rc;
	}
	return 0;
}

static int create_file_exclusive(const char *path, const unsigned char *data,
				 size_t len)
{
	int fd;
	int rc;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		MORPH_RETURN_ERRNO();
	rc = write_full_fd(fd, data, len);
	if (rc == 0 && fsync(fd) != 0)
		rc = -errno;
	if (close(fd) != 0 && rc == 0)
		rc = -errno;
	if (rc < 0)
		(void)unlink(path);
	return rc;
}

static int append_file(const char *path, const unsigned char *data, size_t len)
{
	int fd;
	int rc;

	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0)
		MORPH_RETURN_ERRNO();
	rc = write_full_fd(fd, data, len);
	if (rc == 0 && fsync(fd) != 0)
		rc = -errno;
	if (close(fd) != 0 && rc == 0)
		rc = -errno;
	return rc;
}

static int copy_file(const char *src, const char *dst, int overwrite,
		     size_t *bytes_written)
{
	unsigned char buf[FILE_WRITE_COPY_BUFSIZ];
	int in_fd;
	int out_fd;
	int flags;
	int rc;
	size_t total;
	struct stat st;

	if (!src || !dst || !bytes_written)
		MORPH_RETURN(-EINVAL);
	*bytes_written = 0;
	if (lstat(src, &st) != 0)
		MORPH_RETURN_ERRNO();
	if (!S_ISREG(st.st_mode))
		MORPH_RETURN(-EINVAL);

	in_fd = open(src, O_RDONLY);
	if (in_fd < 0)
		MORPH_RETURN_ERRNO();
	flags = O_WRONLY | O_CREAT;
	flags |= overwrite ? O_TRUNC : O_EXCL;
	out_fd = open(dst, flags, 0600);
	if (out_fd < 0) {
		rc = -errno;
		close(in_fd);
		return rc;
	}

	total = 0;
	rc = 0;
	while (1) {
		ssize_t n;

		n = read(in_fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		if (n == 0)
			break;
		rc = write_full_fd(out_fd, buf, (size_t)n);
		if (rc < 0)
			break;
		total += (size_t)n;
	}
	if (rc == 0 && fsync(out_fd) != 0)
		rc = -errno;
	if (close(out_fd) != 0 && rc == 0)
		rc = -errno;
	close(in_fd);
	if (rc < 0)
		return rc;
	*bytes_written = total;
	return 0;
}

static int add_success(cJSON *out, const char *op, const char *path,
		       const char *resolved, const char *dst_path,
		       const char *resolved_dst)
{
	if (!out)
		MORPH_RETURN(-EINVAL);
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

static int op_write_like(struct file_write_context *ctx, cJSON *root,
			 struct tool_result *result, const char *op,
			 int create_parent_dirs)
{
	const char *path;
	const char *content;
	const char *encoding;
	char resolved[PATH_MAX];
	struct decoded_content decoded;
	int rc;
	int existed;
	cJSON *out;

	path = json_string(root, "path");
	content = json_string(root, "content");
	encoding = json_string(root, "encoding");
	memset(&decoded, 0, sizeof(decoded));
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
			goto out_error;
	}
	rc = stat_regular_or_missing(resolved, &existed);
	if (rc < 0)
		goto out_error;

	if (strcmp(op, "write") == 0)
		rc = create_file_exclusive(resolved, decoded.data, decoded.len);
	else if (strcmp(op, "overwrite") == 0)
		rc = atomic_write_file(resolved, decoded.data, decoded.len);
	else
		rc = append_file(resolved, decoded.data, decoded.len);
	if (rc < 0)
		goto out_error;

	out = cJSON_CreateObject();
	if (!out) {
		free(decoded.data);
		MORPH_RETURN(-ENOMEM);
	}
	add_success(out, op, path, resolved, NULL, NULL);
	cJSON_AddNumberToObject(out, "bytes_written", (double)decoded.len);
	cJSON_AddBoolToObject(out, "created", existed ? 0 : 1);
	cJSON_AddBoolToObject(out, "overwritten",
			      strcmp(op, "overwrite") == 0 && existed);
	rc = json_result(result, out);
	cJSON_Delete(out);
	free(decoded.data);
	return rc;

out_error:
	free(decoded.data);
	return json_error_result(result, rc, "file write failed");
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
	rc = file_ensure_dir(resolved);
	if (rc < 0)
		return json_error_result(result, rc, "mkdir failed");

	out = cJSON_CreateObject();
	if (!out)
		MORPH_RETURN(-ENOMEM);
	add_success(out, "mkdir", path, resolved, NULL, NULL);
	cJSON_AddBoolToObject(out, "created", existed ? 0 : 1);
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
		return json_error_result(result, -EINVAL, "missing dst_path");
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
	rc = copy_file(resolved, resolved_dst, overwrite, &bytes_written);
	if (rc < 0)
		return json_error_result(result, rc, "copy failed");

	out = cJSON_CreateObject();
	if (!out)
		MORPH_RETURN(-ENOMEM);
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
		return json_error_result(result, -EINVAL, "missing dst_path");
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
		return json_error_result(result, rc < 0 ? rc : -ENOENT,
					 "source is not a regular file");
	rc = stat_regular_or_missing(resolved_dst, &exists);
	if (rc < 0)
		return json_error_result(result, rc,
					 "destination is not a regular file");
	if (exists && !overwrite)
		return json_error_result(result, -EEXIST,
					 "destination exists");
	if (create_parent_dirs) {
		rc = ensure_parent_dir(resolved_dst);
		if (rc < 0)
			return json_error_result(result, rc,
						 "create parent dir failed");
	}
	if (rename(resolved, resolved_dst) != 0)
		return json_error_result(result, -errno, "rename failed");

	out = cJSON_CreateObject();
	if (!out)
		MORPH_RETURN(-ENOMEM);
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
			return json_error_result(result, -errno,
						 "delete directory failed");
	} else if (unlink(resolved) != 0) {
		return json_error_result(result, -errno, "delete file failed");
	}

	out = cJSON_CreateObject();
	if (!out)
		MORPH_RETURN(-ENOMEM);
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
		MORPH_RETURN(-EINVAL);
	root = cJSON_Parse(args_json ? args_json : "{}");
	if (!root)
		return json_error_result(result, -EINVAL, "invalid JSON");
	op = json_string(root, "op");
	if (!op) {
		cJSON_Delete(root);
		return json_error_result(result, -EINVAL, "missing op");
	}
	if (!json_string(root, "path")) {
		cJSON_Delete(root);
		return json_error_result(result, -EINVAL, "missing path");
	}

	create_parent_dirs = json_bool(root, "create_parent_dirs", 1);
	if (strcmp(op, "write") == 0 || strcmp(op, "overwrite") == 0 ||
	    strcmp(op, "append") == 0) {
		rc = op_write_like(ctx, root, result, op, create_parent_dirs);
	} else if (strcmp(op, "mkdir") == 0) {
		rc = op_mkdir(ctx, root, result);
	} else if (strcmp(op, "copy") == 0) {
		rc = op_copy(ctx, root, result, create_parent_dirs);
	} else if (strcmp(op, "rename") == 0) {
		rc = op_rename(ctx, root, result, create_parent_dirs);
	} else if (strcmp(op, "delete") == 0) {
		rc = op_delete(ctx, root, result);
	} else {
		rc = json_error_result(result, -EINVAL, "unknown op");
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
		"\"append\",\"mkdir\",\"rename\",\"copy\",\"delete\"]},"
		"\"path\":{\"type\":\"string\"},"
		"\"dst_path\":{\"type\":\"string\"},"
		"\"content\":{\"type\":\"string\"},"
		"\"encoding\":{\"type\":\"string\",\"enum\":[\"utf8\",\"base64\"],"
		"\"default\":\"utf8\"},"
		"\"create_parent_dirs\":{\"type\":\"boolean\",\"default\":true},"
		"\"overwrite\":{\"type\":\"boolean\",\"default\":false}},"
		"\"required\":[\"op\",\"path\"]}";

	if (!reg)
		MORPH_RETURN(-EINVAL);
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		MORPH_RETURN(-ENOMEM);
	ctx->tctx = tctx;
	return tool_register(reg, "file_write",
		"Create, overwrite, append, mkdir, rename, copy, or delete files. "
		"Relative write paths resolve under output_dir; outside paths require approval.",
		schema, file_write_exec, ctx, free);
}

int ext_run(const char *args_json, char **result_json)
{
	(void)args_json;
	if (!result_json)
		return -EINVAL;
	*result_json = strdup("{\"ok\":false,\"code\":-95,"
			      "\"error\":\"file_write requires embedded "
			      "tool_context registration\"}");
	return -ENOTSUP;
}
