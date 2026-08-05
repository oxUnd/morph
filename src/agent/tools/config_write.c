#include "config_write.h"
#include "agent/tool_context.h"
#include "agent/patch.h"
#include "util/file.h"
#include "util/error.h"
#include "config/config.h"
#include "cJSON.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_EDIT_MAX_SIZE (1024 * 1024)
#define CONFIG_EDIT_PATCH_FILE "config.toml"

struct config_edit_context {
	struct tool_context *tctx;
	char config_path[PATH_MAX];
};

static void config_edit_context_destroy(void *user_data)
{
	free(user_data);
}

static int path_equal_config(const char *path, const char *config_path)
{
	char *a;
	char *b;
	int same;

	if (!path || !config_path)
		return 0;
	a = file_expand_path(path);
	b = file_expand_path(config_path);
	if (!a || !b) {
		free(a);
		free(b);
		return 0;
	}
	same = strcmp(a, b) == 0;
	free(a);
	free(b);
	return same;
}

static int validate_config_content(const char *content, char *errbuf,
				   size_t errbuf_size)
{
	struct config_validation_error error = {0};
	int rc;

	if (!content)
		MORPH_RETURN(-EINVAL);
	if (errbuf_size > 0)
		errbuf[0] = '\0';
	rc = config_validate_text(content, &error);
	if (rc != 0 && errbuf_size > 0) {
		if (error.path[0])
			snprintf(errbuf, errbuf_size, "%s: %s",
				error.path, error.message);
		else
			snprintf(errbuf, errbuf_size, "%s", error.message);
	}
	return rc;
}

static int ensure_parent_dir(const char *path)
{
	char tmp[PATH_MAX];
	char *dir;

	if (!path)
		MORPH_RETURN(-EINVAL);
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	dir = dirname(tmp);
	if (!dir || !*dir)
		return 0;
	return file_ensure_dir(dir);
}

