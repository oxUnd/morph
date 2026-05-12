#include "skill.h"
#include "util/log.h"
#include "manifest.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

int skill_load(struct skill *sk, const char *dir_path)
{
	if (!sk || !dir_path)
		return -EINVAL;
	memset(sk, 0, sizeof(*sk));
	strncpy(sk->path, dir_path, sizeof(sk->path) - 1);

	char manifest_path[1024];
	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.toml", dir_path);

	struct stat st;
	if (stat(manifest_path, &st) != 0) {
		log_err("skill manifest not found: %s", manifest_path);
		return -ENOENT;
	}

	int rc = manifest_parse_file(manifest_path, &sk->manifest);
	if (rc < 0) {
		log_err("failed to parse skill manifest: %s", manifest_path);
		return rc;
	}

	if (strcmp(sk->manifest.type, "exec") == 0) {
		snprintf(sk->exec_path, sizeof(sk->exec_path), "%s/%s",
			 dir_path, sk->manifest.entry);
		sk->run = NULL;
		log_info("loaded exec skill: %s from %s", sk->manifest.name, sk->exec_path);
	} else if (strcmp(sk->manifest.type, "so") == 0) {
		log_info("so skill loading not yet implemented: %s", sk->manifest.name);
		sk->dl_handle = NULL;
		sk->run = NULL;
	} else {
		log_warn("unknown skill type: %s", sk->manifest.type);
	}

	snprintf(sk->tool_desc.name, sizeof(sk->tool_desc.name), "%s", sk->manifest.name);
	snprintf(sk->tool_desc.desc, sizeof(sk->tool_desc.desc), "%s", sk->manifest.description);
	if (sk->manifest.args_schema)
		snprintf(sk->tool_desc.args_spec, sizeof(sk->tool_desc.args_spec),
			 "%s", sk->manifest.args_schema);

	sk->enabled = 1;
	return 0;
}

int skill_unload(struct skill *sk)
{
	if (!sk)
		return -EINVAL;
	if (sk->dl_handle) {
		log_info("skill_unload: would dlclose %s", sk->manifest.name);
		sk->dl_handle = NULL;
	}
	free(sk->manifest.args_schema);
	free(sk->manifest.output_schema);
	for (int i = 0; i < sk->manifest.allowed_paths_count; i++)
		free(sk->manifest.allowed_paths[i]);
	free(sk->manifest.allowed_paths);
	for (int i = 0; i < sk->manifest.allowed_env_count; i++)
		free(sk->manifest.allowed_env[i]);
	free(sk->manifest.allowed_env);
	memset(sk, 0, sizeof(*sk));
	return 0;
}

int skill_run(struct skill *sk, const char *args_json, char **result_json)
{
	if (!sk || !args_json || !result_json)
		return -EINVAL;
	if (!sk->enabled)
		return -EACCES;
	if (sk->run)
		return sk->run(args_json, result_json);

	if (strcmp(sk->manifest.type, "exec") == 0 && sk->exec_path[0]) {
		log_info("executing skill: %s", sk->exec_path);
		FILE *pipe = popen(sk->exec_path, "r");
		if (!pipe) {
			log_err("failed to execute skill: %s", sk->exec_path);
			return -EIO;
		}
		char buf[8192];
		size_t total = 0;
		char *result = malloc(8192);
		if (!result) {
			pclose(pipe);
			return -ENOMEM;
		}
		size_t cap = 8192;
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
			if (total + n >= cap) {
				cap = (total + n) * 2;
				char *new_result = realloc(result, cap);
				if (!new_result) {
					free(result);
					pclose(pipe);
					return -ENOMEM;
				}
				result = new_result;
			}
			memcpy(result + total, buf, n);
			total += n;
		}
		result[total] = '\0';
		pclose(pipe);
		*result_json = result;
		return 0;
	}

	log_info("skill_run: no run function for %s", sk->manifest.name);
	return -ENOSYS;
}