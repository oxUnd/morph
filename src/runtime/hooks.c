#include "runtime/hooks.h"
#include "exec/process.h"
#include "agent/tokenizer.h"
#include "agent/react.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/log.h"
#include "util/sha256.h"
#include "util/utf8.h"

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HOOK_LIMIT 128
#define HOOK_OUTPUT_MAX 65536u
#define HOOK_INPUT_MAX (1024u * 1024u)

struct hook_entry {
	char id[65];
	char digest[65];
	char *source;
	char *event;
	char *matcher;
	char *command;
	char *status_message;
	char *definition;
	char *previous;
	char *status;
	int supported;
	int trusted;
	int recorded;
	int disabled;
	int project;
	int timeout;
	int context_limit;
	regex_t regex;
	int regex_ready;
};

struct hook_run {
	struct hook_entry *entry;
	char session[PROCESS_SESSION_ID_MAX];
	morph_buf_t out;
	morph_buf_t err;
	int done;
	int rc;
	int exit_code;
	uint64_t duration_ms;
};

static const char *json_string(const cJSON *obj, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static void digest_text(const char *text, char out[65])
{
	struct sha256_ctx ctx;
	unsigned char hash[32];
	static const char hex[] = "0123456789abcdef";

	sha256_init(&ctx);
	sha256_update(&ctx, (const unsigned char *)text, strlen(text));
	sha256_final(&ctx, hash);
	for (size_t i = 0; i < sizeof(hash); i++) {
		out[i * 2] = hex[hash[i] >> 4];
		out[i * 2 + 1] = hex[hash[i] & 15];
	}
	out[64] = '\0';
}

static int event_supported(const char *event)
{
	return strcmp(event, "UserPromptSubmit") == 0 || strcmp(event, "PreToolUse") == 0 ||
	       strcmp(event, "PostToolUse") == 0 || strcmp(event, "Stop") == 0 ||
	       strcmp(event, "SessionStart") == 0 || strcmp(event, "SessionEnd") == 0;
}

static void entry_cleanup(struct hook_entry *entry)
{
	if (entry->regex_ready)
		regfree(&entry->regex);
	free(entry->source);
	free(entry->event);
	free(entry->matcher);
	free(entry->command);
	free(entry->status_message);
	free(entry->definition);
	free(entry->previous);
	free(entry->status);
}

static int refresh_trust(struct hook_manager *manager, struct hook_entry *entry)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	entry->trusted = 0;
	entry->recorded = 0;
	entry->disabled = 0;
	free(entry->previous);
	entry->previous = NULL;
	rc = sqlite3_prepare_v2(manager->db->handle,
				"SELECT digest,disabled,definition FROM hook_trust WHERE id=?", -1,
				&stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, entry->id, -1, SQLITE_TRANSIENT);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		entry->recorded = 1;
		entry->trusted =
		    strcmp(entry->digest, (const char *)sqlite3_column_text(stmt, 0)) == 0;
		entry->disabled = sqlite3_column_int(stmt, 1);
		if (((const char *)sqlite3_column_text(stmt, 0))[0])
			entry->previous = strdup((const char *)sqlite3_column_text(stmt, 2));
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

static int project_trusted(struct hook_manager *manager)
{
	sqlite3_stmt *stmt = NULL;
	int trusted = 0;

	if (sqlite3_prepare_v2(manager->db->handle, "SELECT 1 FROM hook_workspaces WHERE path=?",
			       -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(stmt, 1, manager->workdir, -1, SQLITE_TRANSIENT);
	trusted = sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);
	return trusted;
}

static int add_entry(struct hook_manager *manager, const char *source, const char *event,
		     const char *matcher, const cJSON *handler, int project, int group_index,
		     int handler_index)
{
	struct hook_entry *entry;
	morph_buf_t identity = {0};
	cJSON *canonical = cJSON_CreateObject();
	const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(handler, "timeout");
	const cJSON *limit = cJSON_GetObjectItemCaseSensitive(handler, "additionalContextLimit");
	int valid = cJSON_IsObject(handler);
	int rc;

	if (!valid) {
		cJSON_Delete(canonical);
		MORPH_RETURN(MORPH_ERR_CONFIG);
	}
	if (!canonical || manager->entries.nelts >= HOOK_LIMIT) {
		cJSON_Delete(canonical);
		MORPH_RETURN(canonical ? -E2BIG : -ENOMEM);
	}
	entry = morph_array_push(&manager->entries);
	if (!entry) {
		cJSON_Delete(canonical);
		MORPH_RETURN(-ENOMEM);
	}
	memset(entry, 0, sizeof(*entry));
	entry->source = file_resolve_path(source);
	if (!entry->source)
		entry->source = strdup(source);
	entry->event = strdup(event);
	entry->matcher = strdup(matcher);
	entry->command = strdup(json_string(handler, "command"));
	entry->status_message = strdup(json_string(handler, "statusMessage"));
	entry->project = project;
	entry->timeout = cJSON_IsNumber(timeout) ? timeout->valueint
						 : (strcmp(event, "SessionEnd") == 0 ? 1 : 600);
	entry->context_limit = cJSON_IsNumber(limit) ? limit->valueint : 2500;
	if (!entry->source || !entry->event || !entry->matcher || !entry->command ||
	    !entry->status_message) {
		cJSON_Delete(canonical);
		MORPH_RETURN(-ENOMEM);
	}
	if ((timeout &&
	     (!cJSON_IsNumber(timeout) || timeout->valuedouble != (double)timeout->valueint)) ||
	    entry->timeout < 1 || entry->timeout > 600 ||
	    (strcmp(event, "SessionEnd") == 0 && entry->timeout > 3) ||
	    (limit && (!cJSON_IsNumber(limit) || limit->valuedouble != (double)limit->valueint)) ||
	    entry->context_limit < 0 || entry->context_limit > 32768 || strlen(matcher) > 1024 ||
	    strlen(entry->command) >= PROCESS_COMMAND_MAX ||
	    (!entry->command[0] && strcmp(json_string(handler, "type"), "command") == 0) ||
	    !*json_string(handler, "type"))
		valid = 0;
	if (*matcher && strcmp(event, "Stop") != 0) {
		entry->regex_ready = regcomp(&entry->regex, matcher, REG_EXTENDED | REG_NOSUB) == 0;
		if (!entry->regex_ready)
			valid = 0;
	}
	/* Stable field order makes formatting and JSON key order irrelevant. */
	cJSON_AddStringToObject(canonical, "event", event);
	cJSON_AddStringToObject(canonical, "matcher", matcher);
	cJSON_AddStringToObject(canonical, "type", json_string(handler, "type"));
	cJSON_AddStringToObject(canonical, "command", entry->command);
	cJSON_AddStringToObject(canonical, "statusMessage", json_string(handler, "statusMessage"));
	cJSON_AddNumberToObject(canonical, "timeout", entry->timeout);
	cJSON_AddNumberToObject(canonical, "additionalContextLimit", entry->context_limit);
	entry->definition = cJSON_PrintUnformatted(canonical);
	cJSON_Delete(canonical);
	if (!entry->definition)
		MORPH_RETURN(-ENOMEM);
	rc = morph_buf_init(&identity, 128);
	if (rc == 0)
		rc = morph_buf_printf(&identity, "%s\n%s\n%s\n%d\n%d",
			project ? manager->workdir : "user", entry->source,
			event, group_index, handler_index);
	if (rc == 0) {
		digest_text(morph_buf_cstr(&identity), entry->id);
		morph_buf_reset(&identity);
		rc = morph_buf_printf(&identity, "%s\n%s", entry->id, entry->definition);
	}
	if (rc == 0)
		digest_text(morph_buf_cstr(&identity), entry->digest);
	morph_buf_cleanup(&identity);
	if (rc != 0)
		return rc;
	int portable = 1;
	const cJSON *field;
	cJSON_ArrayForEach(field, handler)
	{
		if (strcmp(field->string, "type") != 0 && strcmp(field->string, "command") != 0 &&
		    strcmp(field->string, "timeout") != 0 &&
		    strcmp(field->string, "statusMessage") != 0 &&
		    strcmp(field->string, "additionalContextLimit") != 0)
			portable = 0;
	}
	entry->supported = portable && valid && event_supported(event) &&
			   strcmp(json_string(handler, "type"), "command") == 0;
	entry->status = strdup(!valid ? "invalid" : !entry->supported ? "unsupported" : "");
	if (!entry->status)
		MORPH_RETURN(-ENOMEM);
	rc = refresh_trust(manager, entry);
	if (rc == 0 && !project && !entry->recorded) {
		char legacy_id[65];
		char legacy_digest[65];
		morph_buf_t legacy;
		sqlite3_stmt *stmt = NULL;

		rc = morph_buf_init(&legacy, 128);
		if (rc != 0)
			return rc;
		rc = morph_buf_printf(&legacy, "%s\n%s\n%s\n%d\n%d",
			manager->workdir, source, event, group_index, handler_index);
		if (rc == 0) {
			digest_text(morph_buf_cstr(&legacy), legacy_id);
			morph_buf_reset(&legacy);
			rc = morph_buf_printf(&legacy, "%s\n%s", legacy_id, entry->definition);
		}
		if (rc == 0)
			digest_text(morph_buf_cstr(&legacy), legacy_digest);
		morph_buf_cleanup(&legacy);
		if (rc != 0)
			return rc;
		rc = sqlite3_prepare_v2(manager->db->handle,
			"INSERT OR IGNORE INTO hook_trust(id,digest,disabled,definition) "
			"SELECT ?,CASE WHEN digest=? THEN ? ELSE '' END,disabled,definition "
			"FROM hook_trust "
			"WHERE id=? AND (digest=? OR disabled=1)", -1, &stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, entry->id, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 2, legacy_digest, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 3, entry->digest, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 4, legacy_id, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 5, legacy_digest, -1, SQLITE_TRANSIENT);
			rc = sqlite3_step(stmt);
		}
		sqlite3_finalize(stmt);
		if (rc != SQLITE_DONE)
			MORPH_RETURN(MORPH_ERR_DB);
		rc = refresh_trust(manager, entry);
	}
	return rc;
}

static int json_unique_fields(const cJSON *value, int depth)
{
	const cJSON *child;

	if (depth > 16)
		return 0;
	cJSON_ArrayForEach(child, value)
	{
		if (cJSON_IsObject(value)) {
			for (const cJSON *next = child->next; next; next = next->next) {
				if (strcmp(child->string, next->string) == 0)
					return 0;
			}
		}
		if ((cJSON_IsObject(child) || cJSON_IsArray(child)) &&
		    !json_unique_fields(child, depth + 1))
			return 0;
	}
	return 1;
}

static int load_object(struct hook_manager *manager, const cJSON *hooks, const char *source,
		       int project)
{
	const cJSON *event;

	if (!cJSON_IsObject(hooks) || !json_unique_fields(hooks, 0))
		MORPH_RETURN(MORPH_ERR_CONFIG);
	cJSON_ArrayForEach(event, hooks)
	{
		const cJSON *group;
		int gi = 0;

		if (!cJSON_IsArray(event))
			MORPH_RETURN(MORPH_ERR_CONFIG);
		cJSON_ArrayForEach(group, event)
		{
			const cJSON *handlers = cJSON_GetObjectItemCaseSensitive(group, "hooks");
			const cJSON *matcher = cJSON_GetObjectItemCaseSensitive(group, "matcher");
			const cJSON *handler;
			int hi = 0;

			if (!cJSON_IsObject(group) || !cJSON_IsArray(handlers) ||
			    (matcher && !cJSON_IsString(matcher)))
				MORPH_RETURN(MORPH_ERR_CONFIG);
			cJSON_ArrayForEach(handler, handlers)
			{
				int rc = add_entry(manager, source, event->string,
						   matcher ? matcher->valuestring : "", handler,
						   project, gi, hi++);
				if (rc != 0)
					return rc;
			}
			gi++;
		}
	}
	return 0;
}

static int hook_load_error(struct hook_manager *manager, const char *path,
			   int rc, const char *message)
{
	snprintf(manager->error.file, sizeof(manager->error.file), "%s", path);
	if (message != manager->error.detail.message)
		snprintf(manager->error.detail.message, sizeof(manager->error.detail.message),
			 "%s", message ? message : morph_strerror(rc));
	if (manager->error.detail.code == CONFIG_VALIDATION_NONE)
		manager->error.detail.code = rc == MORPH_ERR_PARSE ?
			CONFIG_VALIDATION_SYNTAX : CONFIG_VALIDATION_VALUE;
	log_err(manager ? manager->logger : NULL, "failed to load hooks %s:%d:%d: %s", path,
		manager->error.detail.line, manager->error.detail.column,
		manager->error.detail.message);
	MORPH_RETURN(rc);
}

static void hook_json_error_position(struct hook_manager *manager,
				     const char *text, const char *position)
{
	const char *line_start = text;
	int line = 1;

	if (!position)
		position = text;
	for (const char *p = text; p < position; p++) {
		if (*p == '\n') {
			line++;
			line_start = p + 1;
		}
	}
	manager->error.detail.line = line;
	manager->error.detail.column = (int)utf8nlen(line_start,
		(size_t)(position - line_start)) + 1;
}

static int load_json_file(struct hook_manager *manager, const char *path, int project)
{
	struct stat st;
	char *text;
	cJSON *root;
	int rc;
	const char *parse_end = NULL;
	char canonical[PATH_MAX];

	if (realpath(path, canonical))
		path = canonical;
	struct hook_entry *existing;
	morph_array_foreach(existing, &manager->entries, struct hook_entry)
	{
		if (strcmp(existing->source, path) == 0)
			return 0;
	}
	if (stat(path, &st) != 0) {
		rc = -errno;
		return rc == -ENOENT ? 0 : hook_load_error(manager, path, rc, NULL);
	}
	if (!S_ISREG(st.st_mode) || st.st_size > CONFIG_HOOKS_JSON_MAX)
		return hook_load_error(manager, path, -EFBIG,
			"Hooks configuration must be a regular file within the size limit.");
	text = file_read_all(path, NULL);
	if (!text)
		return hook_load_error(manager, path, -EIO, NULL);
	root = cJSON_ParseWithOpts(text, &parse_end, 1);
	if (!root) {
		hook_json_error_position(manager, text, parse_end);
		rc = hook_load_error(manager, path, MORPH_ERR_PARSE,
			parse_end && *parse_end ?
			"Invalid JSON syntax; check quotes, commas, and brackets." :
			"Unexpected end of JSON input; expected a complete JSON document.");
		free(text);
		return rc;
	}
	free(text);
	rc = load_object(manager, cJSON_GetObjectItemCaseSensitive(root, "hooks"), path, project);
	cJSON_Delete(root);
	if (rc != 0)
		return hook_load_error(manager, path, rc, rc == MORPH_ERR_CONFIG ?
			"Invalid hooks structure: expected a hooks object with event arrays, "
			"hook groups, and valid handlers." : NULL);
	return 0;
}

static int load_inline(struct hook_manager *manager, const char *path, const char *json,
		       int project)
{
	cJSON *hooks;
	int rc;

	if (!json || !*json)
		return 0;
	struct hook_entry *entry;
	const char *slash = strrchr(path, '/');
	if (slash) {
		morph_array_foreach(entry, &manager->entries, struct hook_entry)
		{
			const char *source_slash = strrchr(entry->source, '/');
			if (source_slash &&
			    (size_t)(source_slash - entry->source) == (size_t)(slash - path) &&
			    strncmp(entry->source, path, (size_t)(slash - path)) == 0) {
				log_warn(manager ? manager->logger : NULL,
					 "Both hooks.json and inline hooks found; merging: %s",
					 path);
				break;
			}
		}
	}
	hooks = cJSON_ParseWithOpts(json, NULL, 1);
	if (!hooks)
		return hook_load_error(manager, path, MORPH_ERR_PARSE,
			"Invalid inline hooks JSON.");
	rc = load_object(manager, hooks, path, project);
	cJSON_Delete(hooks);
	if (rc != 0)
		return hook_load_error(manager, path, rc,
			"Invalid inline hooks structure.");
	return 0;
}

int hook_manager_reload(struct hook_manager *manager)
{
	struct hook_entry *entry;
	char path[PATH_MAX];
	char *home = file_expand_path("~/.morph/hooks.json");
	int rc;
	int enabled = manager ? manager->config->hooks_enabled : 0;

	if (!manager || !manager->ready || !home) {
		free(home);
		MORPH_RETURN(-EINVAL);
	}
	memset(&manager->error, 0, sizeof(manager->error));
	morph_array_foreach(entry, &manager->entries, struct hook_entry) entry_cleanup(entry);
	morph_array_clear(&manager->entries);
	rc = load_json_file(manager, home, 0);
	free(home);
	if (rc == 0 && manager->config_path[0]) {
		char *directory = strdup(manager->config_path);
		if (!directory)
			MORPH_RETURN(-ENOMEM);
		char *slash = strrchr(directory, '/');
		if (slash)
			*slash = '\0';
		else
			strcpy(directory, ".");
		rc = file_path_join(path, sizeof(path), *directory ? directory : "/", "hooks.json");
		free(directory);
		if (rc == 0)
			rc = load_json_file(manager, path, 0);
	}
	if (rc == 0 && manager->config_path[0] && file_exists(manager->config_path)) {
		struct config *fresh = calloc(1, sizeof(*fresh));
		if (!fresh)
			MORPH_RETURN(-ENOMEM);
		rc = config_load(manager ? manager->logger : NULL, fresh, manager->config_path);
		if (rc != 0) {
			(void)config_validate_file(manager->config_path, &manager->error.detail);
			hook_load_error(manager, manager->config_path, rc,
				manager->error.detail.message[0] ? manager->error.detail.message : NULL);
		}
		enabled = fresh->hooks_enabled;
		if (rc == 0)
			rc = load_inline(manager, manager->config_path, fresh->hooks_json, 0);
		free(fresh);
	} else if (rc == 0) {
		rc = load_inline(manager, manager->config_path, manager->config->hooks_json, 0);
	}
	if (rc == 0)
		rc = file_path_join(path, sizeof(path), manager->workdir, ".morph/hooks.json");
	if (rc == 0)
		rc = load_json_file(manager, path, 1);
	/* Project TOML is parsed with the normal schema, never applied to runtime. */
	if (rc == 0)
		rc = file_path_join(path, sizeof(path), manager->workdir, ".morph/config.toml");
	if (rc == 0 && file_exists(path) && strcmp(path, manager->config_path) != 0) {
		struct config *project = calloc(1, sizeof(*project));
		if (!project)
			MORPH_RETURN(-ENOMEM);
		rc = config_load(manager ? manager->logger : NULL, project, path);
		if (rc != 0) {
			(void)config_validate_file(path, &manager->error.detail);
			hook_load_error(manager, path, rc,
				manager->error.detail.message[0] ? manager->error.detail.message : NULL);
		}
		if (rc == 0)
			rc = load_inline(manager, path, project->hooks_json, 1);
		free(project);
	}
	manager->enabled = enabled && !getenv("MORPH_HOOK_ACTIVE");
	if (rc != 0)
		manager->enabled = 0;
	return rc;
}

int hook_manager_init(struct hook_manager *manager, struct db *db, const struct config *config,
		      const char *config_path, const char *workdir)
{
	int rc;

	if (!manager || !db || !db->handle || !config || !workdir)
		MORPH_RETURN(-EINVAL);
	memset(manager, 0, sizeof(*manager));
	manager->logger = config->logger;
	manager->db = db;
	manager->config = config;
	snprintf(manager->config_path, sizeof(manager->config_path), "%s",
		 config_path ? config_path : "");
	char *resolved = file_resolve_path(workdir);
	snprintf(manager->workdir, sizeof(manager->workdir), "%s", resolved ? resolved : workdir);
	free(resolved);
	rc = db_exec(db,
		     "CREATE TABLE IF NOT EXISTS hook_trust("
		     "id TEXT PRIMARY KEY,digest TEXT NOT NULL,disabled INTEGER NOT NULL DEFAULT 0,"
		     "definition TEXT NOT NULL); CREATE TABLE IF NOT EXISTS hook_workspaces("
		     "path TEXT PRIMARY KEY)");
	if (rc == 0)
		rc = morph_array_init(&manager->entries, 8, sizeof(struct hook_entry));
	if (rc != 0)
		return rc;
	manager->recent = cJSON_CreateArray();
	if (!manager->recent) {
		morph_array_cleanup(&manager->entries);
		MORPH_RETURN(-ENOMEM);
	}
	manager->remaining_ms = 600000;
	manager->ready = 1;
	return hook_manager_reload(manager);
}

void hook_manager_cleanup(struct hook_manager *manager)
{
	struct hook_entry *entry;

	if (!manager || !manager->ready)
		return;
	morph_array_foreach(entry, &manager->entries, struct hook_entry) entry_cleanup(entry);
	morph_array_cleanup(&manager->entries);
	free(manager->session_context);
	cJSON_Delete(manager->recent);
	memset(manager, 0, sizeof(*manager));
}

static const char *entry_state(struct hook_manager *manager, struct hook_entry *entry)
{
	if (*entry->status)
		return entry->status;
	if (!manager->enabled)
		return "off";
	if (entry->disabled)
		return "disabled";
	if (entry->project && !project_trusted(manager))
		return "project_untrusted";
	if (!entry->trusted)
		return entry->previous ? "changed / needs_review" : "needs_review";
	return "trusted";
}

static int hook_review_digest(struct hook_manager *manager, char digest[65])
{
	morph_buf_t text;
	struct hook_entry *entry;
	int rc = morph_buf_init(&text, 256);

	if (rc != 0)
		return rc;
	rc = morph_buf_printf(&text, "%s\n%d\n", manager->workdir, manager->enabled);
	morph_array_foreach(entry, &manager->entries, struct hook_entry) {
		if (rc == 0)
			rc = morph_buf_printf(&text, "%s:%s:%d\n", entry->id,
				entry->digest, entry->supported);
	}
	if (rc == 0)
		digest_text(morph_buf_cstr(&text), digest);
	morph_buf_cleanup(&text);
	return rc;
}

cJSON *hook_manager_list(struct hook_manager *manager)
{
	cJSON *list = cJSON_CreateArray();
	struct hook_entry *entry;
	char review_digest[65];

	if (!manager || !manager->ready || !list ||
	    hook_review_digest(manager, review_digest) != 0) {
		cJSON_Delete(list);
		return NULL;
	}
	morph_array_foreach(entry, &manager->entries, struct hook_entry)
	{
		cJSON *row = cJSON_CreateObject();
		if (!row || refresh_trust(manager, entry) != 0) {
			cJSON_Delete(row);
			cJSON_Delete(list);
			return NULL;
		}
		cJSON_AddStringToObject(row, "review_digest", review_digest);
		cJSON_AddBoolToObject(row, "project", entry->project);
		cJSON_AddStringToObject(row, "id", entry->id);
		cJSON_AddStringToObject(row, "digest", entry->digest);
		cJSON_AddStringToObject(row, "source", entry->source);
		cJSON_AddStringToObject(row, "event", entry->event);
		cJSON_AddStringToObject(row, "matcher", entry->matcher);
		cJSON_AddStringToObject(row, "command", entry->command);
		cJSON_AddStringToObject(row, "definition", entry->definition);
		cJSON_AddStringToObject(row, "previous_definition",
					entry->previous ? entry->previous : "");
		cJSON_AddStringToObject(row, "state", entry_state(manager, entry));
		cJSON_AddStringToObject(row, "cwd", manager->workdir);
		char workspace_digest[65];
		digest_text(manager->workdir, workspace_digest);
		cJSON_AddStringToObject(row, "workspace_digest", workspace_digest);
		cJSON_AddBoolToObject(row, "workspace_trusted", project_trusted(manager));
		cJSON_AddNumberToObject(row, "timeout", entry->timeout);
		const cJSON *last;
		cJSON_ArrayForEach(last, manager->recent)
		{
			if (strcmp(json_string(last, "hook_id"), entry->id) == 0) {
				cJSON_DeleteItemFromObject(row, "last_run");
				cJSON_AddItemToObject(row, "last_run", cJSON_Duplicate(last, 1));
			}
		}
		cJSON_AddItemToArray(list, row);
	}
	return list;
}

static int hook_manager_batch(struct hook_manager *manager, int trust,
			      const char *expected_digest)
{
	char digest[65];
	struct hook_entry *entry;
	sqlite3_stmt *stmt = NULL;
	int rc = hook_manager_reload(manager);

	if (rc == 0)
		rc = hook_review_digest(manager, digest);
	if (rc != 0)
		return rc;
	if (strcmp(digest, expected_digest) != 0)
		MORPH_RETURN(-ESTALE);
	rc = db_exec(manager->db, "SAVEPOINT hook_batch;");
	if (rc != 0)
		return rc;
	if (trust) {
		rc = sqlite3_prepare_v2(manager->db->handle,
			"INSERT OR IGNORE INTO hook_workspaces(path) VALUES(?)", -1, &stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, manager->workdir, -1, SQLITE_TRANSIENT);
			rc = sqlite3_step(stmt);
		}
		sqlite3_finalize(stmt);
		stmt = NULL;
		if (rc != SQLITE_DONE) {
			MORPH_SET_ERR(rc, MORPH_ERR_DB);
			goto out_rollback_batch;
		}
	}
	rc = sqlite3_prepare_v2(manager->db->handle, trust ?
		"INSERT INTO hook_trust(id,digest,disabled,definition) VALUES(?,?,0,?) "
		"ON CONFLICT(id) DO UPDATE SET digest=excluded.digest,"
		"definition=excluded.definition,disabled=0" :
		"INSERT INTO hook_trust(id,digest,disabled,definition) VALUES(?,'',1,'') "
		"ON CONFLICT(id) DO UPDATE SET disabled=1", -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		MORPH_SET_ERR(rc, MORPH_ERR_DB);
		goto out_rollback_batch;
	}
	morph_array_foreach(entry, &manager->entries, struct hook_entry) {
		if (!entry->supported)
			continue;
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_bind_text(stmt, 1, entry->id, -1, SQLITE_TRANSIENT);
		if (trust) {
			sqlite3_bind_text(stmt, 2, entry->digest, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, 3, entry->definition, -1, SQLITE_TRANSIENT);
		}
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			MORPH_SET_ERR(rc, MORPH_ERR_DB);
			goto out_rollback_batch;
		}
	}
	sqlite3_finalize(stmt);
	stmt = NULL;
	rc = db_exec(manager->db, "RELEASE hook_batch;");
	if (rc == 0)
		return 0;