static int validate_patch_scope(const char *patch, char *error,
				size_t error_size)
{
	char *copy;
	char *save = NULL;
	char *line;
	int updates = 0;

	if (!patch || strlen(patch) > CONFIG_EDIT_MAX_SIZE)
		MORPH_RETURN(-EFBIG);
	copy = strdup(patch);
	if (!copy)
		MORPH_RETURN(-ENOMEM);
	for (line = strtok_r(copy, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *begin = line;
		size_t len;

		while (*begin == ' ' || *begin == '\t')
			begin++;
		len = strlen(begin);
		while (len > 0 && (begin[len - 1] == '\r' ||
				   begin[len - 1] == ' ' ||
				   begin[len - 1] == '\t'))
			begin[--len] = '\0';
		if (strncmp(begin, "*** Update File: ", 17) == 0) {
			if (strcmp(begin, "*** Update File: "
				   CONFIG_EDIT_PATCH_FILE) != 0) {
				snprintf(error, error_size,
					 "patch may only update %s",
					 CONFIG_EDIT_PATCH_FILE);
				free(copy);
				MORPH_RETURN(-EACCES);
			}
			updates++;
		} else if (strncmp(begin, "*** Add File: ", 14) == 0 ||
			   strncmp(begin, "*** Delete File: ", 17) == 0 ||
			   strncmp(begin, "*** Move to: ", 13) == 0) {
			snprintf(error, error_size,
				 "config_edit only accepts an update patch");
			free(copy);
			MORPH_RETURN(-EACCES);
		}
	}
	free(copy);
	if (updates != 1) {
		snprintf(error, error_size,
			 "patch must contain exactly one update for %s",
			 CONFIG_EDIT_PATCH_FILE);
		MORPH_RETURN(-EINVAL);
	}
	return 0;
}

static int build_content_from_patch(const char *old_content,
				    const char *patch, char **out,
				    char *error, size_t error_size)
{
	char temp_dir[] = "/tmp/morph-config-edit-XXXXXX";
	char stage_path[PATH_MAX];
	struct patch_result patch_result = {0};
	struct patch_change *change;
	int rc;

	if (!old_content || !out)
		MORPH_RETURN(-EINVAL);
	stage_path[0] = '\0';
	*out = NULL;
	rc = validate_patch_scope(patch, error, error_size);
	if (rc != 0)
		return rc;
	if (!mkdtemp(temp_dir))
		MORPH_RETURN_ERRNO();
	rc = file_path_join(stage_path, sizeof(stage_path), temp_dir,
		CONFIG_EDIT_PATCH_FILE);
	if (rc == 0)
		rc = file_write_all(stage_path, old_content, strlen(old_content));
	if (rc == 0 && chmod(stage_path, 0600) != 0)
		rc = -errno;
	if (rc == 0)
		rc = patch_apply(temp_dir, patch, &patch_result,
			error, error_size);
	change = patch_result.changes.nelts == 1 ?
		morph_array_get(&patch_result.changes, 0) : NULL;
	if (rc == 0 && (!change || change->action != PATCH_ACTION_UPDATE ||
			strcmp(change->path, CONFIG_EDIT_PATCH_FILE) != 0)) {
		snprintf(error, error_size,
			 "patch must only update %s", CONFIG_EDIT_PATCH_FILE);
		rc = -EACCES;
	}
	if (rc == 0) {
		*out = file_read_all(stage_path, NULL);
		if (!*out)
			rc = -EIO;
	}
	patch_result_cleanup(&patch_result);
	if (stage_path[0])
		(void)unlink(stage_path);
	(void)rmdir(temp_dir);
	return rc;
}

static int write_all_fd(int fd, const char *data, size_t len)
{
	size_t written = 0;

	while (written < len) {
		ssize_t count = write(fd, data + written, len - written);

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			MORPH_RETURN_ERRNO();
		if (count == 0)
			MORPH_RETURN(-EIO);
		written += (size_t)count;
	}
	return 0;
}

static int atomic_write_config(const char *path, const char *content,
			       mode_t mode)
{
	char parent_copy[PATH_MAX];
	char temporary[PATH_MAX];
	char *parent;
	int fd;
	int rc;

	if (!path || !content)
		MORPH_RETURN(-EINVAL);
	strncpy(parent_copy, path, sizeof(parent_copy) - 1);
	parent_copy[sizeof(parent_copy) - 1] = '\0';
	parent = dirname(parent_copy);
	if (snprintf(temporary, sizeof(temporary),
		     "%s/.morph-config-edit-XXXXXX", parent) >=
	    (int)sizeof(temporary))
		MORPH_RETURN(-ENAMETOOLONG);
	fd = mkstemp(temporary);
	if (fd < 0)
		MORPH_RETURN_ERRNO();
	rc = fchmod(fd, mode);
	if (rc == 0)
		rc = write_all_fd(fd, content, strlen(content));
	if (rc == 0 && fsync(fd) != 0)
		rc = -errno;
	if (close(fd) != 0 && rc == 0)
		rc = -errno;
	if (rc == 0 && rename(temporary, path) != 0)
		rc = -errno;
	if (rc != 0)
		(void)unlink(temporary);
	return rc;
}

static int content_unchanged(const char *path, int existed,
			     const char *expected)
{
	char *current = file_read_all(path, NULL);
	int unchanged;

	if (!existed) {
		if (current) {
			free(current);
			return 0;
		}
		return errno == ENOENT;
	}
	unchanged = current && strcmp(current, expected ? expected : "") == 0;
	free(current);
	return unchanged;
}

static int config_edit_exec(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	struct config_edit_context *ctx = user_data;
	cJSON *root = NULL;
	char *target_expanded = NULL;
	char *old_content = NULL;
	char *new_content = NULL;
	char *written_content = NULL;
	const char *path;
	const char *reason;
	const char *patch = NULL;
	char backup_path[PATH_MAX];
	char lock_path[PATH_MAX];
	char errbuf[256];
	struct stat target_stat;
	mode_t mode = 0600;
	int existed = 0;
	int lock_fd = -1;
	int rc = 0;

	errbuf[0] = '\0';

	if (!result)
		MORPH_RETURN(-EINVAL);
	if (!ctx || !ctx->config_path[0]) {
		(void)tool_result_error(result, "tool_failed",
					     "config_edit is not configured");
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}
	root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_error(result, "tool_failed", "invalid JSON");
		MORPH_RETURN(-EINVAL);
	}
	cJSON *path_item = cJSON_GetObjectItem(root, "path");
	cJSON *content_item = cJSON_GetObjectItem(root, "content");
	cJSON *patch_item = cJSON_GetObjectItem(root, "patch");
	cJSON *reason_item = cJSON_GetObjectItem(root, "reason");
	if (cJSON_IsString(content_item) && cJSON_IsString(patch_item)) {
		rc = tool_result_error(result, "tool_failed",
			"provide only one of 'patch' or 'content'");
		rc = rc != 0 ? rc : -EINVAL;
		goto out;
	}
	path = cJSON_IsString(path_item) && path_item->valuestring[0] ?
		path_item->valuestring : ctx->config_path;
	reason = cJSON_IsString(reason_item) ? reason_item->valuestring : "";
	if (!reason[0]) {
		rc = tool_result_error(result, "tool_failed", "missing 'reason' parameter");
		rc = rc != 0 ? rc : -EINVAL;
		goto out;
	}
	if (!path_equal_config(path, ctx->config_path)) {
		rc = tool_result_error(result, "tool_failed",
			"config_edit can only edit the active Morph config file");
		rc = rc != 0 ? rc : -EACCES;
		goto out;
	}
	target_expanded = file_expand_path(ctx->config_path);
	if (!target_expanded) {
		rc = -ENOMEM;
		goto out;
	}
	if (lstat(target_expanded, &target_stat) == 0) {
		if (S_ISLNK(target_stat.st_mode) ||
		    !S_ISREG(target_stat.st_mode)) {
			rc = tool_result_error(result, "tool_failed",
				"active config must be a regular file, not a symlink");
			rc = rc != 0 ? rc : -ELOOP;
			goto out;
		}
		if (target_stat.st_size > (off_t)CONFIG_EDIT_MAX_SIZE) {
			rc = tool_result_error(result, "tool_failed",
				"active config is too large to edit safely");
			rc = rc != 0 ? rc : -EFBIG;
			goto out;
		}
		existed = 1;
		mode = target_stat.st_mode & 0777;
		old_content = file_read_all(target_expanded, NULL);
		if (!old_content) {
			rc = -errno;
			goto out;
		}
	} else if (errno != ENOENT) {
		rc = -errno;
		goto out;
	} else {
		old_content = strdup("");
		if (!old_content) {
			rc = -ENOMEM;
			goto out;
		}
	}
	if (cJSON_IsString(content_item) && content_item->valuestring) {
		if (strlen(content_item->valuestring) > CONFIG_EDIT_MAX_SIZE) {
			rc = tool_result_error(result, "tool_failed", "content too large");
			rc = rc != 0 ? rc : -EFBIG;
			goto out;
		}
		new_content = strdup(content_item->valuestring);
		if (!new_content) {
			rc = -ENOMEM;
			goto out;
		}
	} else if (cJSON_IsString(patch_item) && patch_item->valuestring) {
		if (!existed) {
			rc = tool_result_error(result, "tool_failed",
				"patch mode requires an existing config file");
			rc = rc != 0 ? rc : -ENOENT;
			goto out;
		}
		patch = patch_item->valuestring;
		rc = build_content_from_patch(old_content, patch, &new_content,
			errbuf, sizeof(errbuf));
		if (rc != 0) {
			(void)tool_result_errorf(result, "tool_failed",
				"invalid config patch: %s",
				errbuf[0] ? errbuf : morph_strerror(rc));
			goto out;
		}
	} else {
		rc = tool_result_error(result, "tool_failed",
			"provide 'patch' or complete 'content'");
		rc = rc != 0 ? rc : -EINVAL;
		goto out;
	}
	if (strlen(new_content) > CONFIG_EDIT_MAX_SIZE) {
		rc = tool_result_error(result, "tool_failed", "content too large");
		rc = rc != 0 ? rc : -EFBIG;
		goto out;
	}
	rc = validate_config_content(new_content, errbuf, sizeof(errbuf));
	if (rc < 0) {
		(void)tool_result_errorf(result, "tool_failed",
					      "invalid configuration: %s", errbuf);
		goto out;
	}
	if (strcmp(old_content, new_content) == 0) {
		cJSON *data = cJSON_CreateObject();

		if (!data) {
			rc = -ENOMEM;
			goto out;
		}
		cJSON_AddStringToObject(data, "path", target_expanded);
		cJSON_AddBoolToObject(data, "changed", 0);
		cJSON_AddBoolToObject(data, "validated", 1);
		cJSON_AddBoolToObject(data, "restart_required", 0);
		rc = tool_result_success(result, data);
		goto out;
	}
	if (!ctx->tctx) {
		rc = tool_result_error(result, "tool_failed",
			"config_edit requires an approval context");
		rc = rc != 0 ? rc : -EACCES;
		goto out;
	}
	{
		struct tool_operation op = {
			.kind = TOOL_OP_PATH_WRITE,
			.tool_name = "config_edit",
			.action = reason,
			.target = target_expanded,
			.scope = "active Morph config file",
			.details_json = patch ? args_json : NULL,
		};
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			(void)tool_result_error(result, "tool_failed",
				"config edit denied or not approved");
			goto out;
		}
	}
	rc = ensure_parent_dir(target_expanded);
	if (rc < 0)
		goto out;
	if (snprintf(lock_path, sizeof(lock_path), "%s.lock",
		     target_expanded) >= (int)sizeof(lock_path) ||
	    snprintf(backup_path, sizeof(backup_path), "%s.bak",
		     target_expanded) >= (int)sizeof(backup_path)) {
		rc = -ENAMETOOLONG;
		goto out;
	}
	lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
	if (lock_fd < 0) {
		rc = -errno;
		goto out;
	}
	if (flock(lock_fd, LOCK_EX) != 0) {
		rc = -errno;
		goto out;
	}
	if (!content_unchanged(target_expanded, existed, old_content)) {
		rc = tool_result_error(result, "tool_failed",
			"config changed during approval; inspect it and retry");
		rc = rc != 0 ? rc : -EAGAIN;
		goto out;
	}
	if (existed) {
		rc = atomic_write_config(backup_path, old_content, mode);
		if (rc != 0)
			goto out;
	}
	rc = atomic_write_config(target_expanded, new_content, mode);
	if (rc != 0)
		goto out;
	written_content = file_read_all(target_expanded, NULL);
	if (!written_content ||
	    validate_config_content(written_content, errbuf,
				    sizeof(errbuf)) != 0) {
		int failure = written_content ? MORPH_ERR_CONFIG : -EIO;
		int rollback_rc;

		if (existed)
			rollback_rc = atomic_write_config(target_expanded,
				old_content, mode);
		else
			rollback_rc = unlink(target_expanded) == 0 ? 0 : -errno;
		if (rollback_rc != 0) {
			(void)tool_result_errorf(result, "tool_failed",
				"config verification and rollback failed: %s",
				morph_strerror(rollback_rc));
			rc = rollback_rc;
		} else {
			(void)tool_result_error(result, "tool_failed",
				"written config failed verification and was rolled back");
			rc = failure;
		}
		goto out;
	}
	{
		cJSON *out_json = cJSON_CreateObject();
		char *out_str;
		if (!out_json) {
			rc = -ENOMEM;
			goto out;
		}
		cJSON_AddStringToObject(out_json, "path", target_expanded);
		cJSON_AddBoolToObject(out_json, "changed", 1);
		cJSON_AddBoolToObject(out_json, "validated", 1);
		cJSON_AddBoolToObject(out_json, "atomic", 1);
		cJSON_AddBoolToObject(out_json, "backup_created", existed);
		if (existed)
			cJSON_AddStringToObject(out_json, "backup_path", backup_path);
		cJSON_AddBoolToObject(out_json, "hot_reloaded", 0);
		cJSON_AddBoolToObject(out_json, "restart_required", 1);
		cJSON_AddStringToObject(out_json, "message",
			"configuration edited safely; restart Morph to apply it");
		out_str = cJSON_PrintUnformatted(out_json);
		cJSON_Delete(out_json);
		(void)tool_result_success_json_text(result, out_str);
	}

