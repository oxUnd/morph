#include "sapi/cli/skill_store.h"
#include "sapi/cli/manage.h"
#include "skill/skill_parse.h"
#include "util/error.h"
#include "util/file.h"
#include "blake3.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SKILL_TREE_DEPTH_MAX 32

static int name_compare(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void names_cleanup(morph_array_t *names)
{
	for (size_t i = 0; i < names->nelts; i++)
		free(*(char **)morph_array_get(names, i));
	morph_array_cleanup(names);
}

static int directory_names(const char *path, morph_array_t *names)
{
	DIR *dir;
	struct dirent *entry;
	int rc = morph_array_init(names, 16, sizeof(char *));

	if (rc != 0)
		return rc;
	dir = opendir(path);
	if (!dir) {
		MORPH_SET_ERR(rc, -errno);
		names_cleanup(names);
		return rc;
	}
	errno = 0;
	while ((entry = readdir(dir)) != NULL) {
		char **slot;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		slot = morph_array_push(names);
		if (!slot) {
			MORPH_SET_ERR(rc, -ENOMEM);
			break;
		}
		*slot = strdup(entry->d_name);
		if (!*slot) {
			MORPH_SET_ERR(rc, -ENOMEM);
			break;
		}
		errno = 0;
	}
	if (rc == 0 && errno != 0)
		MORPH_SET_ERR(rc, -errno);
	closedir(dir);
	if (rc != 0) {
		names_cleanup(names);
		return rc;
	}
	qsort(names->elts, names->nelts, names->size, name_compare);
	return 0;
}

static int scan_directory(struct morph_log *logger, const char *path, morph_array_t *candidates,
			  int depth)
{
	char skill_md[PATH_MAX];
	struct stat st;
	morph_array_t names = {0};
	int rc;

	if (depth > SKILL_TREE_DEPTH_MAX)
		MORPH_RETURN(-ELOOP);
	rc = file_path_join(skill_md, sizeof(skill_md), path, "SKILL.md");
	if (rc != 0)
		return rc;
	if (lstat(skill_md, &st) == 0) {
		struct skill_candidate candidate = {0};
		char *body = NULL;
		struct skill_candidate *slot;

		if (!S_ISREG(st.st_mode))
			MORPH_RETURN(-EINVAL);
		rc = skill_parse_file(logger, skill_md, &candidate.fm, &body);
		free(body);
		if (rc != 0)
			return rc;
		if (!cli_manage_name_valid(candidate.fm.name) ||
		    !candidate.fm.description[0]) {
			fprintf(stderr, "morph: skill needs a valid name and description: %s\n",
				skill_md);
			MORPH_RETURN(-EINVAL);
		}
		if (candidates->nelts >= SKILL_MAX_ENTRIES)
			MORPH_RETURN(-E2BIG);
		rc = cli_manage_copy_arg(candidate.path, sizeof(candidate.path), path);
		if (rc != 0)
			return rc;
		slot = morph_array_push(candidates);
		if (!slot)
			MORPH_RETURN(-ENOMEM);
		*slot = candidate;
		return 0;
	}
	if (errno != ENOENT)
		MORPH_RETURN_ERRNO();
	rc = directory_names(path, &names);
	if (rc != 0)
		return rc;
	for (size_t i = 0; rc == 0 && i < names.nelts; i++) {
		const char *name = *(char **)morph_array_get(&names, i);
		char child[PATH_MAX];

		if (strcmp(name, ".git") == 0 || strncmp(name, ".morph-install-", 15) == 0)
			continue;
		rc = file_path_join(child, sizeof(child), path, name);
		if (rc == 0 && lstat(child, &st) != 0)
			MORPH_SET_ERR(rc, -errno);
		if (rc == 0 && S_ISDIR(st.st_mode))
			rc = scan_directory(logger, child, candidates, depth + 1);
	}
	names_cleanup(&names);
	return rc;
}

int skill_store_scan(struct morph_log *logger, const char *root, morph_array_t *candidates)
{
	return scan_directory(logger, root, candidates, 0);
}

static int copy_file(const char *source, const char *destination, mode_t mode)
{
	int input = open(source, O_RDONLY | O_NOFOLLOW);
	int output;
	int rc = 0;
	char buffer[BUFSIZ];

	if (input < 0)
		MORPH_RETURN_ERRNO();
	output = open(destination, O_CREAT | O_EXCL | O_WRONLY, mode & 0777);
	if (output < 0) {
		int error = -errno;

		close(input);
		MORPH_RETURN(error);
	}
	for (;;) {
		ssize_t count = read(input, buffer, sizeof(buffer));
		size_t offset = 0;

		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			MORPH_SET_ERR(rc, -errno);
			break;
		}
		if (count == 0)
			break;
		while (offset < (size_t)count) {
			ssize_t written = write(output, buffer + offset, (size_t)count - offset);

			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0) {
				MORPH_SET_ERR(rc, written < 0 ? -errno : -EIO);
				break;
			}
			offset += (size_t)written;
		}
		if (rc != 0)
			break;
	}
	if (rc == 0 && fchmod(output, mode & 0777) != 0)
		MORPH_SET_ERR(rc, -errno);
	if (close(output) != 0 && rc == 0)
		MORPH_SET_ERR(rc, -errno);
	close(input);
	return rc;
}

