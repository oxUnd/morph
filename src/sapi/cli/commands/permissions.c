#include "sapi/cli/commands/permissions.h"
#include "nb/err.h"
#include "runtime/runtime.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "sapi/cli/adapters/input/confirm.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/presentation/command_output.h"
#include "sapi/cli/presentation/confirm.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/adapters/protocol/interaction.h"
#include "sapi/cli/widgets/list.h"
#include "sapi/cli/pages/permissions.h"
#include "sapi/cli/application.h"

#include <stdint.h>

static const char *cli_permissions_subject_name(const char *subject)
{
	const char *slash;

	if (!subject)
		return "";
	slash = strrchr(subject, '/');
	return slash && slash[1] ? slash + 1 : subject;
}

static const char *cli_permissions_capability_name(const char *resource_kind)
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

static const char *cli_permissions_grant_group_name(const struct runtime_permission_grant *grants,
						    int start, int end)
{
	for (int i = start; i < end; i++) {
		if (strcmp(grants[i].resource_kind, "command") == 0)
			return grants[i].resource;
	}
	return cli_permissions_subject_name(grants[start].subject);
}

static int cli_permissions_compare_grants(const void *left, const void *right)
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

static int cli_permissions_print_permissions(struct cli_application *ctx)
{
	struct runtime_permission_grant *grants = NULL;
	int count = 0;
	int columns = cli_list_columns();
	int group_count = 0;
	int rc;

	rc = runtime_permission_list(ctx->runtime, &grants, &count);
	if (rc != 0)
		return rc;
	CMD_HEADER(ctx, "persistent permissions (%d)", count);
	if (count == 0) {
		cli_command_print_tree_note(ctx, "none for this project");
		runtime_permission_list_free(grants);
		return 0;
	}
	qsort(grants, (size_t)count, sizeof(grants[0]), cli_permissions_compare_grants);
	for (int i = 0; i < count; i++) {
		if (i == 0 || strcmp(grants[i - 1].subject, grants[i].subject) != 0)
			group_count++;
	}
	for (int i = 0, group = 0; i < count;) {
		int end = i + 1;
		int group_last;
		const char *ancestor;

		while (end < count && strcmp(grants[i].subject, grants[end].subject) == 0)
			end++;
		group_last = ++group == group_count;
		cli_list_group(ctx, cli_permissions_grant_group_name(grants, i, end), end - i,
			       group_last);
		ancestor = morph_tui_widget_tree_branch(group_last, 1);
		for (int j = i; j < end; j++) {
			char label[96];

			snprintf(label, sizeof(label), "#%lld  %s", (long long)grants[j].id,
				 cli_permissions_capability_name(grants[j].resource_kind));
			cli_list_item(ctx, ancestor, j == end - 1, "", label, grants[j].resource,
				      24, columns);
		}
		i = end;
	}
	runtime_permission_list_free(grants);
	return 0;
}

static int cli_permissions_confirm_clear(struct cli_application *ctx, int all_projects)
{
	enum cli_input_confirm_result answer;

	if (ctx && ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		cJSON *request = cJSON_CreateObject();
		int confirmed = 0;
		int rc;

		if (!request)
			return 0;
		if (!cJSON_AddStringToObject(request, "action", "clear_permissions") ||
		    !cJSON_AddBoolToObject(request, "all_projects", all_projects)) {
			cJSON_Delete(request);
			return 0;
		}
		rc = cli_interaction_confirm(ctx, "command_confirmation", request, &confirmed);
		cJSON_Delete(request);
		return rc == 0 && confirmed;
	}

	cli_confirm_prompt_show(ctx,
				all_projects ? CLI_CONFIRM_PROMPT_CLEAR_ALL_PERMISSIONS
					     : CLI_CONFIRM_PROMPT_CLEAR_PROJECT_PERMISSIONS,
				NULL);
	answer = all_projects ? cli_input_confirm_phrase("clear all")
			      : cli_input_confirm_yes_no();
	if (answer == CLI_INPUT_CONFIRM_NO_TERMINAL || answer == CLI_INPUT_CONFIRM_READ_ERROR)
		cli_confirm_prompt_end(ctx);
	return answer == CLI_INPUT_CONFIRM_ACCEPTED;
}

static void cli_permissions_grants_cleanup(struct runtime_permission_grant **owner)
{
	runtime_permission_list_free(*owner);
	*owner = NULL;
}

