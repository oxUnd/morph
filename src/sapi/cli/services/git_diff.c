#include "sapi/cli/services/git_diff.h"

#include "terminal/text.h"
#include <nb/cancel.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/process.h>
#include <nb/thread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_GIT_DIFF_LIMIT (1024u * 1024u)

struct cli_git_diff_job {
	nb_thread *thread;
	struct nb_cancel cancel;
	nb_atomic_int done;
	char *directory, *path, *original;
	enum cli_git_diff_kind kind;
	struct cli_git_diff_result *result;
};

static void cli_git_diff_entry_cleanup(struct cli_git_diff_entry *entry)
{
	free(entry->path);
	free(entry->original);
	nb_buf_cleanup(&entry->label);
}

void cli_git_diff_result_cleanup(struct cli_git_diff_result **owner)
{
	struct cli_git_diff_result *result = *owner;
	if (!result)
		return;

	for (size_t i = 0; i < result->entries.len; i++)
		cli_git_diff_entry_cleanup(nb_vec_at(&result->entries, i));

	nb_vec_cleanup(&result->entries);
	nb_buf_cleanup(&result->root);
	nb_buf_cleanup(&result->branch);
	nb_buf_cleanup(&result->patch);
	nb_buf_cleanup(&result->error);
	free(result);
	*owner = NULL;
}

static int cli_git_diff_cancelled(void *data)
{
	return nb_cancel_requested(&((struct cli_git_diff_job *)data)->cancel);
}

static int cli_git_diff_run(struct cli_git_diff_job *job, const char *const *arguments,
			    nb_buf *output, int *exit_code)
{
	const char *argv[40] = {"git",
				"--no-pager",
				"--no-optional-locks",
				"--literal-pathspecs",
				"-c",
				"core.quotepath=false",
				"-c",
				"core.fsmonitor=false"};
	size_t count = 8;
	for (size_t i = 0; arguments[i]; i++) {
		if (count + 1 >= sizeof(argv) / sizeof(argv[0]))
			return NB_ERR_TOO_BIG;

		argv[count++] = arguments[i];
	}

	const nb_process_options options = {.argv = argv,
					    .cwd = job->directory,
					    .search_path = true,
					    .close_fds = true,
					    .new_group = true,
					    .stdin_mode = NB_PROCESS_NULL};
	nb_process *child NB_CLEANUP(nb_process_cleanup) = NULL;
	int rc = nb_process_spawn(&options, &child, NULL);
	if (rc)
		return rc;

	bool truncated = false;
	const nb_process_exchange_options exchange = {.timeout_ms = 10000,
						      .max_output = CLI_GIT_DIFF_LIMIT,
						      .cancelled = cli_git_diff_cancelled,
						      .cancel_ctx = job,
						      .stdout_truncated = &truncated};
	nb_buf errors NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = nb_process_exchange(child, NULL, 0, &exchange, output, &errors, exit_code, NULL);
	job->result->truncated |= truncated;
	if (!rc && *exit_code > 1) {
		nb_buf_clear(&job->result->error);
		char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
			nb_buf_cstr(&errors), errors.len, MORPH_TERMINAL_TEXT_MULTILINE, NULL);
		if (!safe || !nb_buf_append_cstr(&job->result->error, safe))
			return NB_ERR_NOMEM;
	}

	return rc;
}

static const char *cli_git_diff_token(const nb_buf *input, size_t *offset)
{
	if (*offset >= input->len)
		return NULL;

	const char *start = nb_buf_cstr(input) + *offset;
	const char *end = memchr(start, 0, input->len - *offset);
	if (!end)
		return NULL;

	*offset += (size_t)(end - start) + 1;
	return start;
}

static int cli_git_diff_add(struct cli_git_diff_result *result, const char *path,
			    const char *original, enum cli_git_diff_kind kind, char status)
{
	struct cli_git_diff_entry entry NB_CLEANUP(cli_git_diff_entry_cleanup) = {.kind = kind,
										  .status = status};
	entry.path = strdup(path);
	entry.original = original ? strdup(original) : NULL;
	if (!entry.path || (original && !entry.original))
		return NB_ERR_NOMEM;

	if (!nb_vec_push_copy(&result->entries, &entry))
		return NB_ERR_NOMEM;

	memset(&entry, 0, sizeof(entry));
	return NB_OK;
}