static int copy_tree(const char *root, const char *source, const char *destination,
		     int depth)
{
	struct stat st;
	morph_array_t names = {0};
	int rc;

	if (depth > SKILL_TREE_DEPTH_MAX)
		MORPH_RETURN(-ELOOP);
	if (lstat(source, &st) != 0)
		MORPH_RETURN_ERRNO();
	if (S_ISREG(st.st_mode))
		return copy_file(source, destination, st.st_mode);
	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		char resolved[PATH_MAX];
		ssize_t count = readlink(source, target, sizeof(target) - 1);

		if (count < 0)
			MORPH_RETURN_ERRNO();
		target[count] = '\0';
		if (count == (ssize_t)sizeof(target) - 1 || target[0] == '/' ||
		    !realpath(source, resolved) || !path_is_within(resolved, root))
			MORPH_RETURN(-EACCES);
		if (symlink(target, destination) != 0)
			MORPH_RETURN_ERRNO();
		return 0;
	}
	if (!S_ISDIR(st.st_mode))
		MORPH_RETURN(-ENOTSUP);
	if (mkdir(destination, 0700) != 0)
		MORPH_RETURN_ERRNO();
	rc = directory_names(source, &names);
	if (rc != 0)
		return rc;
	for (size_t i = 0; rc == 0 && i < names.nelts; i++) {
		const char *name = *(char **)morph_array_get(&names, i);
		char from[PATH_MAX], to[PATH_MAX];

		if (strcmp(name, ".git") == 0 ||
		    (depth == 0 && strcmp(name, SKILL_INSTALL_METADATA) == 0))
			continue;
		rc = file_path_join(from, sizeof(from), source, name);
		if (rc == 0)
			rc = file_path_join(to, sizeof(to), destination, name);
		if (rc == 0)
			rc = copy_tree(root, from, to, depth + 1);
	}
	names_cleanup(&names);
	return rc;
}

int skill_store_copy(const char *source, const char *destination)
{
	return copy_tree(source, source, destination, 0);
}

static int remove_tree(const char *path, int depth)
{
	struct stat st;
	morph_array_t names = {0};
	int rc;

	if (depth > 128)
		MORPH_RETURN(-ELOOP);
	if (lstat(path, &st) != 0) {
		if (errno == ENOENT)
			return 0;
		MORPH_RETURN_ERRNO();
	}
	if (!S_ISDIR(st.st_mode)) {
		if (unlink(path) != 0)
			MORPH_RETURN_ERRNO();
		return 0;
	}
	rc = directory_names(path, &names);
	if (rc != 0)
		return rc;
	for (size_t i = 0; rc == 0 && i < names.nelts; i++) {
		char child[PATH_MAX];

		rc = file_path_join(child, sizeof(child), path,
			*(char **)morph_array_get(&names, i));
		if (rc == 0)
			rc = remove_tree(child, depth + 1);
	}
	names_cleanup(&names);
	if (rc == 0 && rmdir(path) != 0)
		MORPH_SET_ERR(rc, -errno);
	return rc;
}

int skill_store_remove(const char *path)
{
	return remove_tree(path, 0);
}

static int digest_tree(blake3_hasher *hasher, const char *path, int depth)
{
	struct stat st;
	morph_array_t names = {0};
	int rc;
	char buffer[BUFSIZ];

	if (depth > SKILL_TREE_DEPTH_MAX)
		MORPH_RETURN(-ELOOP);
	if (lstat(path, &st) != 0)
		MORPH_RETURN_ERRNO();
	if (S_ISREG(st.st_mode)) {
		int fd = open(path, O_RDONLY | O_NOFOLLOW);
		const char *kind = st.st_mode & 0111 ? "executable" : "file";

		if (fd < 0)
			MORPH_RETURN_ERRNO();
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
				MORPH_SET_ERR(rc, -errno);
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
			MORPH_RETURN_ERRNO();
		buffer[count] = '\0';
		blake3_hasher_update(hasher, "link", 5);
		blake3_hasher_update(hasher, buffer, (size_t)count + 1);
		return 0;
	}
	if (!S_ISDIR(st.st_mode))
		MORPH_RETURN(-ENOTSUP);
	blake3_hasher_update(hasher, "directory", 10);
	rc = directory_names(path, &names);
	if (rc != 0)
		return rc;
	for (size_t i = 0; rc == 0 && i < names.nelts; i++) {
		const char *name = *(char **)morph_array_get(&names, i);
		char child[PATH_MAX];

		if (depth == 0 && strcmp(name, SKILL_INSTALL_METADATA) == 0)
			continue;
		blake3_hasher_update(hasher, name, strlen(name) + 1);
		rc = file_path_join(child, sizeof(child), path, name);
		if (rc == 0)
			rc = digest_tree(hasher, child, depth + 1);
	}
	names_cleanup(&names);
	blake3_hasher_update(hasher, "end", 4);
	return rc;
}

