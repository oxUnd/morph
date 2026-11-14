#include "runtime/hooks.h"
#include "nb/cleanup.h"
#include "runtime/hooks_internal.h"

#include "nb/buf.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/log.h"
#include "cJSON.h"
#include "nb/hash.h"
#include "nb/encoding.h"
#include "runtime/runtime.h"
#include "nb/utf8.h"

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *hook_json_string_borrow(const cJSON *obj, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);

	return cJSON_IsString(value) ? value->valuestring : "";
}

static void digest_text(const char *text, char out[65])
{
	unsigned char hash[NB_SHA256_DIGEST_SIZE];
	nb_sha256(text, strlen(text), hash);
	(void)nb_hex_encode_to(hash, sizeof(hash), false, out, 65, NULL);
}

void runtime_hooks_workspace_token(struct runtime *runtime, char out[65])
{
	digest_text(runtime_workdir_borrow(runtime), out);
}

static int event_supported(const char *event)
{
	return strcmp(event, "UserPromptSubmit") == 0 || strcmp(event, "PreToolUse") == 0 ||
	       strcmp(event, "PostToolUse") == 0 || strcmp(event, "Stop") == 0 ||
	       strcmp(event, "SessionStart") == 0 || strcmp(event, "SessionEnd") == 0 ||
	       strcmp(event, "PreCompact") == 0 || strcmp(event, "PostCompact") == 0 ||
	       strcmp(event, "Interrupt") == 0;
}

int hook_event_ignores_matcher(const char *event)
{
	return strcmp(event, "Stop") == 0 || strcmp(event, "UserPromptSubmit") == 0 ||
	       strcmp(event, "Interrupt") == 0;
}