out_rollback_batch:
	sqlite3_finalize(stmt);
	(void)db_exec(manager->db, "ROLLBACK TO hook_batch; RELEASE hook_batch;");
	MORPH_RETURN(rc);
}

int hook_manager_action(struct hook_manager *manager, const char *action, const char *id,
			const char *expected_digest)
{
	struct hook_entry *entry;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!manager || !action || !id || !expected_digest)
		MORPH_RETURN(-EINVAL);
	if (strcmp(action, "trust-all") == 0 || strcmp(action, "deny-all") == 0)
		return hook_manager_batch(manager, strcmp(action, "trust-all") == 0,
			expected_digest);
	if (strcmp(action, "trust-project") == 0 || strcmp(action, "revoke-project") == 0) {
		char digest[65];
		digest_text(manager->workdir, digest);
		if (strcmp(expected_digest, digest) != 0)
			MORPH_RETURN(-ESTALE);
		rc =
		    sqlite3_prepare_v2(manager->db->handle,
				       strcmp(action, "trust-project") == 0
					   ? "INSERT OR IGNORE INTO hook_workspaces(path) VALUES(?)"
					   : "DELETE FROM hook_workspaces WHERE path=?",
				       -1, &stmt, NULL);
		if (rc == SQLITE_OK)
			sqlite3_bind_text(stmt, 1, manager->workdir, -1, SQLITE_TRANSIENT);
	} else {
		/* Reload immediately: a review token cannot approve an edited definition. */
		rc = hook_manager_reload(manager);
		if (rc != 0)
			return rc;
		entry = NULL;
		for (size_t i = 0; i < manager->entries.nelts; i++) {
			struct hook_entry *candidate = morph_array_get(&manager->entries, i);
			if (strcmp(candidate->id, id) == 0)
				entry = candidate;
		}
		if (!entry)
			MORPH_RETURN(-ENOENT);
		if (strcmp(entry->digest, expected_digest) != 0)
			MORPH_RETURN(-ESTALE);
		if (strcmp(action, "trust") == 0) {
			if (!entry->supported || (entry->project && !project_trusted(manager)))
				MORPH_RETURN(-EPERM);
			rc = sqlite3_prepare_v2(
			    manager->db->handle,
			    "INSERT INTO hook_trust(id,digest,disabled,definition) VALUES(?,?,0,?) "
			    "ON CONFLICT(id) DO UPDATE SET "
			    "digest=excluded.digest,definition=excluded.definition,disabled=0",
			    -1, &stmt, NULL);
			if (rc == SQLITE_OK) {
				sqlite3_bind_text(stmt, 2, entry->digest, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, 3, entry->definition, -1, SQLITE_TRANSIENT);
			}
		} else if (strcmp(action, "revoke") == 0) {
			rc = sqlite3_prepare_v2(manager->db->handle,
						"INSERT INTO hook_trust(id,digest,disabled,definition) VALUES(?,'',0,'') "
						"ON CONFLICT(id) DO UPDATE SET digest='',disabled=0,definition=''",
						-1, &stmt,
						NULL);
		} else if (strcmp(action, "disable") == 0 || strcmp(action, "enable") == 0) {
			rc = sqlite3_prepare_v2(
			    manager->db->handle,
			    strcmp(action, "disable") == 0
				? "INSERT INTO hook_trust(id,digest,disabled,definition) "
				  "VALUES(?,'',1,'') "
				  "ON CONFLICT(id) DO UPDATE SET disabled=1"
				: "UPDATE hook_trust SET disabled=0 WHERE id=?",
			    -1, &stmt, NULL);
			if (rc == SQLITE_OK)
				sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
		} else {
			MORPH_RETURN(-EINVAL);
		}
		if (rc == SQLITE_OK && strcmp(action, "disable") != 0 &&
		    strcmp(action, "enable") != 0)
			sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
	}
	if (rc == SQLITE_OK)
		rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	MORPH_RETURN(rc == SQLITE_DONE ? 0 : MORPH_ERR_DB);
}

