#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"

#include <stdint.h>

static const char *subject_name(const char *subject)
{
	const char *slash;

	if (!subject)
		return "";
	slash = strrchr(subject, '/');
	return slash && slash[1] ? slash + 1 : subject;
}

static const char *capability_name(const char *resource_kind)
{
	if (!resource_kind)
		return "";
	if (strcmp(resource_kind, "write_path") == 0 ||
	    strcmp(resource_kind, "tool_write_path") == 0)
		return "read/write";
	if (strcmp(resource_kind, "read_path") == 0)
		return "read";
	return resource_kind;
}

static const char *grant_group_name(
	const struct runtime_permission_grant *grants, int start, int end)
{
	for (int i = start; i < end; i++) {
		if (strcmp(grants[i].resource_kind, "command") == 0)
			return grants[i].resource;
	}
	return subject_name(grants[start].subject);
}

static int compare_grants(const void *left, const void *right)
{
	const struct runtime_permission_grant *a = left;
	const struct runtime_permission_grant *b = right;
	int rc = strcmp(a->subject, b->subject);

	if (rc != 0)
		return rc;
	if (a->id < b->id)
		return -1;
	if (a->id > b->id)
		return 1;
	return 0;
}

static int print_permissions(struct cli_context *ctx)
{
	struct runtime_permission_grant *grants = NULL;
	int count = 0;
	int columns = cli_list_columns();
	int group_count = 0;
	int rc;

	rc = runtime_permission_list(ctx->runtime, &grants, &count);
	if (rc != 0)
		return rc;
	CMD_HEADER("persistent permissions (%d)", count);
	if (count == 0) {
		printf("  " ANSI_DIM "└ none for this project" ANSI_RESET "\n");
		runtime_permission_list_free(grants);
		return 0;
	}
	qsort(grants, (size_t)count, sizeof(grants[0]), compare_grants);
	for (int i = 0; i < count; i++) {
		if (i == 0 || strcmp(grants[i - 1].subject,
				    grants[i].subject) != 0)
			group_count++;
	}
	for (int i = 0, group = 0; i < count;) {
		int end = i + 1;
		int group_last;
		const char *ancestor;

		while (end < count &&
		       strcmp(grants[i].subject, grants[end].subject) == 0)
			end++;
		group_last = ++group == group_count;
		cli_list_group(grant_group_name(grants, i, end), end - i,
			       group_last);
		ancestor = group_last ? "  " : "│ ";
		for (int j = i; j < end; j++) {
			char label[96];

			snprintf(label, sizeof(label), "#%lld  %s",
				 (long long)grants[j].id,
				 capability_name(grants[j].resource_kind));
			cli_list_item(ancestor, j == end - 1, "", label,
				      grants[j].resource, 24, columns);
		}
		i = end;
	}
	runtime_permission_list_free(grants);
	return 0;
}

static int confirm_clear(int all_projects)
{
	char answer[64];
	FILE *tty;

	if (all_projects)
		printf(ANSI_BOLD ANSI_RED
		       "? Type \"clear all\" to revoke permissions "
		       "for every project: " ANSI_RESET);
	else
		printf(ANSI_BOLD ANSI_YELLOW
		       "? Revoke all persistent permissions for "
		       "this project? [y/N]: " ANSI_RESET);
	fflush(stdout);
	tty = fopen("/dev/tty", "r");
	if (!tty) {
		putchar('\n');
		return 0;
	}
	if (!fgets(answer, sizeof(answer), tty)) {
		fclose(tty);
		putchar('\n');
		return 0;
	}
	fclose(tty);
	answer[strcspn(answer, "\r\n")] = '\0';
	if (all_projects)
		return strcmp(answer, "clear all") == 0;
	return answer[0] == 'y' || answer[0] == 'Y';
}

