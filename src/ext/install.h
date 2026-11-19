#ifndef EXT_INSTALL_H
#define EXT_INSTALL_H

struct nb_log;
#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stdio.h>

#define EXT_SOURCE_OWNER_MAX 128
#define EXT_SOURCE_REPO_MAX 128
#define EXT_SOURCE_REF_MAX 256
#define EXT_SOURCE_SUBDIR_MAX PATH_MAX

struct ext_source {
	char owner[EXT_SOURCE_OWNER_MAX];
	char repo[EXT_SOURCE_REPO_MAX];
	char ref[EXT_SOURCE_REF_MAX];
	char subdir[EXT_SOURCE_SUBDIR_MAX];
};

struct ext_install_options {
	const char *install_dir;
	const char *github_base_url;
	int yes;
	FILE *in;
	FILE *out;
	/* Optional frontend adapters. Output receives arbitrary byte chunks from
	 * both child streams; a negative result aborts installation. Confirmation
	 * returns positive to accept, zero to decline, or a negative status. */
	int (*output)(const char *bytes, size_t length, void *user_data);
	int (*confirm)(const char *name, const char *command, void *user_data);
	void *user_data;
};

struct ext_install_result {
	char name[64];
	char path[PATH_MAX];
	char resolved_ref[64];
};

int ext_source_parse(const char *source, struct ext_source *out);
int ext_install_source(struct nb_log *logger, const char *source,
		       const struct ext_install_options *opts, struct ext_install_result *result);

#ifdef __cplusplus
}
#endif

#endif