static int hook_matches(struct hook_entry *entry, const char *event, const cJSON *input)
{
	const char *name;
	const char *alias = "";

	if (strcmp(entry->event, event) != 0)
		return 0;
	if (!entry->regex_ready || strcmp(event, "Stop") == 0)
		return 1;
	name = json_string(input, "tool_name");
	if (strcmp(event, "SessionStart") == 0)
		name = json_string(input, "source");
	if (strcmp(event, "SessionEnd") == 0)
		name = json_string(input, "reason");
	if (strcmp(name, "exec") == 0)
		alias = "Bash";
	if (regexec(&entry->regex, name, 0, NULL, 0) == 0 ||
	    (*alias && regexec(&entry->regex, alias, 0, NULL, 0) == 0))
		return 1;
	return strcmp(name, "apply_patch") == 0 &&
	       (regexec(&entry->regex, "Edit", 0, NULL, 0) == 0 ||
		regexec(&entry->regex, "Write", 0, NULL, 0) == 0);
}

int hook_manager_has(struct hook_manager *manager, const char *event)
{
	struct hook_entry *entry;

	if (!manager || !manager->ready || !manager->enabled)
		return 0;
	morph_array_foreach(entry, &manager->entries, struct hook_entry)
	{
		if (entry->supported && strcmp(entry->event, event) == 0 &&
		    refresh_trust(manager, entry) == 0 &&
		    strcmp(entry_state(manager, entry), "trusted") == 0)
			return 1;
	}
	return 0;
}

