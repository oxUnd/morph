#include "config_write.h"
#include "nb/str.h"
#include "nb/utf8.h"
#include "cJSON.h"
#include "nb/cleanup.h"
#include "agent/tool_context.h"
#include "agent/patch.h"
#include "nb/file.h"
#include "errors.h"
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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};

	char *a;
	char *b;
	int same;

	if (!path || !config_path)
		return 0;
	a = (nb_path_expand(&file_value_1, path, NULL) == NB_OK ? strdup(nb_buf_cstr(&file_value_1))
								: NULL);
	b = (nb_path_expand(&file_value_2, config_path, NULL) == NB_OK
		     ? strdup(nb_buf_cstr(&file_value_2))
		     : NULL);
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
		return NB_ERR_INVAL;
	if (errbuf_size > 0)
		errbuf[0] = '\0';
	rc = config_validate_text(content, &error);
	if (rc != NB_OK && errbuf_size > 0) {
		nb_buf message NB_CLEANUP(nb_buf_cleanup) = {0};
		if (error.path[0])
			(void)nb_buf_printf(&message, "%s: %s", error.path, error.message);
		else
			(void)nb_buf_append_cstr(&message, error.message);
		(void)nb_str_copy_to(nb_str_from_parts(nb_buf_cstr(&message),
						       nb_utf8_clamp_bytes(nb_buf_cstr(&message),
									   errbuf_size - 1)),
				     errbuf, errbuf_size);
	}

	return rc;
}

static int ensure_parent_dir(const char *path)
{
	char tmp[PATH_MAX];
	char *dir;

	if (!path)
		return NB_ERR_INVAL;
	(void)nb_str_copy_to(nb_str_from_parts(path, nb_utf8_clamp_bytes(path, sizeof(tmp) - 1)),
			     tmp, (sizeof(tmp) - 1) + 1);
	tmp[sizeof(tmp) - 1] = '\0';
	dir = dirname(tmp);
	if (!dir || !*dir)
		return 0;
	return nb_file_mkdir(dir, true, NULL);
}

static int validate_patch_scope(const char *patch, char *error,
				size_t error_size)
{
	char *copy;
	char *save = NULL;
	char *line;
	int updates = 0;

	if (!patch || strlen(patch) > CONFIG_EDIT_MAX_SIZE)
		return NB_ERR_TOO_BIG;
	copy = strdup(patch);
	if (!copy)
		return NB_ERR_NOMEM;
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
				return NB_ERR_PERMISSION;
			}
			updates++;
		} else if (strncmp(begin, "*** Add File: ", 14) == 0 ||
			   strncmp(begin, "*** Delete File: ", 17) == 0 ||
			   strncmp(begin, "*** Move to: ", 13) == 0) {
			snprintf(error, error_size,
				 "config_edit only accepts an update patch");
			free(copy);
			return NB_ERR_PERMISSION;
		}
	}
	free(copy);
	if (updates != 1) {
		snprintf(error, error_size,
			 "patch must contain exactly one update for %s",
			 CONFIG_EDIT_PATCH_FILE);
		return NB_ERR_INVAL;
	}
	return 0;
}

static int build_content_from_patch(const char *old_content,
				    const char *patch, char **out,
				    char *error, size_t error_size)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_3 = NB_OK;
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_4 NB_CLEANUP(nb_charp_cleanup) = NULL;

	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
	char stage_path[PATH_MAX];
	struct patch_result patch_result = {0};
	struct patch_change *change;
	int rc;

	if (!old_content || !out)
		return NB_ERR_INVAL;
	stage_path[0] = '\0';
	*out = NULL;
	rc = validate_patch_scope(patch, error, error_size);
	if (rc != 0)
		return rc;
	rc = nb_file_temp_dir(&directory, "/tmp", "morph-config-edit-", NULL);
	if (rc != NB_OK)
		return rc;
	const char *temp_dir = nb_buf_cstr(&directory);
	rc = ((file_status_3 = nb_path_concat(&file_value_3, temp_dir, CONFIG_EDIT_PATCH_FILE)) !=
			      NB_OK
		      ? file_status_3
	      : (file_status_3 = nb_str_copy_to(nb_buf_str(&file_value_3), stage_path,
						sizeof(stage_path))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : file_status_3);
	if (rc == 0)
		rc = nb_file_write_all(stage_path, old_content, strlen(old_content), NULL);
	if (rc == 0 && chmod(stage_path, 0600) != 0)
		rc = nb_status_from_errno(errno, NULL);
	if (rc == 0)
		rc = patch_apply(temp_dir, patch, &patch_result,
			error, error_size);
	change = patch_result.changes.len == 1 ? nb_vec_at(&patch_result.changes, 0) : NULL;
	if (rc == 0 && (!change || change->action != PATCH_ACTION_UPDATE ||
			strcmp(change->path, CONFIG_EDIT_PATCH_FILE) != 0)) {
		snprintf(error, error_size,
			 "patch must only update %s", CONFIG_EDIT_PATCH_FILE);
		rc = NB_ERR_PERMISSION;
	}
	if (rc == 0) {
		*out = (nb_file_read_all(stage_path, &file_value_4, 0, NULL) == NB_OK &&
					(file_copy_4 = (char *)malloc(nb_buf_len(&file_value_4) +
								      1)) != NULL
				? (memcpy(file_copy_4, nb_buf_cstr(&file_value_4),
					  nb_buf_len(&file_value_4) + 1),
				   nb_charp_steal(&file_copy_4))
				: NULL);
		if (!*out)
			rc = NB_ERR_IO;
	}
	patch_result_cleanup(&patch_result);
	if (stage_path[0])
		(void)unlink(stage_path);
	(void)rmdir(temp_dir);
	return rc;
}