static int cli_git_diff_status(struct cli_git_diff_job *job)
{
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	int code = 0;
	const char *const argv[] = {"status", "--porcelain=v1", "-z", "--untracked-files=all",
				    NULL};
	int rc = cli_git_diff_run(job, argv, &output, &code);
	if (rc || code)
		return rc ? rc : NB_ERR_OS;

	if (job->result->truncated)
		return NB_ERR_TOO_BIG;

	size_t offset = 0;
	const char *record;
	while ((record = cli_git_diff_token(&output, &offset))) {
		if (strlen(record) < 4)
			return NB_ERR_FORMAT;

		const char *original = NULL;
		if (record[0] == 'R' || record[0] == 'C' || record[1] == 'R' || record[1] == 'C') {
			original = cli_git_diff_token(&output, &offset);
			if (!original)
				return NB_ERR_FORMAT;
		}

		if (record[0] == '?') {
			rc = cli_git_diff_add(job->result, record + 3, NULL, CLI_GIT_DIFF_UNTRACKED,
					      '?');
		} else {
			if (record[0] != ' ')
				rc = cli_git_diff_add(job->result, record + 3, original,
						      CLI_GIT_DIFF_STAGED, record[0]);

			if (!rc && record[1] != ' ')
				rc = cli_git_diff_add(job->result, record + 3, original,
						      CLI_GIT_DIFF_UNSTAGED, record[1]);
		}

		if (rc)
			return rc;
	}

	return NB_OK;
}

static int cli_git_diff_stats(struct cli_git_diff_job *job, enum cli_git_diff_kind kind)
{
	nb_buf output NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *const argv[] = {"diff",
				    "--no-ext-diff",
				    "--no-textconv",
				    "--numstat",
				    "-z",
				    kind == CLI_GIT_DIFF_STAGED ? "--cached" : "--",
				    NULL};
	int code = 0;
	int rc = cli_git_diff_run(job, argv, &output, &code);
	if (rc || code || job->result->truncated)
		return rc ? rc : NB_ERR_TOO_BIG;

	size_t offset = 0;
	const char *record;
	while ((record = cli_git_diff_token(&output, &offset))) {
		const char *first = strchr(record, '\t');
		const char *second = first ? strchr(first + 1, '\t') : NULL;
		if (!second)
			return NB_ERR_FORMAT;

		const char *path = second + 1;
		if (!*path) {
			const char *old = cli_git_diff_token(&output, &offset);
			path = cli_git_diff_token(&output, &offset);
			if (!old || !path)
				return NB_ERR_FORMAT;
		}

		for (size_t i = 0; i < job->result->entries.len; i++) {
			struct cli_git_diff_entry *entry = nb_vec_at(&job->result->entries, i);
			if (entry->kind != kind || strcmp(entry->path, path))
				continue;

			entry->binary = record[0] == '-';
			if (!entry->binary) {
				entry->added = strtoll(record, NULL, 10);
				entry->removed = strtoll(first + 1, NULL, 10);
			}
		}
	}

	return NB_OK;
}

static int cli_git_diff_compare(const void *left, const void *right)
{
	const struct cli_git_diff_entry *a = left, *b = right;
	return a->kind != b->kind ? (int)a->kind - (int)b->kind : strcmp(a->path, b->path);
}

static int cli_git_diff_inventory(struct cli_git_diff_job *job)
{
	struct cli_git_diff_result *result = job->result;
	const char *const root[] = {"rev-parse", "--show-toplevel", NULL};
	int code = 0;
	int rc = cli_git_diff_run(job, root, &result->root, &code);
	if (rc || code)
		return rc ? rc : NB_ERR_NOT_FOUND;

	while (result->root.len && (result->root.ptr[result->root.len - 1] == '\n' ||
				    result->root.ptr[result->root.len - 1] == '\r'))
		result->root.ptr[--result->root.len] = 0;

	char *directory NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&result->root));
	if (!directory)
		return NB_ERR_NOMEM;

	free(job->directory);
	job->directory = nb_charp_steal(&directory);

	const char *const branch[] = {"symbolic-ref", "--quiet", "--short", "HEAD", NULL};
	rc = cli_git_diff_run(job, branch, &result->branch, &code);
	if (rc)
		return rc;

	if (code && !nb_buf_append_cstr(&result->branch, "Detached HEAD"))
		return NB_ERR_NOMEM;

	while (result->branch.len && result->branch.ptr[result->branch.len - 1] == '\n')
		result->branch.ptr[--result->branch.len] = 0;

	rc = cli_git_diff_status(job);
	if (!rc)
		rc = cli_git_diff_stats(job, CLI_GIT_DIFF_UNSTAGED);

	if (!rc)
		rc = cli_git_diff_stats(job, CLI_GIT_DIFF_STAGED);

	if (rc)
		return rc;

	if (result->entries.len > 1)
		qsort(result->entries.data, result->entries.len, sizeof(struct cli_git_diff_entry),
		      cli_git_diff_compare);
	for (size_t i = 0; i < result->entries.len; i++) {
		struct cli_git_diff_entry *entry = nb_vec_at(&result->entries, i);
		char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
			entry->path, strlen(entry->path), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
		if (!safe || !nb_buf_printf(&entry->label, "%c %s", entry->status, safe))
			return NB_ERR_NOMEM;

		if (entry->binary)
			nb_buf_append_cstr(&entry->label, "  (binary)");
		else if (entry->kind != CLI_GIT_DIFF_UNTRACKED)
			nb_buf_printf(&entry->label, "  +%lld -%lld", entry->added, entry->removed);

		if (!nb_buf_ok(&entry->label))
			return nb_buf_status(&entry->label);
	}

	return NB_OK;
}