static void hook_emit(struct hook_manager *manager, struct hook_entry *entry, const char *name,
		      int rc, int blocked)
{
	cJSON *data = cJSON_CreateObject();

	if (!data)
		return;
	cJSON_AddStringToObject(data, "hook_id", entry->id);
	cJSON_AddStringToObject(data, "digest", entry->digest);
	cJSON_AddStringToObject(data, "event", entry->event);
	cJSON_AddStringToObject(data, "state", entry_state(manager, entry));
	cJSON_AddBoolToObject(data, "blocked", blocked);
	if (rc < 0) {
		cJSON_AddNumberToObject(data, "error_code", rc);
		cJSON_AddStringToObject(data, "error", morph_strerror(rc));
	}
	if (strcmp(name, "hook.started") != 0 && manager->recent) {
		cJSON_AddStringToObject(data, "name", name);
		if (cJSON_GetArraySize(manager->recent) >= 64)
			cJSON_DeleteItemFromArray(manager->recent, 0);
		cJSON_AddItemToArray(manager->recent, cJSON_Duplicate(data, 1));
	}
	(void)morph_event_emit_simple(
	    manager->event_cb, manager->event_user, MORPH_EVENT_BACKGROUND, name,
	    rc < 0 ? "failed" : "end",
	    strcmp(name, "hook.skipped") == 0 ? "Hook skipped; inspect /hooks"
	    : rc < 0			      ? "Hook failed; inspect /hooks"
	    : strcmp(name, "hook.started") == 0 && *entry->status_message ? entry->status_message
									  : "Hook lifecycle",
	    data);
	cJSON_Delete(data);
}

