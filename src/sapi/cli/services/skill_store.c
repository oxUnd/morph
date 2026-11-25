#include "sapi/cli/services/skill_store.h"
#include <nb/cleanup.h>
#include "cJSON.h"
#include "sapi/cli/services/management.h"
#include "skill/skill_parse.h"
#include "errors.h"
#include <nb/file.h>
#include "blake3.h"
#include <nb/encoding.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLI_SKILL_STORE_TREE_DEPTH_MAX 32

static int cli_skill_store_scan_directory(struct nb_log *logger, const char *path,
					  nb_vec *candidates, int depth)
{
	nb_buf skill_manifest_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int skill_manifest_path_status = NB_OK;
	nb_buf child_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int child_path_status = NB_OK;

	char skill_md[PATH_MAX];
	struct stat st;
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int rc;

	if (depth > CLI_SKILL_STORE_TREE_DEPTH_MAX)
		return NB_ERR_LOOP;

	rc = ((skill_manifest_path_status =
		       nb_path_concat(&skill_manifest_path_buffer, path, "SKILL.md")) != NB_OK
		      ? skill_manifest_path_status
	      : (skill_manifest_path_status =
			 nb_str_copy_to(nb_buf_str(&skill_manifest_path_buffer), skill_md,
					sizeof(skill_md))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : skill_manifest_path_status);
	if (rc != 0)
		return rc;

	if (lstat(skill_md, &st) == 0) {
		struct cli_skill_candidate candidate = {0};
		char *body = NULL;
		struct cli_skill_candidate *slot;

		if (!S_ISREG(st.st_mode))
			return NB_ERR_INVAL;

		rc = skill_parse_file(logger, skill_md, &candidate.fm, &body);
		free(body);
		if (rc != 0)
			return rc;

		if (!cli_management_name_valid(candidate.fm.name) || !candidate.fm.description[0]) {
			fprintf(stderr, "morph: skill needs a valid name and description: %s\n",
				skill_md);
			return NB_ERR_INVAL;
		}

		rc = cli_management_copy_arg(candidate.path, sizeof(candidate.path), path);
		if (rc != 0)
			return rc;

		slot = nb_vec_push(candidates);
		if (!slot)
			return NB_ERR_NOMEM;

		*slot = candidate;
		return 0;
	}

	if (errno != ENOENT)
		return nb_status_from_errno(errno, NULL);

	rc = nb_dir_scan(path, NB_DIR_SCAN_ALL | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_SORT, &names,
			 NULL);
	if (rc != 0)
		return rc;

	for (size_t i = 0; rc == 0 && i < names.names.len; i++) {
		const char *name = nb_dir_listing_name(&names, i);
		char child[PATH_MAX];

		if (strcmp(name, ".git") == 0 || strncmp(name, ".morph-install-", 15) == 0)
			continue;

		rc = ((child_path_status = nb_path_concat(&child_path_buffer, path, name)) != NB_OK
			      ? child_path_status
		      : (child_path_status = nb_str_copy_to(nb_buf_str(&child_path_buffer), child,
							    sizeof(child))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : child_path_status);
		if (rc == 0 && lstat(child, &st) != 0)
			rc = (nb_status_from_errno(errno, NULL));

		if (rc == 0 && S_ISDIR(st.st_mode))
			rc = cli_skill_store_scan_directory(logger, child, candidates, depth + 1);
	}

	nb_dir_listing_destroy(&names);
	return rc;
}

int cli_skill_store_scan(struct nb_log *logger, const char *root, nb_vec *candidates)
{
	return cli_skill_store_scan_directory(logger, root, candidates, 0);
}

struct cli_skill_store_copy_policy {
	const char *root;
};

static nb_status cli_skill_store_copy_filter(const char *source, const char *destination,
				       const nb_file_copy_entry *entry, bool *include,
				       void *context, nb_err *err)
{
	struct cli_skill_store_copy_policy *policy = context;
	const char *name = nb_path_basename(source);
	(void)destination;

	if (entry->depth > 0 &&
	    (!strcmp(name, ".git") ||
	     (entry->depth == 1 && !strcmp(name, CLI_SKILL_STORE_INSTALL_METADATA)))) {
		*include = false;
		return NB_OK;
	}

	if (!entry->info.is_symlink)
		return NB_OK;

	if (!entry->symlink_target || nb_path_is_absolute(entry->symlink_target) || !entry->depth)
		return NB_ERR_PERMISSION;

	nb_buf target NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf resolved NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = nb_path_dirname(&target, source);
	if (rc == 0)
		rc = nb_path_join(&target, nb_buf_cstr(&target), entry->symlink_target);

	if (rc == 0)
		rc = nb_path_canonical(&resolved, nb_buf_cstr(&target), err);

	if (rc != 0)
		return rc == NB_ERR_NOMEM ? NB_ERR_NOMEM : NB_ERR_PERMISSION;

	return nb_path_is_within(nb_buf_cstr(&resolved), policy->root, true) ? NB_OK
									     : NB_ERR_PERMISSION;
}

int cli_skill_store_copy(const char *source, const char *destination)
{
	nb_buf root NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = nb_path_canonical(&root, source, NULL);

	if (rc != 0)
		return rc;

	struct cli_skill_store_copy_policy policy = {.root = nb_buf_cstr(&root)};
	nb_file_copy_options options = {.recursive = true,
					.preserve_mode = true,
					.copy_symlinks = true,
					.directory_mode = 0700,
					.max_depth = CLI_SKILL_STORE_TREE_DEPTH_MAX,
					.filter = cli_skill_store_copy_filter,
					.filter_context = &policy};
	return nb_file_copy(source, destination, &options, NULL);
}

int cli_skill_store_remove(const char *path)
{
	return nb_file_remove_all(path, 128, NULL);
}

static int cli_skill_store_digest_tree(blake3_hasher *hasher, const char *path, int depth)
{
	nb_buf child_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int child_path_status = NB_OK;

	struct stat st;
	nb_dir_listing names NB_CLEANUP(nb_dir_listing_destroy) = {0};
	int rc;
	char buffer[BUFSIZ];

	if (depth > CLI_SKILL_STORE_TREE_DEPTH_MAX)
		return NB_ERR_LOOP;

	if (lstat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);

	if (S_ISREG(st.st_mode)) {
		int fd = open(path, O_RDONLY | O_NOFOLLOW);
		const char *kind = st.st_mode & 0111 ? "executable" : "file";

		if (fd < 0)
			return nb_status_from_errno(errno, NULL);

		blake3_hasher_update(hasher, kind, strlen(kind) + 1);
		/* Include length so file content cannot mimic tree boundaries. */
		int length = snprintf(buffer, sizeof(buffer), "%lld:", (long long)st.st_size);

		blake3_hasher_update(hasher, buffer, (size_t)length);
		rc = 0;
		for (;;) {
			ssize_t count = read(fd, buffer, sizeof(buffer));

			if (count < 0 && errno == EINTR)
				continue;

			if (count < 0) {
				rc = (nb_status_from_errno(errno, NULL));
				break;
			}

			if (count == 0)
				break;

			blake3_hasher_update(hasher, buffer, (size_t)count);
		}

		close(fd);
		return rc;
	}

	if (S_ISLNK(st.st_mode)) {
		ssize_t count = readlink(path, buffer, sizeof(buffer) - 1);

		if (count < 0)
			return nb_status_from_errno(errno, NULL);

		buffer[count] = '\0';
		blake3_hasher_update(hasher, "link", 5);
		blake3_hasher_update(hasher, buffer, (size_t)count + 1);
		return 0;
	}

	if (!S_ISDIR(st.st_mode))
		return NB_ERR_UNSUPPORTED;

	blake3_hasher_update(hasher, "directory", 10);
	rc = nb_dir_scan(path, NB_DIR_SCAN_ALL | NB_DIR_SCAN_HIDDEN | NB_DIR_SCAN_SORT, &names,
			 NULL);
	if (rc != 0)
		return rc;

	for (size_t i = 0; rc == 0 && i < names.names.len; i++) {
		const char *name = nb_dir_listing_name(&names, i);
		char child[PATH_MAX];

		if (depth == 0 && strcmp(name, CLI_SKILL_STORE_INSTALL_METADATA) == 0)
			continue;

		blake3_hasher_update(hasher, name, strlen(name) + 1);
		rc = ((child_path_status = nb_path_concat(&child_path_buffer, path, name)) != NB_OK
			      ? child_path_status
		      : (child_path_status = nb_str_copy_to(nb_buf_str(&child_path_buffer), child,
							    sizeof(child))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : child_path_status);
		if (rc == 0)
			rc = cli_skill_store_digest_tree(hasher, child, depth + 1);
	}

	nb_dir_listing_destroy(&names);
	blake3_hasher_update(hasher, "end", 4);
	return rc;
}

int cli_skill_store_digest(const char *path, char digest[CLI_SKILL_STORE_DIGEST_MAX])
{
	blake3_hasher hasher;
	unsigned char hash[BLAKE3_OUT_LEN];
	int rc;

	blake3_hasher_init(&hasher);
	rc = cli_skill_store_digest_tree(&hasher, path, 0);
	if (rc != 0)
		return rc;

	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	(void)nb_hex_encode_to(hash, sizeof(hash), false, digest, CLI_SKILL_STORE_DIGEST_MAX, NULL);
	return 0;
}

cJSON *cli_skill_store_metadata_copy(const char *path)
{
	nb_buf manifest_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int manifest_path_status = NB_OK;
	nb_buf manifest_contents_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	char *manifest_contents_copy NB_CLEANUP(nb_charp_cleanup) = NULL;

	char file[PATH_MAX];
	char *text;
	cJSON *metadata;

	if (((manifest_path_status = nb_path_concat(&manifest_path_buffer, path,
						    CLI_SKILL_STORE_INSTALL_METADATA)) != NB_OK
		     ? manifest_path_status
	     : (manifest_path_status = nb_str_copy_to(nb_buf_str(&manifest_path_buffer), file,
						      sizeof(file))) == NB_ERR_RANGE
		     ? NB_ERR_NAME_TOO_LONG
		     : manifest_path_status) != 0)
		return NULL;

	text = (nb_file_read_all(file, &manifest_contents_buffer, 0, NULL) == NB_OK &&
				(manifest_contents_copy = (char *)malloc(
					 nb_buf_len(&manifest_contents_buffer) + 1)) != NULL
			? (memcpy(manifest_contents_copy, nb_buf_cstr(&manifest_contents_buffer),
				  nb_buf_len(&manifest_contents_buffer) + 1),
			   nb_charp_steal(&manifest_contents_copy))
			: NULL);
	if (!text)
		return NULL;

	metadata = cJSON_Parse(text);
	free(text);
	return metadata;
}

int cli_skill_store_unmodified(const char *path, const cJSON *metadata)
{
	char digest[CLI_SKILL_STORE_DIGEST_MAX];
	cJSON *expected = cJSON_GetObjectItemCaseSensitive(metadata, "digest");
	int rc;

	if (!cJSON_IsString(expected)) {
		fprintf(stderr, "morph: unmanaged skill, refusing replacement/removal: %s\n", path);
		return NB_ERR_NOT_PERMITTED;
	}

	rc = cli_skill_store_digest(path, digest);
	if (rc != 0)
		return rc;

	if (strcmp(digest, expected->valuestring) != 0) {
		fprintf(stderr,
			"morph: skill has local changes; "
			"preserve them before retrying: %s\n",
			path);
		return NB_ERR_BUSY;
	}

	return 0;
}

int cli_skill_store_install(const char *root, const struct cli_skill_candidate *candidate,
			    cJSON *metadata, int replace)
{
	nb_buf target_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int target_path_status = NB_OK;
	nb_buf temporary_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf stage_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int stage_path_status = NB_OK;
	nb_buf backup_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int backup_path_status = NB_OK;
	nb_buf metadata_path_buffer NB_CLEANUP(nb_buf_destroy) = {0};
	int metadata_path_status = NB_OK;

	char temporary[PATH_MAX], stage[PATH_MAX], target[PATH_MAX];
	char backup[PATH_MAX], metadata_path[PATH_MAX];
	char digest[CLI_SKILL_STORE_DIGEST_MAX];
	struct stat st;
	char *json = NULL;
	int exists;
	int moved = 0;
	int rc;

	rc = ((target_path_status =
		       nb_path_concat(&target_path_buffer, root, candidate->fm.name)) != NB_OK
		      ? target_path_status
	      : (target_path_status = nb_str_copy_to(nb_buf_str(&target_path_buffer), target,
						     sizeof(target))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : target_path_status);
	if (rc != 0)
		return rc;

	exists = lstat(target, &st) == 0;
	if (!exists && errno != ENOENT)
		return nb_status_from_errno(errno, NULL);

	if (exists) {
		cJSON *previous;

		if (!replace)
			return NB_ERR_EXISTS;

		if (!S_ISDIR(st.st_mode))
			return NB_ERR_LOOP;

		previous = cli_skill_store_metadata_copy(target);
		rc = cli_skill_store_unmodified(target, previous);
		cJSON_Delete(previous);
		if (rc != 0)
			return rc;
	}

	rc = nb_file_temp_dir(&temporary_path_buffer, root, ".morph-install-", NULL);
	if (rc == NB_OK)
		rc = nb_str_copy_to(nb_buf_str(&temporary_path_buffer), temporary,
				    sizeof(temporary));

	if (rc != NB_OK) {
		if (!nb_buf_empty(&temporary_path_buffer))
			(void)nb_file_remove_all(nb_buf_cstr(&temporary_path_buffer), 0, NULL);

		return rc == NB_ERR_RANGE ? NB_ERR_NAME_TOO_LONG : rc;
	}

	rc = ((stage_path_status = nb_path_concat(&stage_path_buffer, temporary, "skill")) != NB_OK
		      ? stage_path_status
	      : (stage_path_status = nb_str_copy_to(nb_buf_str(&stage_path_buffer), stage,
						    sizeof(stage))) == NB_ERR_RANGE
		      ? NB_ERR_NAME_TOO_LONG
		      : stage_path_status);
	if (rc == 0)
		rc = ((backup_path_status =
			       nb_path_concat(&backup_path_buffer, temporary, "previous")) != NB_OK
			      ? backup_path_status
		      : (backup_path_status = nb_str_copy_to(nb_buf_str(&backup_path_buffer),
							     backup, sizeof(backup))) ==
				      NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : backup_path_status);

	if (rc == 0)
		rc = cli_skill_store_copy(candidate->path, stage);

	if (rc == 0)
		rc = cli_skill_store_digest(stage, digest);

	if (rc == 0 && !cJSON_AddStringToObject(metadata, "digest", digest))
		rc = NB_ERR_NOMEM;

	if (rc == 0) {
		json = cJSON_PrintUnformatted(metadata);
		if (!json)
			rc = NB_ERR_NOMEM;
	}

	if (rc == 0)
		rc = ((metadata_path_status = nb_path_concat(&metadata_path_buffer, stage,
							     CLI_SKILL_STORE_INSTALL_METADATA)) !=
				      NB_OK
			      ? metadata_path_status
		      : (metadata_path_status =
				 nb_str_copy_to(nb_buf_str(&metadata_path_buffer), metadata_path,
						sizeof(metadata_path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : metadata_path_status);

	if (rc == 0)
		rc = nb_file_write_all_atomic(
			metadata_path, json, strlen(json),
			&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);

	if (rc == 0 && exists) {
		if (rename(target, backup) != 0)
			rc = (nb_status_from_errno(errno, NULL));
		else
			moved = 1;
	}

	if (rc == 0 && rename(stage, target) != 0)
		rc = (nb_status_from_errno(errno, NULL));

	if (rc != 0 && moved && rename(backup, target) != 0) {
		fprintf(stderr, "morph: previous skill preserved at %s\n", backup);
		free(json);
		return nb_status_from_errno(errno, NULL);
	}

	free(json);
	int cleanup_rc = cli_skill_store_remove(temporary);

	return rc != 0 ? rc : cleanup_rc;
}