static int cli_permissions_revoke_subject_matches(struct cli_application *ctx, const char *name,
						  int *deleted)
{
	struct runtime_permission_grant *grants NB_CLEANUP(cli_permissions_grants_cleanup) = NULL;
	nb_vec subjects NB_CLEANUP(nb_vec_cleanup) = {0};
	int count = 0;
	int rc;

	*deleted = 0;
	rc = runtime_permission_list(ctx->runtime, &grants, &count);
	if (rc != 0)
		return rc;
	rc = (nb_vec_init(&subjects, sizeof(char *)),
	      nb_vec_reserve(&subjects, 4) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		return rc;
	}
	for (int i = 0; i < count; i++) {
		char **stored;
		int exists = 0;

		if (strcmp(grants[i].subject, name) != 0 &&
		    strcmp(cli_permissions_subject_name(grants[i].subject), name) != 0 &&
		    !(strcmp(grants[i].resource_kind, "command") == 0 &&
		      strcmp(grants[i].resource, name) == 0))
			continue;
		for (size_t _i_ = 0;
		     _i_ < nb_vec_len(&subjects) && (stored = (char **)nb_vec_at(&subjects, _i_));
		     _i_++) {
			if (strcmp(*stored, grants[i].subject) == 0) {
				exists = 1;
				break;
			}
		}
		if (exists)
			continue;
		stored = nb_vec_push(&subjects);
		if (!stored) {
			rc = NB_ERR_NOMEM;
			return rc;
		}
		*stored = grants[i].subject;
	}
	{
		char **subject;

		for (size_t _i_ = 0;
		     _i_ < nb_vec_len(&subjects) && (subject = (char **)nb_vec_at(&subjects, _i_));
		     _i_++) {
			int subject_deleted = 0;

			rc = runtime_permission_revoke_subject(ctx->runtime, *subject,
							       &subject_deleted);
			if (rc != 0)
				return rc;
			*deleted += subject_deleted;
		}
	}

	return rc;
}

static int cli_permissions_command_permissions(struct cli_application *ctx, int argc, char **argv)
{
	const char *action = cli_cmd_arg_borrow(argc, argv, 1);
	const char *target;
	int deleted = 0;
	int rc;

	if (!action || !strcmp(action, "status")) {
		struct permission_policy policy;

		if (!action && ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON &&
		    cli_application_tui_borrow(ctx))
			return cli_permissions_show(ctx);
		rc = runtime_permission_get_policy(ctx->runtime, &policy);
		if (rc != 0)
			return rc;
		CMD_HEADER(ctx, "permissions");
		cli_output_printf(ctx, "  %s: access=%s approval=%s reviewer=%s\n",
			runtime_permission_preset_name(&policy), permission_access_name(policy.access),
			permission_approval_name(policy.approval), permission_reviewer_name(policy.reviewer));
		return 0;
	}
	if (!strcmp(action, "mode") || !strcmp(action, "set")) {
		struct permission_policy policy;

		if (!strcmp(action, "mode") && argc == 3)
			rc = runtime_permission_preset(argv[2], &policy);
		else if (!strcmp(action, "set") && argc == 5)
			rc = permission_policy_parse(argv[2], argv[3], argv[4], &policy);
		else
			rc = NB_ERR_INVAL;
		if (rc == 0)
			rc = runtime_permission_set_policy(ctx->runtime, &policy);
		if (rc == 0)
			CMD_OK(ctx, "permissions: %s", runtime_permission_preset_name(&policy));
		return rc;
	}
	if (strcmp(action, "list") == 0)
		return cli_permissions_print_permissions(ctx);
	if (strcmp(action, "revoke") == 0) {
		char *end;
		long long id;

		target = cli_cmd_arg_borrow(argc, argv, 2);
		if (!target) {
			CMD_ERROR(ctx, "usage: /permissions revoke <id|program>");
			return NB_ERR_INVAL;
		}
		errno = 0;
		id = strtoll(target, &end, 10);
		if (errno == 0 && *end == '\0' && id > 0)
			rc = runtime_permission_revoke_id(ctx->runtime, (int64_t)id, &deleted);
		else
			rc = cli_permissions_revoke_subject_matches(ctx, target, &deleted);
		if (rc != 0)
			return rc;
		if (deleted == 0) {
			CMD_ERROR(ctx, "permission not found: %s", target);
			return NB_ERR_NOT_FOUND;
		}
		CMD_OK(ctx, "revoked %d persistent permission%s", deleted, deleted == 1 ? "" : "s");
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
				CMD_ERROR(ctx, "unknown option: %s", argv[i]);
				return NB_ERR_INVAL;
			}
		}
		if (!confirmed)
			confirmed = cli_permissions_confirm_clear(ctx, all_projects);
		if (!confirmed) {
			cli_command_print_cancelled(ctx);
			return 0;
		}
		rc = runtime_permission_clear(ctx->runtime, all_projects, &deleted);
		if (rc != 0)
			return rc;
		CMD_OK(ctx, "revoked %d persistent permission%s%s", deleted,
		       deleted == 1 ? "" : "s", all_projects ? " across all projects" : "");
		return 0;
	}
	CMD_ERROR(ctx, "usage: /permissions [list|revoke <id|program>|"
		       "clear [--all-projects] [--yes]]");
	return NB_ERR_INVAL;
}

static const struct cli_command cli_permissions_entries[] = {
	{"/permissions", cli_permissions_command_permissions,
	 "Configure access, approval and reviewer; manage grants",
	 "/permissions [status|mode <ask|auto|full|read_only>|set <access> <approval> <reviewer>|list|revoke <id|program>|"
	 "clear [--all-projects] [--yes]]"},
	{"/perms", cli_permissions_command_permissions, "Alias for /permissions",
	 "/perms [list|revoke <id|program>|clear]"},
};

struct cli_command_group cli_permission_commands(void)
{
	return (struct cli_command_group){
		cli_permissions_entries,
		(int)(sizeof(cli_permissions_entries) / sizeof(cli_permissions_entries[0])),
		"Security"};
}