static int cli_git_diff_preview(struct cli_git_diff_job *job)
{
	const char *argv[20] = {"diff",		 "--no-ext-diff",   "--no-textconv",  "--no-color",
				"--no-relative", "--src-prefix=a/", "--dst-prefix=b/"};
	size_t count = 7;
	if (job->kind == CLI_GIT_DIFF_UNTRACKED)
		argv[count++] = "--no-index";
	else if (job->kind == CLI_GIT_DIFF_STAGED)
		argv[count++] = "--cached";

	argv[count++] = "--";
	if (job->kind == CLI_GIT_DIFF_UNTRACKED)
		argv[count++] = "/dev/null";
	else if (job->original)
		argv[count++] = job->original;

	argv[count++] = job->path;
	int code = 0;
	int rc = cli_git_diff_run(job, argv, &job->result->patch, &code);
	return rc ? rc : code > 1 ? NB_ERR_OS : NB_OK;
}

static void *cli_git_diff_worker(void *data)
{
	struct cli_git_diff_job *job = data;
	job->result->status = job->path ? cli_git_diff_preview(job) : cli_git_diff_inventory(job);
	nb_atomic_int_store(&job->done, 1);
	return NULL;
}

void cli_git_diff_stop(struct cli_git_diff_job **owner)
{
	struct cli_git_diff_job *job = *owner;
	if (!job)
		return;

	nb_cancel_request(&job->cancel);
	if (job->thread)
		(void)nb_thread_join_take(&job->thread, NULL);

	cli_git_diff_result_cleanup(&job->result);
	free(job->directory);
	free(job->path);
	free(job->original);
	free(job);
	*owner = NULL;
}

int cli_git_diff_start(const char *directory, const struct cli_git_diff_entry *entry,
		       struct cli_git_diff_job **out)
{
	if (!directory || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	struct cli_git_diff_job *job NB_CLEANUP(cli_git_diff_stop) = calloc(1, sizeof(*job));
	if (!job)
		return NB_ERR_NOMEM;

	nb_cancel_init(&job->cancel);
	nb_atomic_int_init(&job->done, 0);
	job->directory = strdup(directory);
	job->result = calloc(1, sizeof(*job->result));
	if (!job->directory || !job->result)
		return NB_ERR_NOMEM;

	nb_vec_init(&job->result->entries, sizeof(struct cli_git_diff_entry));
	if (entry) {
		job->path = strdup(entry->path);
		job->original = entry->original ? strdup(entry->original) : NULL;
		job->kind = entry->kind;
		if (!job->path || (entry->original && !job->original))
			return NB_ERR_NOMEM;
	}

	int rc = nb_thread_create(&job->thread, cli_git_diff_worker, job, NULL);
	if (rc)
		return rc;

	*out = job;
	job = NULL;
	return NB_OK;
}

int cli_git_diff_collect(struct cli_git_diff_job **owner, struct cli_git_diff_result **out)
{
	if (!owner || !*owner || !out)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	struct cli_git_diff_job *job = *owner;
	if (!nb_atomic_int_load(&job->done))
		return NB_ERR_AGAIN;

	(void)nb_thread_join_take(&job->thread, NULL);
	*out = job->result;
	job->result = NULL;
	cli_git_diff_stop(owner);
	return NB_OK;
}