out:
	if (lock_fd >= 0) {
		(void)flock(lock_fd, LOCK_UN);
		(void)close(lock_fd);
	}
	free(target_expanded);
	free(old_content);
	free(new_content);
	free(written_content);
	cJSON_Delete(root);
	return rc;
}

int config_edit_init(struct tool_registry *reg, struct tool_context *tctx,
		     const char *config_path)
{
	struct config_edit_context *ctx;
	struct tool_spec spec = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "config_edit",
		.description =
			"Safely edit the active Morph config after user approval. "
			"Prefer patch mode to preserve comments and formatting. The "
			"patch must use the Codex patch envelope and update only "
			"config.toml. The result is schema-validated, backed up, and "
			"atomically replaced. A Morph restart is required. Prefer "
			"api_key_env over API key values.",
		.input_schema =
			"{\"type\":\"object\",\"properties\":{"
			"\"path\":{\"type\":\"string\",\"description\":"
			"\"Optional config path; must match the active Morph config\"},"
			"\"reason\":{\"type\":\"string\",\"description\":"
			"\"Human-readable reason shown for approval\"},"
			"\"patch\":{\"type\":\"string\",\"description\":"
			"\"Codex patch updating only config.toml\"},"
			"\"content\":{\"type\":\"string\",\"description\":"
			"\"Complete TOML fallback for creating a config\"}},"
			"\"required\":[\"reason\"],"
			"\"oneOf\":[{\"required\":[\"patch\"]},"
			"{\"required\":[\"content\"]}],"
			"\"additionalProperties\":false}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = config_edit_exec,
		.user_data_destroy = config_edit_context_destroy
	};
	int rc;

	if (!reg || !config_path || !config_path[0])
		MORPH_RETURN(-EINVAL);
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		MORPH_RETURN(-ENOMEM);
	ctx->tctx = tctx;
	strncpy(ctx->config_path, config_path, sizeof(ctx->config_path) - 1);
	spec.user_data = ctx;
	rc = tool_register(reg, &spec);
	if (rc < 0) {
		free(ctx);
		return rc;
	}
	{
		struct tool_entry *e = tool_lookup(reg, "config_edit");
		if (e)
			e->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	}
	return 0;
}

int config_write_init(struct tool_registry *reg, struct tool_context *tctx,
		      const char *config_path)
{
	return config_edit_init(reg, tctx, config_path);
}