static int content_unchanged(const char *path, int existed, const char *expected)
{
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_5 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char *current =
		(nb_file_read_all(path, &file_value_5, 0, NULL) == NB_OK &&
				 (file_copy_5 = (char *)malloc(nb_buf_len(&file_value_5) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_5, nb_buf_cstr(&file_value_5),
				   nb_buf_len(&file_value_5) + 1),
			    nb_charp_steal(&file_copy_5))
			 : NULL);
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

static void config_edit_lock_cleanup(int *fd)
{
	if (*fd < 0)
		return;
	(void)flock(*fd, LOCK_UN);
	(void)close(*fd);
	*fd = -1;
}

static int config_edit_exec(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {0};
	int read_status_7 = NB_OK;
	char *file_copy_7 NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf file_value_8 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_8 NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct config_edit_context *ctx = user_data;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	char *target_expanded NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *old_content NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *new_content NB_CLEANUP(nb_charp_cleanup) = NULL;
	char *written_content NB_CLEANUP(nb_charp_cleanup) = NULL;
	const char *path;
	const char *reason;
	const char *patch = NULL;
	char backup_path[PATH_MAX];
	char lock_path[PATH_MAX];
	char errbuf[256];
	struct stat target_stat;
	mode_t mode = 0600;
	int existed = 0;
	int lock_fd NB_CLEANUP(config_edit_lock_cleanup) = -1;
	int rc = 0;

	errbuf[0] = '\0';

	if (!result)
		return NB_ERR_INVAL;
	if (!ctx || !ctx->config_path[0]) {
		(void)tool_result_error_copy(result, "tool_failed",
					     "config_edit is not configured");
		return MORPH_ERR_NOT_CONFIGURED;
	}
	root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_error_copy(result, "tool_failed", "invalid JSON");
		return NB_ERR_INVAL;
	}
	cJSON *path_item = cJSON_GetObjectItem(root, "path");
	cJSON *content_item = cJSON_GetObjectItem(root, "content");
	cJSON *patch_item = cJSON_GetObjectItem(root, "patch");
	cJSON *reason_item = cJSON_GetObjectItem(root, "reason");
	if (cJSON_IsString(content_item) && cJSON_IsString(patch_item)) {
		rc = tool_result_error_copy(result, "tool_failed",
					    "provide only one of 'patch' or 'content'");
		rc = rc != 0 ? rc : NB_ERR_INVAL;
		return rc;
	}
	path = cJSON_IsString(path_item) && path_item->valuestring[0] ?
		path_item->valuestring : ctx->config_path;
	reason = cJSON_IsString(reason_item) ? reason_item->valuestring : "";
	if (!reason[0]) {
		rc = tool_result_error_copy(result, "tool_failed", "missing 'reason' parameter");
		rc = rc != 0 ? rc : NB_ERR_INVAL;
		return rc;
	}
	if (!path_equal_config(path, ctx->config_path)) {
		rc = tool_result_error_copy(
			result, "tool_failed",
			"config_edit can only edit the active Morph config file");
		rc = rc != 0 ? rc : NB_ERR_PERMISSION;
		return rc;
	}
	target_expanded = (nb_path_expand(&file_value_6, ctx->config_path, NULL) == NB_OK
				   ? strdup(nb_buf_cstr(&file_value_6))
				   : NULL);
	if (!target_expanded) {
		rc = NB_ERR_NOMEM;
		return rc;
	}
	if (lstat(target_expanded, &target_stat) == 0) {
		if (S_ISLNK(target_stat.st_mode) ||
		    !S_ISREG(target_stat.st_mode)) {
			rc = tool_result_error_copy(
				result, "tool_failed",
				"active config must be a regular file, not a symlink");
			rc = rc != 0 ? rc : NB_ERR_LOOP;
			return rc;
		}
		if (target_stat.st_size > (off_t)CONFIG_EDIT_MAX_SIZE) {
			rc = tool_result_error_copy(result, "tool_failed",
						    "active config is too large to edit safely");
			rc = rc != 0 ? rc : NB_ERR_TOO_BIG;
			return rc;
		}
		existed = 1;
		mode = target_stat.st_mode & 0777;
		old_content = ((read_status_7 = nb_file_read_all(target_expanded, &file_value_7, 0,
			NULL)) == NB_OK &&
					       (file_copy_7 = (char *)malloc(
							nb_buf_len(&file_value_7) + 1)) != NULL
				       ? (memcpy(file_copy_7, nb_buf_cstr(&file_value_7),
						 nb_buf_len(&file_value_7) + 1),
					  nb_charp_steal(&file_copy_7))
				       : NULL);
		if (!old_content) {
			rc = read_status_7 != NB_OK ? read_status_7 : NB_ERR_NOMEM;
			return rc;
		}
	} else if (errno != ENOENT) {
		rc = nb_status_from_errno(errno, NULL);
		return rc;
	} else {
		old_content = strdup("");
		if (!old_content) {
			rc = NB_ERR_NOMEM;
			return rc;
		}
	}
	if (cJSON_IsString(content_item) && content_item->valuestring) {
		if (strlen(content_item->valuestring) > CONFIG_EDIT_MAX_SIZE) {
			rc = tool_result_error_copy(result, "tool_failed", "content too large");
			rc = rc != 0 ? rc : NB_ERR_TOO_BIG;
			return rc;
		}
		new_content = strdup(content_item->valuestring);
		if (!new_content) {
			rc = NB_ERR_NOMEM;
			return rc;
		}
	} else if (cJSON_IsString(patch_item) && patch_item->valuestring) {
		if (!existed) {
			rc = tool_result_error_copy(result, "tool_failed",
						    "patch mode requires an existing config file");
			rc = rc != 0 ? rc : NB_ERR_NOT_FOUND;
			return rc;
		}
		patch = patch_item->valuestring;
		rc = build_content_from_patch(old_content, patch, &new_content,
			errbuf, sizeof(errbuf));
		if (rc != 0) {
			(void)tool_result_error_format(
				result, "tool_failed", "invalid config patch: %s",
				errbuf[0] ? errbuf
					  : nb_err_str(morph_error_descriptions(),
						       MORPH_ERROR_COUNT, rc));
			return rc;
		}
	} else {
		rc = tool_result_error_copy(result, "tool_failed",
					    "provide 'patch' or complete 'content'");
		rc = rc != 0 ? rc : NB_ERR_INVAL;
		return rc;
	}
	if (strlen(new_content) > CONFIG_EDIT_MAX_SIZE) {
		rc = tool_result_error_copy(result, "tool_failed", "content too large");
		rc = rc != 0 ? rc : NB_ERR_TOO_BIG;
		return rc;
	}
	rc = validate_config_content(new_content, errbuf, sizeof(errbuf));
	if (rc < 0) {
		(void)tool_result_error_format(result, "tool_failed", "invalid configuration: %s",
					       errbuf);
		return rc;
	}
	if (strcmp(old_content, new_content) == 0) {
		cJSON *data NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();

		if (!data) {
			rc = NB_ERR_NOMEM;
			return rc;
		}
		cJSON_AddStringToObject(data, "path", target_expanded);
		cJSON_AddBoolToObject(data, "changed", 0);
		cJSON_AddBoolToObject(data, "validated", 1);
		cJSON_AddBoolToObject(data, "restart_required", 0);
		rc = tool_result_success_take(result, &data);
		return rc;
	}
	if (!ctx->tctx) {
		rc = tool_result_error_copy(result, "tool_failed",
					    "config_edit requires an approval context");
		rc = rc != 0 ? rc : NB_ERR_PERMISSION;
		return rc;
	}
	{
		struct permission_policy policy;

		rc = tool_context_get_permission_policy(ctx->tctx, &policy);
		if (rc != 0)
			return rc;
		struct tool_operation op = {
			.kind = TOOL_OP_PATH_WRITE,
			.tool_name = "config_edit",
			.require_user = policy.access != PERMISSION_FULL,
			.action = reason,
			.target = target_expanded,
			.scope = "active Morph config file",
			.details_json = patch ? args_json : NULL,
		};
		rc = tool_context_check_operation(ctx->tctx, &op);
		if (rc < 0) {
			(void)tool_result_error_copy(result, "tool_failed",
						     "config edit denied or not approved");
			return rc;
		}
	}
	rc = ensure_parent_dir(target_expanded);
	if (rc < 0)
		return rc;
	if (snprintf(lock_path, sizeof(lock_path), "%s.lock",
		     target_expanded) >= (int)sizeof(lock_path) ||
	    snprintf(backup_path, sizeof(backup_path), "%s.bak",
		     target_expanded) >= (int)sizeof(backup_path)) {
		rc = NB_ERR_NAME_TOO_LONG;
		return rc;
	}
	lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
	if (lock_fd < 0) {
		rc = nb_status_from_errno(errno, NULL);
		return rc;
	}
	if (flock(lock_fd, LOCK_EX) != 0) {
		rc = nb_status_from_errno(errno, NULL);
		return rc;
	}
	if (!content_unchanged(target_expanded, existed, old_content)) {
		rc = tool_result_error_copy(result, "tool_failed",
					    "config changed during approval; inspect it and retry");
		rc = rc != 0 ? rc : NB_ERR_AGAIN;
		return rc;
	}
	if (existed) {
		rc = nb_file_write_all_atomic(
			backup_path, old_content, strlen(old_content),
			&(nb_file_write_options){.mode = mode, .sync_dir = true}, NULL);
		if (rc != 0)
			return rc;
	}
	rc = nb_file_write_all_atomic(target_expanded, new_content, strlen(new_content),
				      &(nb_file_write_options){.mode = mode, .sync_dir = true},
				      NULL);
	if (rc != 0)
		return rc;
	written_content =
		(nb_file_read_all(target_expanded, &file_value_8, 0, NULL) == NB_OK &&
				 (file_copy_8 = (char *)malloc(nb_buf_len(&file_value_8) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_8, nb_buf_cstr(&file_value_8),
				   nb_buf_len(&file_value_8) + 1),
			    nb_charp_steal(&file_copy_8))
			 : NULL);
	if (!written_content ||
	    validate_config_content(written_content, errbuf,
				    sizeof(errbuf)) != 0) {
		int failure = written_content ? MORPH_ERR_CONFIG : NB_ERR_IO;
		int rollback_rc;

		if (existed)
			rollback_rc = nb_file_write_all_atomic(
				target_expanded, old_content, strlen(old_content),
				&(nb_file_write_options){.mode = mode, .sync_dir = true}, NULL);
		else
			rollback_rc = unlink(target_expanded) == 0
					      ? 0
					      : nb_status_from_errno(errno, NULL);
		if (rollback_rc != 0) {
			(void)tool_result_error_format(
				result, "tool_failed",
				"config verification and rollback failed: %s",
				nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT,
					   rollback_rc));
			rc = rollback_rc;
		} else {
			(void)tool_result_error_copy(
				result, "tool_failed",
				"written config failed verification and was rolled back");
			rc = failure;
		}
		return rc;
	}
	{
		cJSON *out_json = cJSON_CreateObject();
		char *out_str NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
		if (!out_json) {
			rc = NB_ERR_NOMEM;
			return rc;
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
		(void)tool_result_success_json_text_copy(result, out_str);
	}

	return rc;
}

