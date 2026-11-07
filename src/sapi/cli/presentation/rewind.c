#include "sapi/cli/presentation/rewind.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/widgets/list.h"
#include "util/error.h"
#include <limits.h>
#include <stdio.h>

static const char *cli_rewind_string(const cJSON *object, const char *key)
{
	const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
	return value ? value : "";
}

void cli_rewind_report_print(struct cli_application *ctx, const cJSON *report)
{
	const cJSON *item;
	cJSON_ArrayForEach (item, cJSON_GetObjectItemCaseSensitive(report, "files")) {
		char path[PATH_MAX];
		cli_list_compact_text(path, sizeof(path), cli_rewind_string(item, "path"));
		cli_output_printf(ctx, "  %s\n", path);
	}
	cJSON_ArrayForEach (item, cJSON_GetObjectItemCaseSensitive(report, "conflicts")) {
		char description[BUFSIZ];
		char path[PATH_MAX];
		cli_list_compact_text(path, sizeof(path), cli_rewind_string(item, "path"));
		cli_list_compact_text(description, sizeof(description),
				      cJSON_IsString(item) ? item->valuestring
							   : cli_rewind_string(item, "reason"));
		cli_output_printf(ctx, "  Conflict: %s%s%s\n", path,
				  *path && *description ? ": " : "", description);
	}
	int incomplete =
		cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "rollback_incomplete"));
	if (incomplete)
		cli_output_printf(ctx, "  Recovery incomplete: inspect session history and files "
				       "before continuing.\n");
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "cleanup_incomplete")))
		cli_output_printf(ctx, "  Temporary-file cleanup is incomplete.\n");
	cJSON_ArrayForEach (item, cJSON_GetObjectItemCaseSensitive(report, "effects")) {
		const cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "compensation_error");
		const cJSON *cleanup_code = cJSON_GetObjectItemCaseSensitive(item, "cleanup_error");
		int error = cJSON_IsNumber(code) ? code->valueint : 0;
		int cleanup_error = cJSON_IsNumber(cleanup_code) ? cleanup_code->valueint : 0;
		int applied = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "applied"));
		int compensated =
			cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "compensated"));
		int compensation_failed = error || (incomplete && applied && !compensated);
		if (!compensation_failed && !cleanup_error)
			continue;
		char path[PATH_MAX];
		char recovery[PATH_MAX];
		cli_list_compact_text(path, sizeof(path), cli_rewind_string(item, "path"));
		if (compensation_failed)
			cli_output_printf(ctx,
					  "  Could not return %s to its state before rewind: %s\n",
					  path, morph_strerror(error ? error : -EIO));
		if (cleanup_error)
			cli_output_printf(ctx, "  Temporary-file cleanup failed for %s: %s\n", path,
					  morph_strerror(cleanup_error));
		if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "recovery_available"))) {
			cli_list_compact_text(recovery, sizeof(recovery),
					      cli_rewind_string(item, "recovery_path"));
			if (*recovery)
				cli_output_printf(ctx, "  Recovery copy: %s\n", recovery);
		}
	}
}