static int event_short_timeout(const char *event)
{
	return strcmp(event, "SessionEnd") == 0 || strcmp(event, "Interrupt") == 0;
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

int hook_refresh_trust(struct hook_manager *manager, struct hook_entry *entry)
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
		return MORPH_ERR_DB;
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
		return MORPH_ERR_DB;
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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	struct hook_entry *entry;
	nb_buf identity NB_CLEANUP(nb_buf_cleanup) = {0};
	cJSON *canonical = cJSON_CreateObject();
	const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(handler, "timeout");
	const cJSON *limit = cJSON_GetObjectItemCaseSensitive(handler, "additionalContextLimit");
	int valid = cJSON_IsObject(handler);
	int rc;

	if (!valid) {
		cJSON_Delete(canonical);
		return MORPH_ERR_CONFIG;
	}
	if (!canonical || manager->entries.len >= HOOK_LIMIT) {
		cJSON_Delete(canonical);
		return canonical ? NB_ERR_TOO_BIG : NB_ERR_NOMEM;
	}
	entry = nb_vec_push(&manager->entries);
	if (!entry) {
		cJSON_Delete(canonical);
		return NB_ERR_NOMEM;
	}
	memset(entry, 0, sizeof(*entry));
	entry->source =
		(nb_path_expand(&file_value_1, source, NULL) == NB_OK &&
				 nb_path_canonical_missing(
					 &file_value_1, nb_buf_cstr(&file_value_1), NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_1))
			 : NULL);
	if (!entry->source)
		entry->source = strdup(source);
	entry->event = strdup(event);
	entry->matcher = strdup(matcher);
	entry->command = strdup(hook_json_string_borrow(handler, "command"));
	entry->status_message = strdup(hook_json_string_borrow(handler, "statusMessage"));
	entry->project = project;
	entry->timeout = cJSON_IsNumber(timeout) ? timeout->valueint
						 : (event_short_timeout(event) ? 1 : 600);
	entry->context_limit = cJSON_IsNumber(limit) ? limit->valueint : 2500;
	if (!entry->source || !entry->event || !entry->matcher || !entry->command ||
	    !entry->status_message) {
		cJSON_Delete(canonical);
		return NB_ERR_NOMEM;
	}
	if ((timeout &&
	     (!cJSON_IsNumber(timeout) || timeout->valuedouble != (double)timeout->valueint)) ||
	    entry->timeout < 1 || entry->timeout > 600 ||
	    (event_short_timeout(event) && entry->timeout > 3) ||
	    (limit && (!cJSON_IsNumber(limit) || limit->valuedouble != (double)limit->valueint)) ||
	    entry->context_limit < 0 || entry->context_limit > 32768 || strlen(matcher) > 1024 ||
	    strlen(entry->command) >= PROCESS_COMMAND_MAX ||
	    (!entry->command[0] &&
	     strcmp(hook_json_string_borrow(handler, "type"), "command") == 0) ||
	    !*hook_json_string_borrow(handler, "type"))
		valid = 0;
	if (*matcher && strcmp(matcher, "*") != 0 && !hook_event_ignores_matcher(event)) {
		entry->regex_ready = regcomp(&entry->regex, matcher, REG_EXTENDED | REG_NOSUB) == 0;
		if (!entry->regex_ready)
			valid = 0;
	}
	/* Stable field order makes formatting and JSON key order irrelevant. */
	if (!cJSON_AddStringToObject(canonical, "event", event) ||
	    !cJSON_AddStringToObject(canonical, "matcher", matcher) ||
	    !cJSON_AddStringToObject(canonical, "type", hook_json_string_borrow(handler, "type")) ||
	    !cJSON_AddStringToObject(canonical, "command", entry->command) ||
	    !cJSON_AddStringToObject(canonical, "statusMessage",
				     hook_json_string_borrow(handler, "statusMessage")) ||
	    !cJSON_AddNumberToObject(canonical, "timeout", entry->timeout) ||
	    !cJSON_AddNumberToObject(canonical, "additionalContextLimit", entry->context_limit)) {
		cJSON_Delete(canonical);
		return NB_ERR_NOMEM;
	}
	entry->definition = cJSON_PrintUnformatted(canonical);
	cJSON_Delete(canonical);
	if (!entry->definition)
		return NB_ERR_NOMEM;
	rc = (nb_buf_init(&identity), nb_buf_reserve(&identity, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&identity, "%s\n%s\n%s\n%d\n%d",
				    project ? manager->workdir : "user", entry->source, event,
				    group_index, handler_index)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0) {
		digest_text(nb_buf_cstr(&identity), entry->id);
		nb_buf_clear(&identity);
		rc = (nb_buf_printf(&identity, "%s\n%s", entry->id, entry->definition)
			      ? 0
			      : NB_ERR_NOMEM);
	}
	if (rc == 0)
		digest_text(nb_buf_cstr(&identity), entry->digest);
	nb_buf_cleanup(&identity);
	if (rc != 0)
		return rc;
	int portable = 1;
	const cJSON *field;
	cJSON_ArrayForEach (field, handler) {
		if (strcmp(field->string, "type") != 0 && strcmp(field->string, "command") != 0 &&
		    strcmp(field->string, "timeout") != 0 &&
		    strcmp(field->string, "statusMessage") != 0 &&
		    strcmp(field->string, "additionalContextLimit") != 0)
			portable = 0;
	}
	entry->supported = portable && valid && event_supported(event) &&
			   strcmp(hook_json_string_borrow(handler, "type"), "command") == 0;
	entry->diagnostic = !valid ? "Invalid command, timeout, context limit, or matcher regex."
			    : !event_supported(event)
				    ? "This lifecycle event is not supported by Morph."
			    : strcmp(hook_json_string_borrow(handler, "type"), "command") != 0
				    ? "Only command handlers are supported."
			    : !portable ? "Handler contains unsupported fields (for example async "
					  "or commandWindows)."
					: "";
	entry->status = strdup(!valid ? "invalid" : !entry->supported ? "unsupported" : "");
	if (!entry->status)
		return NB_ERR_NOMEM;
	rc = hook_refresh_trust(manager, entry);
	if (rc == 0 && !project && !entry->recorded) {
		char legacy_id[65];
		char legacy_digest[65];
		nb_buf legacy NB_CLEANUP(nb_buf_cleanup) = {0};
		sqlite3_stmt *stmt = NULL;

		rc = (nb_buf_init(&legacy), nb_buf_reserve(&legacy, 128) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		rc = (nb_buf_printf(&legacy, "%s\n%s\n%s\n%d\n%d", manager->workdir, source, event,
				    group_index, handler_index)
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc == 0) {
			digest_text(nb_buf_cstr(&legacy), legacy_id);
			nb_buf_clear(&legacy);
			rc = (nb_buf_printf(&legacy, "%s\n%s", legacy_id, entry->definition)
				      ? 0
				      : NB_ERR_NOMEM);
		}
		if (rc == 0)
			digest_text(nb_buf_cstr(&legacy), legacy_digest);
		nb_buf_cleanup(&legacy);
		if (rc != 0)
			return rc;
		rc = sqlite3_prepare_v2(
			manager->db->handle,
			"INSERT OR IGNORE INTO hook_trust(id,digest,disabled,definition) "
			"SELECT ?,CASE WHEN digest=? THEN ? ELSE '' END,disabled,definition "
			"FROM hook_trust "
			"WHERE id=? AND (digest=? OR disabled=1)",
			-1, &stmt, NULL);
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
			return MORPH_ERR_DB;
		rc = hook_refresh_trust(manager, entry);
	}
	return rc;
}