static int append_context(struct hook_manager *manager, morph_buf_t *context,
			  struct hook_entry *entry, const char *text)
{
	char path[PATH_MAX];
	char *directory = NULL;
	char *preview = NULL;
	const char *shown = text;
	int rc = 0;

	if (!text || !*text)
		return 0;
	if (utf8valid(text))
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	if (entry->context_limit > 0 && tokenizer_estimate_tokens(text) > entry->context_limit) {
		directory = file_expand_path(manager->config->general.output_dir);
		if (!directory)
			MORPH_RETURN(-ENOMEM);
		rc = file_ensure_dir(directory);
		if (rc == 0)
			rc = file_path_join(path, sizeof(path), directory, "hook-context-XXXXXX");
		free(directory);
		if (rc != 0)
			return rc;
		int fd = mkstemp(path);
		if (fd < 0)
			MORPH_RETURN_ERRNO();
		FILE *file = fdopen(fd, "w");
		if (!file) {
			int saved = -errno;
			close(fd);
			unlink(path);
			MORPH_RETURN(saved);
		}
		if (fwrite(text, 1, strlen(text), file) != strlen(text))
			rc = -EIO;
		if (fclose(file) != 0)
			rc = -EIO;
		if (rc != 0) {
			unlink(path);
			return rc;
		}
		/* The shared estimator uses tokens; clamp only at UTF-8 boundaries. */
		size_t bytes = strlen(text);
		preview = utf8_dup_clamped(text, bytes);
		if (!preview)
			MORPH_RETURN(-ENOMEM);
		while (tokenizer_estimate_tokens(preview) > entry->context_limit && bytes > 0) {
			bytes /= 2;
			preview[utf8_clamp_bytes(preview, bytes)] = '\0';
		}
		shown = preview;
	}
	if (context->len + strlen(shown) > 32768u) {
		free(preview);
		MORPH_RETURN(-E2BIG);
	}
	rc = morph_buf_printf(context, "[Hook %s / %s]\n%s\n", entry->id, entry->event, shown);
	if (rc == 0 && preview)
		rc = morph_buf_printf(context, "Full hook context: %s\n", path);
	free(preview);
	return rc;
}