int skill_store_digest(const char *path, char digest[SKILL_DIGEST_MAX])
{
	blake3_hasher hasher;
	unsigned char hash[BLAKE3_OUT_LEN];
	static const char hex[] = "0123456789abcdef";
	int rc;

	blake3_hasher_init(&hasher);
	rc = digest_tree(&hasher, path, 0);
	if (rc != 0)
		return rc;
	blake3_hasher_finalize(&hasher, hash, sizeof(hash));
	for (size_t i = 0; i < sizeof(hash); i++) {
		digest[i * 2] = hex[hash[i] >> 4];
		digest[i * 2 + 1] = hex[hash[i] & 15];
	}
	digest[sizeof(hash) * 2] = '\0';
	return 0;
}

cJSON *skill_store_metadata(const char *path)
{
	char file[PATH_MAX];
	char *text;
	cJSON *metadata;

	if (file_path_join(file, sizeof(file), path, SKILL_INSTALL_METADATA) != 0)
		return NULL;
	text = file_read_all(file, NULL);
	if (!text)
		return NULL;
	metadata = cJSON_Parse(text);
	free(text);
	return metadata;
}

int skill_store_unmodified(const char *path, const cJSON *metadata)
{
	char digest[SKILL_DIGEST_MAX];
	cJSON *expected = cJSON_GetObjectItemCaseSensitive(metadata, "digest");
	int rc;

	if (!cJSON_IsString(expected)) {
		fprintf(stderr, "morph: unmanaged skill, refusing replacement/removal: %s\n", path);
		MORPH_RETURN(-EPERM);
	}
	rc = skill_store_digest(path, digest);
	if (rc != 0)
		return rc;
	if (strcmp(digest, expected->valuestring) != 0) {
		fprintf(stderr, "morph: skill has local changes; "
			"preserve them before retrying: %s\n",
			path);
		MORPH_RETURN(-EBUSY);
	}
	return 0;
}

int skill_store_install(const char *root, const struct skill_candidate *candidate,
			cJSON *metadata, int replace)
{
	char temporary[PATH_MAX], stage[PATH_MAX], target[PATH_MAX];
	char backup[PATH_MAX], metadata_path[PATH_MAX];
	char digest[SKILL_DIGEST_MAX];
	struct stat st;
	char *json = NULL;
	int exists;
	int moved = 0;
	int rc;

	rc = file_path_join(target, sizeof(target), root, candidate->fm.name);
	if (rc != 0)
		return rc;
	exists = lstat(target, &st) == 0;
	if (!exists && errno != ENOENT)
		MORPH_RETURN_ERRNO();
	if (exists) {
		cJSON *previous;

		if (!replace)
			MORPH_RETURN(-EEXIST);
		if (!S_ISDIR(st.st_mode))
			MORPH_RETURN(-ELOOP);
		previous = skill_store_metadata(target);
		rc = skill_store_unmodified(target, previous);
		cJSON_Delete(previous);
		if (rc != 0)
			return rc;
	}
	rc = file_path_join(temporary, sizeof(temporary), root, ".morph-install-XXXXXX");
	if (rc != 0)
		return rc;
	if (!mkdtemp(temporary))
		MORPH_RETURN_ERRNO();
	rc = file_path_join(stage, sizeof(stage), temporary, "skill");
	if (rc == 0)
		rc = file_path_join(backup, sizeof(backup), temporary, "previous");
	if (rc == 0)
		rc = skill_store_copy(candidate->path, stage);
	if (rc == 0)
		rc = skill_store_digest(stage, digest);
	if (rc == 0 && !cJSON_AddStringToObject(metadata, "digest", digest))
		rc = -ENOMEM;
	if (rc == 0) {
		json = cJSON_PrintUnformatted(metadata);
		if (!json)
			rc = -ENOMEM;
	}
	if (rc == 0)
		rc = file_path_join(metadata_path, sizeof(metadata_path), stage,
			SKILL_INSTALL_METADATA);
	if (rc == 0)
		rc = cli_manage_atomic_write(metadata_path, json, strlen(json));
	if (rc == 0 && exists) {
		if (rename(target, backup) != 0)
			MORPH_SET_ERR(rc, -errno);
		else
			moved = 1;
	}
	if (rc == 0 && rename(stage, target) != 0)
		MORPH_SET_ERR(rc, -errno);
	if (rc != 0 && moved && rename(backup, target) != 0) {
		fprintf(stderr, "morph: previous skill preserved at %s\n", backup);
		free(json);
		MORPH_RETURN(-errno);
	}
	free(json);
	int cleanup_rc = skill_store_remove(temporary);

	return rc != 0 ? rc : cleanup_rc;
}
