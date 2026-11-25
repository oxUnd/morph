#include "runtime/runtime_internal.h"
#include <nb/str.h>
#include <nb/cleanup.h>
#include "runtime/sync.h"
#include <nb/buf.h>
#include "errors.h"
#include <nb/file.h>
#include <nb/random.h>
#include <nb/utf8.h>

#include <limits.h>
#include <string.h>
#include <unistd.h>

int runtime_session_import_file(struct runtime *runtime, const char *path,
	const char *origin, struct runtime_session_import_result *result)
{
	if (!runtime || !path || !origin || !result)
		return NB_ERR_INVAL;

	memset(result, 0, sizeof(*result));
	if (!nb_file_exists(path))
		return NB_ERR_NOT_FOUND;

	int rc = nb_mutex_trylock(runtime->context.execution_lock);

	if (rc != 0)
		return rc;

	rc = orm_session_import_file(&runtime->context.database, path, origin, result);
	nb_mutex_unlock(runtime->context.execution_lock);
	return rc;
}

int runtime_sync_import_backup(struct runtime *runtime,
	const struct morph_sync_config *cfg, const char *snapshot_id,
	struct runtime_session_import_result *result)
{
	nb_buf import_dir_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int import_dir_status = NB_OK;
	nb_buf import_token_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int import_token_status = NB_OK;
	nb_buf import_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int import_path_status = NB_OK;

	struct morph_sync_backup *backups = NULL;
	char origin[MORPH_SYNC_DEVICE_ID_MAX] = { 0 };
	char directory[PATH_MAX];
	char path[PATH_MAX];
	char token[64];
	int count = 0;
	int rc;

	if (!runtime || !cfg || !snapshot_id || !result)
		return NB_ERR_INVAL;
	memset(result, 0, sizeof(*result));
	rc = morph_sync_backups(cfg, "data.db", &backups, &count);
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		if (strcmp(backups[i].snapshot_id, snapshot_id) == 0)
			(void)nb_str_copy_to(
				nb_str_from_parts(backups[i].device_id,
						  nb_utf8_clamp_bytes(backups[i].device_id,
								      sizeof(origin) - 1)),
				origin, (sizeof(origin) - 1) + 1);
	}
	morph_sync_backups_free(backups);
	if (!origin[0])
		return NB_ERR_NOT_FOUND;
	rc = ((import_dir_status = nb_path_concat(&import_dir_buffer, cfg->sync_dir,
						  ".morph-sync/import")) != NB_OK
		      ? import_dir_status
	      : (import_dir_status = nb_str_copy_to(nb_buf_str(&import_dir_buffer), directory,
						    sizeof(directory))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : import_dir_status);
	if (rc == 0)
		rc = nb_file_mkdir(directory, true, NULL);
	if (rc == 0)
		rc = ((import_token_status =
			       ((16) == 0 || (16) > 16 ? NB_ERR_INVAL
				: !(nb_buf_clear(&import_token_buffer),
				    nb_buf_append_cstr(&import_token_buffer, "session_"))
					? NB_ERR_NOMEM
					: nb_random_hex(&import_token_buffer, 16, NULL))) != NB_OK
			      ? import_token_status
		      : (import_token_status = nb_str_copy_to(nb_buf_str(&import_token_buffer),
							      token, sizeof(token))) == NB_ERR_RANGE
			      ? NB_ERR_NO_SPACE
			      : import_token_status);
	if (rc == 0)
		rc = ((import_path_status =
			       nb_path_concat(&import_path_buffer, directory, token)) != NB_OK
			      ? import_path_status
		      : (import_path_status = nb_str_copy_to(nb_buf_str(&import_path_buffer), path,
							     sizeof(path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : import_path_status);
	if (rc != 0)
		return rc;
	rc = morph_sync_restore_db(cfg, snapshot_id, path);
	if (rc == 0)
		rc = runtime_session_import_file(runtime, path, origin, result);
	(void)unlink(path);
	return rc;
}