int hook_json_unique_fields(const cJSON *value, int depth)
{
	const cJSON *child;

	if (depth > 16)
		return 0;
	cJSON_ArrayForEach (child, value) {
		if (cJSON_IsObject(value)) {
			for (const cJSON *next = child->next; next; next = next->next) {
				if (strcmp(child->string, next->string) == 0)
					return 0;
			}
		}
		if ((cJSON_IsObject(child) || cJSON_IsArray(child)) &&
		    !hook_json_unique_fields(child, depth + 1))
			return 0;
	}
	return 1;
}

static int load_object(struct hook_manager *manager, const cJSON *hooks, const char *source,
		       int project)
{
	const cJSON *event;

	if (!cJSON_IsObject(hooks) || !hook_json_unique_fields(hooks, 0))
		return MORPH_ERR_CONFIG;
	cJSON_ArrayForEach (event, hooks) {
		const cJSON *group;
		int gi = 0;

		if (!cJSON_IsArray(event))
			return MORPH_ERR_CONFIG;
		cJSON_ArrayForEach (group, event) {
			const cJSON *handlers = cJSON_GetObjectItemCaseSensitive(group, "hooks");
			const cJSON *matcher = cJSON_GetObjectItemCaseSensitive(group, "matcher");
			const cJSON *handler;
			int hi = 0;

			if (!cJSON_IsObject(group) || !cJSON_IsArray(handlers) ||
			    (matcher && !cJSON_IsString(matcher)))
				return MORPH_ERR_CONFIG;
			cJSON_ArrayForEach (handler, handlers) {
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

static int hook_load_error(struct hook_manager *manager, const char *path, int rc,
			   const char *message)
{
	snprintf(manager->error.file, sizeof(manager->error.file), "%s", path);
	if (message != manager->error.detail.message)
		snprintf(manager->error.detail.message, sizeof(manager->error.detail.message), "%s",
			 message ? message
				 : nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	if (manager->error.detail.code == CONFIG_VALIDATION_NONE)
		manager->error.detail.code =
			rc == NB_ERR_PARSE ? CONFIG_VALIDATION_SYNTAX : CONFIG_VALIDATION_VALUE;
	nb_log_write(manager ? manager->logger : NULL, NB_LOG_ERROR,
		     "failed to load hooks %s:%d:%d: %s", path, manager->error.detail.line,
		     manager->error.detail.column, manager->error.detail.message);
	return rc;
}

static void hook_json_error_position(struct hook_manager *manager, const char *text,
				     const char *position)
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
	manager->error.detail.column =
		(int)nb_utf8_count(nb_str_from_parts(line_start, (size_t)(position - line_start))) +
		1;
}

static int load_json_file(struct hook_manager *manager, const char *path, int project)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;

	struct stat st;
	char *text;
	cJSON *root;
	int rc;
	const char *parse_end = NULL;
	nb_buf canonical NB_CLEANUP(nb_buf_destroy) = {0};

	if (nb_path_canonical(&canonical, path, NULL) == NB_OK)
		path = nb_buf_cstr(&canonical);
	struct hook_entry *existing;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (existing = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++) {
		if (strcmp(existing->source, path) == 0)
			return 0;
	}
	if (stat(path, &st) != 0) {
		rc = nb_status_from_errno(errno, NULL);
		return rc == NB_ERR_NOT_FOUND ? 0 : hook_load_error(manager, path, rc, NULL);
	}
	if (!S_ISREG(st.st_mode) || st.st_size > CONFIG_HOOKS_JSON_MAX)
		return hook_load_error(
			manager, path, NB_ERR_TOO_BIG,
			"Hooks configuration must be a regular file within the size limit.");
	text = (nb_file_read_all(path, &file_value_2, 0, NULL) == NB_OK &&
				(file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) + 1)) !=
					NULL
			? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
				  nb_buf_len(&file_value_2) + 1),
			   nb_charp_steal(&file_copy_2))
			: NULL);
	if (!text)
		return hook_load_error(manager, path, NB_ERR_IO, NULL);
	root = cJSON_ParseWithOpts(text, &parse_end, 1);
	if (!root) {
		hook_json_error_position(manager, text, parse_end);
		rc = hook_load_error(
			manager, path, NB_ERR_PARSE,
			parse_end && *parse_end
				? "Invalid JSON syntax; check quotes, commas, and brackets."
				: "Unexpected end of JSON input; expected a complete JSON "
				  "document.");
		free(text);
		return rc;
	}
	free(text);
	rc = load_object(manager, cJSON_GetObjectItemCaseSensitive(root, "hooks"), path, project);
	cJSON_Delete(root);
	if (rc != 0)
		return hook_load_error(manager, path, rc,
				       rc == MORPH_ERR_CONFIG ? "Invalid hooks structure: expected "
								"a hooks object with event arrays, "
								"hook groups, and valid handlers."
							      : NULL);
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
		for (size_t _i_ = 0;
		     _i_ < nb_vec_len(&manager->entries) &&
		     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
		     _i_++) {
			const char *source_slash = strrchr(entry->source, '/');
			if (source_slash &&
			    (size_t)(source_slash - entry->source) == (size_t)(slash - path) &&
			    strncmp(entry->source, path, (size_t)(slash - path)) == 0) {
				nb_log_write(manager ? manager->logger : NULL, NB_LOG_WARN,
					     "Both hooks.json and inline hooks found; merging: %s",
					     path);
				break;
			}
		}
	}
	hooks = cJSON_ParseWithOpts(json, NULL, 1);
	if (!hooks)
		return hook_load_error(manager, path, NB_ERR_PARSE, "Invalid inline hooks JSON.");
	rc = load_object(manager, hooks, path, project);
	cJSON_Delete(hooks);
	if (rc != 0)
		return hook_load_error(manager, path, rc, "Invalid inline hooks structure.");
	return 0;
}

