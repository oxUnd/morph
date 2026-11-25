#ifndef MORPH_CLI_SERVICES_GIT_DIFF_H
#define MORPH_CLI_SERVICES_GIT_DIFF_H

#include <nb/buf.h>
#include <nb/vec.h>

enum cli_git_diff_kind { CLI_GIT_DIFF_UNSTAGED, CLI_GIT_DIFF_STAGED, CLI_GIT_DIFF_UNTRACKED };

struct cli_git_diff_entry {
	char *path, *original;
	nb_buf label;
	enum cli_git_diff_kind kind;
	char status;
	long long added, removed;
	int binary;
};

struct cli_git_diff_result {
	nb_buf root, branch, patch, error;
	nb_vec entries;
	int status, truncated;
};

struct cli_git_diff_job;

/* A NULL entry requests an inventory; otherwise the job copies its path(s).
 * Workers never access views. All subprocess output and execution are bounded. */
int cli_git_diff_start(const char *directory, const struct cli_git_diff_entry *entry,
		       struct cli_git_diff_job **out);
int cli_git_diff_collect(struct cli_git_diff_job **job, struct cli_git_diff_result **out);
void cli_git_diff_stop(struct cli_git_diff_job **job);
void cli_git_diff_result_cleanup(struct cli_git_diff_result **result);

#endif