int config_edit_init(struct tool_registry *reg, struct tool_context *tctx,
		     const char *config_path)
{
	struct config_edit_context *ctx;
	struct tool_spec spec NB_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "config_edit",
		.description = "Safely edit the active Morph config. Requires user approval "
			       "unless access is full. "
			       "Prefer patch mode to preserve comments and formatting. The "
			       "patch must use the Codex patch envelope and update only "
			       "config.toml. The result is schema-validated, backed up, and "
			       "atomically replaced. A Morph restart is required. Prefer "
			       "api_key_env over API key values.",
		.input_schema = "{\"type\":\"object\",\"properties\":{"
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
		.user_data_destroy = config_edit_context_destroy};
	int rc;

	if (!reg || !config_path || !config_path[0])
		return NB_ERR_INVAL;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NB_ERR_NOMEM;
	ctx->tctx = tctx;
	(void)nb_str_copy_to(
		nb_str_from_parts(config_path,
				  nb_utf8_clamp_bytes(config_path, sizeof(ctx->config_path) - 1)),
		ctx->config_path, (sizeof(ctx->config_path) - 1) + 1);
	spec.user_data = ctx;
	rc = tool_register_take(reg, &spec);
	if (rc < 0)
		return rc;
	{
		struct tool_entry *e = tool_borrow(reg, "config_edit");
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