int hook_manager_reload(struct hook_manager *manager)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_4 = NB_OK;
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_5 = NB_OK;
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_6 = NB_OK;

	struct hook_entry *entry;
	char path[PATH_MAX];
	char *home = (nb_path_expand(&file_value_3, "~/.morph/hooks.json", NULL) == NB_OK
			      ? strdup(nb_buf_cstr(&file_value_3))
			      : NULL);
	int rc;
	int enabled = manager ? manager->config->hooks_enabled : 0;

	if (!manager || !manager->ready || !home) {
		free(home);
		return NB_ERR_INVAL;
	}
	memset(&manager->error, 0, sizeof(manager->error));
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++)
		entry_cleanup(entry);
	nb_vec_clear(&manager->entries);
	rc = load_json_file(manager, home, 0);
	free(home);
	if (rc == 0 && manager->config_path[0]) {
		char *directory = strdup(manager->config_path);
		if (!directory)
			return NB_ERR_NOMEM;
		char *slash = strrchr(directory, '/');
		if (slash)
			*slash = '\0';
		else
			strcpy(directory, ".");
		rc = ((file_status_4 = nb_path_concat(&file_value_4, *directory ? directory : "/",
						      "hooks.json")) != NB_OK
			      ? file_status_4
		      : (file_status_4 = nb_str_copy_to(nb_buf_str(&file_value_4), path,
							sizeof(path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_4);
		free(directory);
		if (rc == 0)
			rc = load_json_file(manager, path, 0);
	}
	if (rc == 0 && manager->config_path[0] && nb_file_exists(manager->config_path)) {
		struct config *fresh = calloc(1, sizeof(*fresh));
		if (!fresh)
			return NB_ERR_NOMEM;
		rc = config_load(manager ? manager->logger : NULL, fresh, manager->config_path);
		if (rc != 0) {
			(void)config_validate_file(manager->config_path, &manager->error.detail);
			hook_load_error(manager, manager->config_path, rc,
					manager->error.detail.message[0]
						? manager->error.detail.message
						: NULL);
		}
		enabled = fresh->hooks_enabled;
		if (rc == 0)
			rc = load_inline(manager, manager->config_path, fresh->hooks_json, 0);
		free(fresh);
	} else if (rc == 0) {
		rc = load_inline(manager, manager->config_path, manager->config->hooks_json, 0);
	}
	if (rc == 0)
		rc = ((file_status_5 = nb_path_concat(&file_value_5, manager->workdir,
						      ".morph/hooks.json")) != NB_OK
			      ? file_status_5
		      : (file_status_5 = nb_str_copy_to(nb_buf_str(&file_value_5), path,
							sizeof(path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_5);
	if (rc == 0)
		rc = load_json_file(manager, path, 1);
	/* Project TOML is parsed with the normal schema, never applied to runtime. */
	if (rc == 0)
		rc = ((file_status_6 = nb_path_concat(&file_value_6, manager->workdir,
						      ".morph/config.toml")) != NB_OK
			      ? file_status_6
		      : (file_status_6 = nb_str_copy_to(nb_buf_str(&file_value_6), path,
							sizeof(path))) == NB_ERR_RANGE
			      ? NB_ERR_NAME_TOO_LONG
			      : file_status_6);
	if (rc == 0 && nb_file_exists(path) && strcmp(path, manager->config_path) != 0) {
		struct config *project = calloc(1, sizeof(*project));
		if (!project)
			return NB_ERR_NOMEM;
		rc = config_load(manager ? manager->logger : NULL, project, path);
		if (rc != 0) {
			(void)config_validate_file(path, &manager->error.detail);
			hook_load_error(manager, path, rc,
					manager->error.detail.message[0]
						? manager->error.detail.message
						: NULL);
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
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {0};

	int rc;

	if (!manager || !db || !db->handle || !config || !workdir)
		return NB_ERR_INVAL;
	memset(manager, 0, sizeof(*manager));
	manager->logger = config->logger;
	manager->db = db;
	manager->config = config;
	snprintf(manager->config_path, sizeof(manager->config_path), "%s",
		 config_path ? config_path : "");
	char *resolved =
		(nb_path_expand(&file_value_7, workdir, NULL) == NB_OK &&
				 nb_path_canonical_missing(
					 &file_value_7, nb_buf_cstr(&file_value_7), NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_7))
			 : NULL);
	snprintf(manager->workdir, sizeof(manager->workdir), "%s", resolved ? resolved : workdir);
	free(resolved);
	rc = db_exec(db,
		     "CREATE TABLE IF NOT EXISTS hook_trust("
		     "id TEXT PRIMARY KEY,digest TEXT NOT NULL,disabled INTEGER NOT NULL DEFAULT 0,"
		     "definition TEXT NOT NULL); CREATE TABLE IF NOT EXISTS hook_workspaces("
		     "path TEXT PRIMARY KEY)");
	if (rc == 0)
		rc = (nb_vec_init(&manager->entries, sizeof(struct hook_entry)),
		      nb_vec_reserve(&manager->entries, 8) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	manager->recent = cJSON_CreateArray();
	if (!manager->recent) {
		nb_vec_cleanup(&manager->entries);
		return NB_ERR_NOMEM;
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
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++)
		entry_cleanup(entry);
	nb_vec_cleanup(&manager->entries);
	free(manager->session_context);
	cJSON_Delete(manager->recent);
	memset(manager, 0, sizeof(*manager));
}

const char *hook_entry_state(struct hook_manager *manager, struct hook_entry *entry)
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
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	struct hook_entry *entry;
	int rc = (nb_buf_init(&text), nb_buf_reserve(&text, 256) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	rc = (nb_buf_printf(&text, "%s\n%d\n", manager->workdir, manager->enabled) ? 0
										   : NB_ERR_NOMEM);
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++) {
		if (rc == 0)
			rc = (nb_buf_printf(&text, "%s:%s:%d\n", entry->id, entry->digest,
					    entry->supported)
				      ? 0
				      : NB_ERR_NOMEM);
	}
	if (rc == 0)
		digest_text(nb_buf_cstr(&text), digest);
	nb_buf_cleanup(&text);
	return rc;
}

cJSON *hook_manager_list(struct hook_manager *manager)
{
	cJSON *list NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
	struct hook_entry *entry;
	char review_digest[65];

	if (!manager || !manager->ready || !list || hook_review_digest(manager, review_digest) != 0)
		return NULL;
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++) {
		cJSON *row NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		char workspace_digest[65];

		if (!row || hook_refresh_trust(manager, entry) != 0)
			return NULL;
		digest_text(manager->workdir, workspace_digest);
		if (!cJSON_AddStringToObject(row, "review_digest", review_digest) ||
		    !cJSON_AddBoolToObject(row, "project", entry->project) ||
		    !cJSON_AddStringToObject(row, "id", entry->id) ||
		    !cJSON_AddStringToObject(row, "digest", entry->digest) ||
		    !cJSON_AddStringToObject(row, "source", entry->source) ||
		    !cJSON_AddStringToObject(row, "event", entry->event) ||
		    !cJSON_AddStringToObject(row, "matcher", entry->matcher) ||
		    !cJSON_AddStringToObject(row, "command", entry->command) ||
		    !cJSON_AddStringToObject(row, "definition", entry->definition) ||
		    !cJSON_AddStringToObject(row, "previous_definition",
					     entry->previous ? entry->previous : "") ||
		    !cJSON_AddStringToObject(row, "state", hook_entry_state(manager, entry)) ||
		    !cJSON_AddStringToObject(row, "diagnostic", entry->diagnostic) ||
		    !cJSON_AddStringToObject(row, "statusMessage", entry->status_message) ||
		    !cJSON_AddNumberToObject(row, "additionalContextLimit", entry->context_limit) ||
		    !cJSON_AddStringToObject(row, "cwd", manager->workdir) ||
		    !cJSON_AddStringToObject(row, "workspace_digest", workspace_digest) ||
		    !cJSON_AddBoolToObject(row, "workspace_trusted", project_trusted(manager)) ||
		    !cJSON_AddNumberToObject(row, "timeout", entry->timeout))
			return NULL;
		const cJSON *last;
		cJSON_ArrayForEach (last, manager->recent) {
			if (strcmp(hook_json_string_borrow(last, "hook_id"), entry->id) == 0) {
				cJSON *run NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(last, 1);

				cJSON_DeleteItemFromObject(row, "last_run");
				if (!run || (cJSON_AddItemToObject(row, "last_run", run)
						     ? (run = NULL, NB_OK)
						     : NB_ERR_NOMEM) != 0)
					return NULL;
			}
		}
		if ((cJSON_AddItemToArray(list, row) ? (row = NULL, NB_OK) : NB_ERR_NOMEM) != 0)
			return NULL;
	}
	cJSON *result = list;
	list = NULL;
	return result;
}

static void hook_batch_stmt_cleanup(sqlite3_stmt **statement)
{
	sqlite3_finalize(*statement);
	*statement = NULL;
}

static int hook_manager_batch_write(struct hook_manager *manager, int trust)
{
	struct hook_entry *entry;
	sqlite3_stmt *stmt NB_CLEANUP(hook_batch_stmt_cleanup) = NULL;
	int rc;

	if (trust) {
		rc = sqlite3_prepare_v2(manager->db->handle,
					"INSERT OR IGNORE INTO hook_workspaces(path) VALUES(?)", -1,
					&stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, manager->workdir, -1, SQLITE_TRANSIENT);
			rc = sqlite3_step(stmt);
		}
		hook_batch_stmt_cleanup(&stmt);
		if (rc != SQLITE_DONE) {
			rc = (MORPH_ERR_DB);
			return rc;
		}
	}
	rc = sqlite3_prepare_v2(
		manager->db->handle,
		trust ? "INSERT INTO hook_trust(id,digest,disabled,definition) VALUES(?,?,0,?) "
			"ON CONFLICT(id) DO UPDATE SET digest=excluded.digest,"
			"definition=excluded.definition,disabled=0"
		      : "INSERT INTO hook_trust(id,digest,disabled,definition) VALUES(?,'',1,'') "
			"ON CONFLICT(id) DO UPDATE SET disabled=1",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		rc = (MORPH_ERR_DB);
		return rc;
	}
	for (size_t _i_ = 0; _i_ < nb_vec_len(&manager->entries) &&
			     (entry = (struct hook_entry *)nb_vec_at(&manager->entries, _i_));
	     _i_++) {
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
			rc = (MORPH_ERR_DB);
			return rc;
		}
	}
	hook_batch_stmt_cleanup(&stmt);
	return 0;
}

static int hook_manager_batch(struct hook_manager *manager, int trust, const char *expected_digest)
{
	struct db_lock lock NB_CLEANUP(db_lock_cleanup) = {0};
	char digest[65];
	int rc = hook_manager_reload(manager);

	if (rc == 0)
		rc = hook_review_digest(manager, digest);
	if (rc != 0)
		return rc;
	if (strcmp(digest, expected_digest) != 0)
		return NB_ERR_STALE;
	rc = db_lock_acquire(manager->db, &lock);
	if (rc == 0)
		rc = db_exec(manager->db, "SAVEPOINT hook_batch;");
	if (rc != 0)
		return rc;
	rc = hook_manager_batch_write(manager, trust);
	if (rc == 0)
		rc = db_exec(manager->db, "RELEASE hook_batch;");
	if (rc != 0)
		(void)db_exec(manager->db, "ROLLBACK TO hook_batch; RELEASE hook_batch;");
	return rc;
}

int hook_manager_action(struct hook_manager *manager, const char *action, const char *id,
			const char *expected_digest)
{
	struct hook_entry *entry;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!manager || !action || !id || !expected_digest)
		return NB_ERR_INVAL;
	if (strcmp(action, "trust-all") == 0 || strcmp(action, "deny-all") == 0)
		return hook_manager_batch(manager, strcmp(action, "trust-all") == 0,
					  expected_digest);
	if (strcmp(action, "trust-project") == 0 || strcmp(action, "revoke-project") == 0) {
		char digest[65];
		digest_text(manager->workdir, digest);
		if (strcmp(expected_digest, digest) != 0)
			return NB_ERR_STALE;
		rc = sqlite3_prepare_v2(
			manager->db->handle,
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
		for (size_t i = 0; i < manager->entries.len; i++) {
			struct hook_entry *candidate = nb_vec_at(&manager->entries, i);
			if (strcmp(candidate->id, id) == 0)
				entry = candidate;
		}
		if (!entry)
			return NB_ERR_NOT_FOUND;
		if (strcmp(entry->digest, expected_digest) != 0)
			return NB_ERR_STALE;
		if (strcmp(action, "trust") == 0) {
			if (!entry->supported || (entry->project && !project_trusted(manager)))
				return NB_ERR_NOT_PERMITTED;
			rc = sqlite3_prepare_v2(
				manager->db->handle,
				"INSERT INTO hook_trust(id,digest,disabled,definition) "
				"VALUES(?,?,0,?) "
				"ON CONFLICT(id) DO UPDATE SET "
				"digest=excluded.digest,definition=excluded.definition,disabled=0",
				-1, &stmt, NULL);
			if (rc == SQLITE_OK) {
				sqlite3_bind_text(stmt, 2, entry->digest, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(stmt, 3, entry->definition, -1, SQLITE_TRANSIENT);
			}
		} else if (strcmp(action, "revoke") == 0) {
			rc = sqlite3_prepare_v2(
				manager->db->handle,
				"INSERT INTO hook_trust(id,digest,disabled,definition) "
				"VALUES(?,'',0,'') "
				"ON CONFLICT(id) DO UPDATE SET digest='',disabled=0,definition=''",
				-1, &stmt, NULL);
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
			return NB_ERR_INVAL;
		}
		if (rc == SQLITE_OK && strcmp(action, "disable") != 0 &&
		    strcmp(action, "enable") != 0)
			sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
	}
	if (rc == SQLITE_OK)
		rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}