static int decode_output(struct hook_manager *manager, struct hook_run *run, const char *event,
			 morph_buf_t *context, morph_buf_t *reason, cJSON **updated, int *blocked,
			 int *ask, int *terminate)
{
	const char *text = morph_buf_cstr(&run->out);
	const char *feedback = "";
	cJSON *root;
	const cJSON *specific;
	const cJSON *replacement;
	int deny = 0;
	int rc = 0;

	if (run->rc < 0)
		return run->rc;
	if (run->exit_code == 2) {
		feedback = morph_buf_cstr(&run->err);
		deny = strcmp(event, "PreToolUse") == 0 || strcmp(event, "Stop") == 0 ||
		       strcmp(event, "UserPromptSubmit") == 0 || strcmp(event, "PostToolUse") == 0;
		if (deny) {
			*blocked = 1;
			return morph_buf_printf(reason, "%s%s\n", feedback,
						*feedback ? "" : "Hook blocked this operation.");
		}
		MORPH_RETURN(MORPH_ERR_PROCESSING);
	}
	if (run->exit_code != 0)
		MORPH_RETURN(MORPH_ERR_PROCESSING);
	if (!*text)
		return 0;
	if (utf8valid(text))
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	root = cJSON_ParseWithOpts(text, NULL, 1);
	if (!root) {
		if (strcmp(event, "SessionStart") == 0 || strcmp(event, "UserPromptSubmit") == 0)
			return append_context(manager, context, run->entry, text);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	if (!cJSON_IsObject(root) || !json_unique_fields(root, 0)) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	specific = cJSON_GetObjectItemCaseSensitive(root, "hookSpecificOutput");
	if (specific && (!cJSON_IsObject(specific) ||
			 strcmp(json_string(specific, "hookEventName"), event) != 0)) {
		cJSON_Delete(root);
		MORPH_RETURN(MORPH_ERR_PROTOCOL);
	}
	if (strcmp(event, "PreToolUse") == 0) {
		const char *decision = json_string(specific, "permissionDecision");
		if (*decision && strcmp(decision, "deny") != 0 && strcmp(decision, "allow") != 0 &&
		    strcmp(decision, "ask") != 0)
			rc = MORPH_ERR_PROTOCOL;
		if (strcmp(decision, "ask") == 0)
			*ask = 1;
		deny = strcmp(decision, "deny") == 0 ||
		       strcmp(json_string(root, "decision"), "block") == 0;
		feedback = deny ? json_string(specific, "permissionDecisionReason") : "";
		if (!*feedback)
			feedback = json_string(root, "reason");
		replacement = cJSON_GetObjectItemCaseSensitive(specific, "updatedInput");
		if (replacement) {
			if (strcmp(decision, "allow") != 0 || !cJSON_IsObject(replacement)) {
				rc = MORPH_ERR_PROTOCOL;
			} else if (*updated && !cJSON_Compare(*updated, replacement, 1)) {
				/* Never execute a nondeterministically selected rewrite. */
				deny = 1;
				feedback = "Conflicting hook input rewrites.";
			} else if (!*updated) {
				*updated = cJSON_Duplicate(replacement, 1);
				if (!*updated)
					rc = -ENOMEM;
			}
		}
		if (cJSON_GetObjectItemCaseSensitive(root, "continue") ||
		    cJSON_GetObjectItemCaseSensitive(root, "stopReason"))
			rc = MORPH_ERR_PROTOCOL;
	} else if (strcmp(event, "Stop") == 0 || strcmp(event, "UserPromptSubmit") == 0 ||
		   strcmp(event, "PostToolUse") == 0) {
		const cJSON *cont = cJSON_GetObjectItemCaseSensitive(root, "continue");
		deny = strcmp(json_string(root, "decision"), "block") == 0 || cJSON_IsFalse(cont);
		if (strcmp(event, "Stop") == 0 && cJSON_IsFalse(cont))
			*terminate = 1;
		feedback = json_string(root, "reason");
		if (!*feedback)
			feedback = json_string(root, "stopReason");
	}
	if (rc == 0)
		rc = append_context(manager, context, run->entry,
				    json_string(specific, "additionalContext"));
	if (rc == 0 && deny) {
		*blocked = 1;
		rc = morph_buf_printf(reason, "%s\n",
				      *feedback ? feedback : "Hook blocked this operation.");
	}
	cJSON_Delete(root);
	return rc;
}

static int collect_run(struct process_manager *processes, struct hook_run *run)
{
	struct process_snapshot snapshot = {0};
	int rc = process_session_snapshot(processes, run->session, &snapshot);

	if (rc != 0)
		return rc;
	if (snapshot.stdout_truncated || snapshot.stderr_truncated ||
	    run->out.len + strlen(snapshot.stdout_text ? snapshot.stdout_text : "") >
		HOOK_OUTPUT_MAX ||
	    run->err.len + strlen(snapshot.stderr_text ? snapshot.stderr_text : "") >
		HOOK_OUTPUT_MAX) {
		rc = -E2BIG;
	} else {
		rc = morph_buf_puts(&run->out, snapshot.stdout_text ? snapshot.stdout_text : "");
		if (rc == 0)
			rc = morph_buf_puts(&run->err,
					    snapshot.stderr_text ? snapshot.stderr_text : "");
	}
	if (snapshot.state >= PROCESS_EXITED) {
		run->done = 1;
		run->exit_code = snapshot.exit_code;
		if (snapshot.state == PROCESS_TIMED_OUT)
			rc = -ETIMEDOUT;
		else if (snapshot.state != PROCESS_EXITED)
			rc = -ECANCELED;
	}
	run->duration_ms = snapshot.duration_ms;
	process_snapshot_cleanup(&snapshot);
	return rc;
}

static uint64_t hook_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int hook_manager_dispatch(void *opaque, const char *event, const cJSON *input, cJSON **output)
{
	struct hook_manager *manager = opaque;
	struct process_manager *processes = NULL;
	struct process_manager_config pcfg = {
	    .max_session_output = HOOK_OUTPUT_MAX,
	    .max_inline_output = HOOK_OUTPUT_MAX,
	    .kill_grace_ms = 100,
	    .shell = "/bin/sh",
	};
	morph_array_t runs = {0};
	morph_buf_t context = {0}, reason = {0};
	struct hook_entry *entry;
	cJSON *payload = NULL, *updated = NULL;
	char *wire = NULL;
	int rc = 0, blocked = 0, ask = 0, terminate = 0;
	uint64_t started = hook_now_ms();
	uint64_t elapsed;

	if (!manager || !output || !event)
		MORPH_RETURN(-EINVAL);
	*output = NULL;
	if (!manager->ready || !manager->enabled)
		return 0;
	int matching = 0;
	morph_array_foreach(entry, &manager->entries, struct hook_entry)
	{
		if (!hook_matches(entry, event, input))
			continue;
		if (refresh_trust(manager, entry) == 0 && entry->supported &&
		    strcmp(entry_state(manager, entry), "trusted") == 0)
			matching++;
		else
			hook_emit(manager, entry, "hook.skipped", 0, 0);
	}
	if (!matching) {
		if (strcmp(event, "UserPromptSubmit") == 0 && manager->session_context) {
			*output = cJSON_CreateObject();
			if (!*output ||
			    !cJSON_AddStringToObject(*output, "context", manager->session_context))
				MORPH_RETURN(-ENOMEM);
		}
		return 0;
	}
	if ((manager->cancelled && *manager->cancelled) ||
	    morph_cancel_token_is_cancelled(manager->cancel_token))
		MORPH_RETURN(-ECANCELED);
	payload = input ? cJSON_Duplicate(input, 1) : cJSON_CreateObject();
	if (!payload)
		MORPH_RETURN(-ENOMEM);
	char sid[32];
	snprintf(sid, sizeof(sid), "%lld", (long long)manager->session_id);
	cJSON_AddStringToObject(payload, "session_id", sid);
	cJSON_AddNullToObject(payload, "transcript_path");
	cJSON_AddStringToObject(payload, "cwd", manager->workdir);
	cJSON_AddStringToObject(payload, "hook_event_name", event);
	cJSON_AddStringToObject(payload, "model", manager->model ? manager->model : "");
	cJSON_AddStringToObject(payload, "turn_id", manager->turn_id ? manager->turn_id : "");
	cJSON_AddStringToObject(payload, "permission_mode", "default");
	wire = cJSON_PrintUnformatted(payload);
	if (!wire || strlen(wire) > HOOK_INPUT_MAX) {
		MORPH_SET_ERR(rc, wire ? -E2BIG : -ENOMEM);
		goto out_cleanup;
	}
	rc = morph_array_init(&runs, 8, sizeof(struct hook_run));
	if (rc == 0)
		rc = morph_buf_init(&context, 256);
	if (rc == 0 && strcmp(event, "UserPromptSubmit") == 0 && manager->session_context)
		rc = morph_buf_puts(&context, manager->session_context);
	if (rc == 0)
		rc = morph_buf_init(&reason, 256);
	if (rc == 0)
		rc = process_manager_create(&pcfg, &processes);
	if (rc != 0)
		goto out_cleanup;
	morph_array_foreach(entry, &manager->entries, struct hook_entry)
	{
		struct hook_run *run;
		FILE *stdin_file;
		struct process_spawn_options options = {.logger = manager->logger};

		if (!hook_matches(entry, event, input))
			continue;
		if (refresh_trust(manager, entry) != 0 || !entry->supported ||
		    strcmp(entry_state(manager, entry), "trusted") != 0) {
			continue;
		}
		if (manager->remaining_ms == 0) {
			hook_emit(manager, entry, "hook.failed", -ETIMEDOUT, 0);
			continue;
		}
		run = morph_array_push(&runs);
		if (!run) {
			MORPH_SET_ERR(rc, -ENOMEM);
			goto out_cleanup;
		}
		memset(run, 0, sizeof(*run));
		run->entry = entry;
		run->rc = morph_buf_init(&run->out, 256);
		if (run->rc == 0)
			run->rc = morph_buf_init(&run->err, 256);
		stdin_file = tmpfile();
		if (!stdin_file) {
			run->rc = -errno;
		} else if (run->rc == 0) {
			if (fwrite(wire, 1, strlen(wire), stdin_file) != strlen(wire) ||
			    fflush(stdin_file) != 0 || fseek(stdin_file, 0, SEEK_SET) != 0)
				run->rc = -EIO;
			if (run->rc == 0) {
				options.command = entry->command;
				options.workdir = manager->workdir;
				options.timeout_ms = (uint64_t)entry->timeout * 1000u;
				if (options.timeout_ms > manager->remaining_ms)
					options.timeout_ms = manager->remaining_ms;
				if (strcmp(event, "SessionEnd") == 0 && options.timeout_ms > 1000)
					options.timeout_ms = 1000;
				options.background = 1;
				options.hook_mode = 1;
				options.use_stdin_fd = 1;
				options.stdin_fd = fileno(stdin_file);
				hook_emit(manager, entry, "hook.started", 0, 0);
				run->rc = process_spawn(processes, &options, run->session, NULL);
			}
		}
		if (stdin_file)
			fclose(stdin_file);
		if (run->rc != 0)
			run->done = 1;
	}
	for (;;) {
		int pending = 0;
		struct hook_run *run;
		int cancelled = (manager->cancelled && *manager->cancelled) ||
				morph_cancel_token_is_cancelled(manager->cancel_token);

		morph_array_foreach(run, &runs, struct hook_run)
		{
			if (run->done)
				continue;
			if (cancelled) {
				(void)process_session_kill(processes, run->session);
				run->rc = -ECANCELED;
				run->done = 1;
				continue;
			}
			run->rc = collect_run(processes, run);
			if (run->rc < 0) {
				(void)process_session_kill(processes, run->session);
				run->done = 1;
			}
			pending += !run->done;
		}
		if (cancelled) {
			MORPH_SET_ERR(rc, -ECANCELED);
			goto out_cleanup;
		}
		if (!pending)
			break;
		struct timespec delay = {.tv_nsec = 10000000};
		nanosleep(&delay, NULL);
	}
	struct hook_run *run;
	morph_array_foreach(run, &runs, struct hook_run)
	{
		int decision = 0;
		size_t context_before = context.len, reason_before = reason.len;
		cJSON *updated_before = updated;
		int ask_before = ask, terminate_before = terminate;
		int decode_rc = decode_output(manager, run, event, &context, &reason, &updated,
					      &decision, &ask, &terminate);
		if (context.len + manager->context_bytes > 32768u || reason.len > 32768u)
			decode_rc = -E2BIG;
		if (decode_rc < 0) {
			(void)morph_buf_truncate(&context, context_before);
			(void)morph_buf_truncate(&reason, reason_before);
			if (updated != updated_before)
				cJSON_Delete(updated);
			updated = updated_before;
			ask = ask_before;
			terminate = terminate_before;
			decision = 0;
		}
		blocked |= decision;
		if (decode_rc == 0) {
			cJSON *parsed = cJSON_Parse(morph_buf_cstr(&run->out));
			const char *message = json_string(parsed, "systemMessage");
			if (*message)
				(void)morph_event_emit_simple(
				    manager->event_cb, manager->event_user, MORPH_EVENT_BACKGROUND,
				    "hook.message", "end", message, NULL);
			cJSON_Delete(parsed);
		}
		hook_emit(manager, run->entry,
			  decode_rc < 0 ? "hook.failed"
			  : decision	? "hook.blocked"
					: "hook.completed",
			  decode_rc, decision);
		cJSON *last =
		    cJSON_GetArrayItem(manager->recent, cJSON_GetArraySize(manager->recent) - 1);
		cJSON_AddNumberToObject(last, "duration_ms", (double)run->duration_ms);
		cJSON_AddNumberToObject(last, "exit_code", run->exit_code);
		char *diagnostic = utf8_dup_clamped(morph_buf_cstr(&run->err), 16384);
		if (diagnostic) {
			cJSON_AddStringToObject(last, "stderr", diagnostic);
			free(diagnostic);
		}
	}
	manager->context_bytes += context.len;
	*output = cJSON_CreateObject();
	if (!*output) {
		MORPH_SET_ERR(rc, -ENOMEM);
		goto out_cleanup;
	}
	cJSON_AddStringToObject(*output, "context", morph_buf_cstr(&context));
	cJSON_AddStringToObject(*output, "reason", morph_buf_cstr(&reason));
	cJSON_AddBoolToObject(*output, "blocked", blocked);
	cJSON_AddBoolToObject(*output, "ask", ask);
	cJSON_AddBoolToObject(*output, "terminate", terminate);
	if (updated) {
		cJSON_AddItemToObject(*output, "updatedInput", updated);
		updated = NULL;
	}

out_cleanup:
	elapsed = hook_now_ms() - started;
	manager->remaining_ms =
	    elapsed < manager->remaining_ms ? manager->remaining_ms - elapsed : 0;
	process_manager_destroy(processes);
	for (size_t i = 0; i < runs.nelts; i++) {
		struct hook_run *item = morph_array_get(&runs, i);
		morph_buf_cleanup(&item->out);
		morph_buf_cleanup(&item->err);
	}
	morph_array_cleanup(&runs);
	morph_buf_cleanup(&context);
	morph_buf_cleanup(&reason);
	cJSON_Delete(payload);
	cJSON_Delete(updated);
	free(wire);
	MORPH_RETURN(rc);
}