static int revoke_subject_matches(struct cli_context *ctx,
				  const char *name, int *deleted)
{
	struct runtime_permission_grant *grants = NULL;
	morph_array_t subjects;
	int count = 0;
	int rc;

	*deleted = 0;
	rc = runtime_permission_list(ctx->runtime, &grants, &count);
	if (rc != 0)
		return rc;
	rc = morph_array_init(&subjects, 4, sizeof(char *));
	if (rc != 0) {
		runtime_permission_list_free(grants);
		return rc;
	}
	for (int i = 0; i < count; i++) {
		char **stored;
		int exists = 0;

		if (strcmp(grants[i].subject, name) != 0 &&
		    strcmp(subject_name(grants[i].subject), name) != 0 &&
		    !(strcmp(grants[i].resource_kind, "command") == 0 &&
		      strcmp(grants[i].resource, name) == 0))
			continue;
		morph_array_foreach(stored, &subjects, char *) {
			if (strcmp(*stored, grants[i].subject) == 0) {
				exists = 1;
				break;
			}
		}
		if (exists)
			continue;
		stored = morph_array_push(&subjects);
		if (!stored) {
			rc = -ENOMEM;
			goto out;
		}
		*stored = grants[i].subject;
	}
	{
		char **subject;

		morph_array_foreach(subject, &subjects, char *) {
			int subject_deleted = 0;

			rc = runtime_permission_revoke_subject(
				ctx->runtime, *subject, &subject_deleted);
			if (rc != 0)
				goto out;
			*deleted += subject_deleted;
		}
	}

out:
	morph_array_cleanup(&subjects);
	runtime_permission_list_free(grants);
	return rc;
}

static int cmd_permissions(struct cli_context *ctx, int argc, char **argv)
{
	const char *action = cli_cmd_arg(argc, argv, 1);
	const char *target;
	int deleted = 0;
	int rc;

	if (!action || strcmp(action, "list") == 0)
		return print_permissions(ctx);
	if (strcmp(action, "revoke") == 0) {
		char *end;
		long long id;

		target = cli_cmd_arg(argc, argv, 2);
		if (!target) {
			CMD_ERROR("usage: /permissions revoke <id|program>");
			return -EINVAL;
		}
		errno = 0;
		id = strtoll(target, &end, 10);
		if (errno == 0 && *end == '\0' && id > 0)
			rc = runtime_permission_revoke_id(
				ctx->runtime, (int64_t)id, &deleted);
		else
			rc = revoke_subject_matches(ctx, target, &deleted);
		if (rc != 0)
			return rc;
		if (deleted == 0) {
			CMD_ERROR("permission not found: %s", target);
			return -ENOENT;
		}
		CMD_OK("revoked %d persistent permission%s",
		       deleted, deleted == 1 ? "" : "s");
		return 0;
	}
	if (strcmp(action, "clear") == 0) {
		int all_projects = 0;
		int confirmed = 0;

		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--all-projects") == 0)
				all_projects = 1;
			else if (strcmp(argv[i], "--yes") == 0)
				confirmed = 1;
			else {
				CMD_ERROR("unknown option: %s", argv[i]);
				return -EINVAL;
			}
		}
		if (!confirmed)
			confirmed = confirm_clear(all_projects);
		if (!confirmed) {
			printf(ANSI_DIM "  cancelled" ANSI_RESET "\n");
			return 0;
		}
		rc = runtime_permission_clear(ctx->runtime, all_projects,
					      &deleted);
		if (rc != 0)
			return rc;
		CMD_OK("revoked %d persistent permission%s%s",
		       deleted, deleted == 1 ? "" : "s",
		       all_projects ? " across all projects" : "");
		return 0;
	}
	CMD_ERROR("usage: /permissions [list|revoke <id|program>|"
		  "clear [--all-projects] [--yes]]");
	return -EINVAL;
}

static const struct cli_command permission_commands[] = {
	{ "/permissions", cmd_permissions,
	  "View or revoke persistent tool permissions",
	  "/permissions [list|revoke <id|program>|"
	  "clear [--all-projects] [--yes]]" },
	{ "/perms", cmd_permissions, "Alias for /permissions",
	  "/perms [list|revoke <id|program>|clear]" },
};

int cli_register_permission_commands(void)
{
	return cli_command_register_many(
		permission_commands,
		(int)(sizeof(permission_commands) /
		      sizeof(permission_commands[0])),
		"Security");
}
