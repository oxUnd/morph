#include "runtime/hooks.h"
#include "runtime/hooks_internal.h"

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
		     const char *matcher, const struct json *handler, int project, int group_index,
		     int handler_index)
{
	struct hook_entry *entry;
	morph_buf_t identity = {0};
	struct json *canonical = json_object();
	const struct json *timeout = json_member(handler, "timeout");
	const struct json *limit = json_member(handler, "additionalContextLimit");
	int valid = json_is_object_value(handler);
	int rc;

	if (!valid) {
		json_free(canonical);
		return MORPH_ERR_CONFIG;
	}
	if (!canonical || manager->entries.nelts >= HOOK_LIMIT) {
		json_free(canonical);
		return canonical ? -E2BIG : -ENOMEM;
	}
	entry = morph_array_push(&manager->entries);
	if (!entry) {
		json_free(canonical);
		return -ENOMEM;
	}
	memset(entry, 0, sizeof(*entry));
	entry->source = file_resolve_path(source);
	if (!entry->source)
		entry->source = strdup(source);
	entry->event = strdup(event);
	entry->matcher = strdup(matcher);
	entry->command = strdup(json_str_or(handler, "command", ""));
	entry->status_message = strdup(json_str_or(handler, "statusMessage", ""));
	entry->project = project;
	entry->timeout = json_is_number_value(timeout) ? json_int_value(timeout)
						       : (event_short_timeout(event) ? 1 : 600);
	entry->context_limit = json_is_number_value(limit) ? json_int_value(limit) : 2500;
	if (!entry->source || !entry->event || !entry->matcher || !entry->command ||
	    !entry->status_message) {
		json_free(canonical);
		return -ENOMEM;
	}
	if ((timeout && (!json_is_number_value(timeout) ||
			 json_number_value(timeout) != (double)json_int_value(timeout))) ||
	    entry->timeout < 1 || entry->timeout > 600 ||
	    (event_short_timeout(event) && entry->timeout > 3) ||
	    (limit && (!json_is_number_value(limit) ||
		       json_number_value(limit) != (double)json_int_value(limit))) ||
	    entry->context_limit < 0 || entry->context_limit > 32768 || strlen(matcher) > 1024 ||
	    strlen(entry->command) >= PROCESS_COMMAND_MAX ||
	    (!entry->command[0] && strcmp(json_str_or(handler, "type", ""), "command") == 0) ||
	    !*json_str_or(handler, "type", ""))
		valid = 0;
	if (*matcher && strcmp(matcher, "*") != 0 && !hook_event_ignores_matcher(event)) {
		entry->regex_ready = regcomp(&entry->regex, matcher, REG_EXTENDED | REG_NOSUB) == 0;
		if (!entry->regex_ready)
			valid = 0;
	}
	/* Stable field order makes formatting and JSON key order irrelevant. */
	json_add_string(canonical, "event", event);
	json_add_string(canonical, "matcher", matcher);
	json_add_string(canonical, "type", json_str_or(handler, "type", ""));
	json_add_string(canonical, "command", entry->command);
	json_add_string(canonical, "statusMessage", json_str_or(handler, "statusMessage", ""));
	json_add_number(canonical, "timeout", entry->timeout);
	json_add_number(canonical, "additionalContextLimit", entry->context_limit);
	entry->definition = json_stringify(canonical);
	json_free(canonical);
	if (!entry->definition)
		return -ENOMEM;
	rc = morph_buf_init(&identity, 128);
	if (rc == 0)
		rc = morph_buf_printf(&identity, "%s\n%s\n%s\n%d\n%d",
				      project ? manager->workdir : "user", entry->source, event,
				      group_index, handler_index);
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
	const struct json *field;
	json_foreach (field, handler) {
		if (strcmp(json_key(field), "type") != 0 &&
		    strcmp(json_key(field), "command") != 0 &&
		    strcmp(json_key(field), "timeout") != 0 &&
		    strcmp(json_key(field), "statusMessage") != 0 &&
		    strcmp(json_key(field), "additionalContextLimit") != 0)
			portable = 0;
	}
	entry->supported = portable && valid && event_supported(event) &&
			   strcmp(json_str_or(handler, "type", ""), "command") == 0;
	entry->diagnostic = !valid ? "Invalid command, timeout, context limit, or matcher regex."
			    : !event_supported(event)
				    ? "This lifecycle event is not supported by Morph."
			    : strcmp(json_str_or(handler, "type", ""), "command") != 0
				    ? "Only command handlers are supported."
			    : !portable ? "Handler contains unsupported fields (for example async "
					  "or commandWindows)."
					: "";
	entry->status = strdup(!valid ? "invalid" : !entry->supported ? "unsupported" : "");
	if (!entry->status)
		return -ENOMEM;
	rc = hook_refresh_trust(manager, entry);
	if (rc == 0 && !project && !entry->recorded) {
		char legacy_id[65];
		char legacy_digest[65];
		morph_buf_t legacy;
		sqlite3_stmt *stmt = NULL;

		rc = morph_buf_init(&legacy, 128);
		if (rc != 0)
			return rc;
		rc = morph_buf_printf(&legacy, "%s\n%s\n%s\n%d\n%d", manager->workdir, source,
				      event, group_index, handler_index);
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

int hook_json_unique_fields(const struct json *value, int depth)
{
	const struct json *child;

	if (depth > 16)
		return 0;
	json_foreach (child, value) {
		if (json_is_object_value(value)) {
			for (const struct json *next = json_next(child); next;
			     next = json_next(next)) {
				if (strcmp(json_key(child), json_key(next)) == 0)
					return 0;
			}
		}
		if ((json_is_object_value(child) || json_is_array_value(child)) &&
		    !hook_json_unique_fields(child, depth + 1))
			return 0;
	}
	return 1;
}

static int load_object(struct hook_manager *manager, const struct json *hooks, const char *source,
		       int project)
{
	const struct json *event;

	if (!json_is_object_value(hooks) || !hook_json_unique_fields(hooks, 0))
		return MORPH_ERR_CONFIG;
	json_foreach (event, hooks) {
		const struct json *group;
		int gi = 0;

		if (!json_is_array_value(event))
			return MORPH_ERR_CONFIG;
		json_foreach (group, event) {
			const struct json *handlers = json_member(group, "hooks");
			const struct json *matcher = json_member(group, "matcher");
			const struct json *handler;
			int hi = 0;

			if (!json_is_object_value(group) || !json_is_array_value(handlers) ||
			    (matcher && !json_is_string_value(matcher)))
				return MORPH_ERR_CONFIG;
			json_foreach (handler, handlers) {
				int rc = add_entry(manager, source, json_key(event),
						   matcher ? json_string_value(matcher) : "",
						   handler, project, gi, hi++);
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
			 message ? message : morph_strerror(rc));
	if (manager->error.detail.code == CONFIG_VALIDATION_NONE)
		manager->error.detail.code =
			rc == MORPH_ERR_PARSE ? CONFIG_VALIDATION_SYNTAX : CONFIG_VALIDATION_VALUE;
	log_err(manager ? manager->logger : NULL, "failed to load hooks %s:%d:%d: %s", path,
		manager->error.detail.line, manager->error.detail.column,
		manager->error.detail.message);
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
		(int)utf8nlen(line_start, (size_t)(position - line_start)) + 1;
}

static int load_json_file(struct hook_manager *manager, const char *path, int project)
{
	struct stat st;
	char *text;
	struct json *root;
	int rc;
	const char *parse_end = NULL;
	char canonical[PATH_MAX];

	if (realpath(path, canonical))
		path = canonical;
	struct hook_entry *existing;
	morph_array_foreach (existing, &manager->entries, struct hook_entry) {
		if (strcmp(existing->source, path) == 0)
			return 0;
	}
	if (stat(path, &st) != 0) {
		rc = -errno;
		return rc == -ENOENT ? 0 : hook_load_error(manager, path, rc, NULL);
	}
	if (!S_ISREG(st.st_mode) || st.st_size > CONFIG_HOOKS_JSON_MAX)
		return hook_load_error(
			manager, path, -EFBIG,
			"Hooks configuration must be a regular file within the size limit.");
	text = file_read_all(path, NULL);
	if (!text)
		return hook_load_error(manager, path, -EIO, NULL);
	root = json_parse_ex(text, &parse_end, 1, NULL);
	if (!root) {
		hook_json_error_position(manager, text, parse_end);
		rc = hook_load_error(
			manager, path, MORPH_ERR_PARSE,
			parse_end && *parse_end
				? "Invalid JSON syntax; check quotes, commas, and brackets."
				: "Unexpected end of JSON input; expected a complete JSON "
				  "document.");
		free(text);
		return rc;
	}
	free(text);
	rc = load_object(manager, json_member(root, "hooks"), path, project);
	json_free(root);
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
	struct json *hooks;
	int rc;

	if (!json || !*json)
		return 0;
	struct hook_entry *entry;
	const char *slash = strrchr(path, '/');
	if (slash) {
		morph_array_foreach (entry, &manager->entries, struct hook_entry) {
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
	hooks = json_parse_ex(json, NULL, 1, NULL);
	if (!hooks)
		return hook_load_error(manager, path, MORPH_ERR_PARSE,
				       "Invalid inline hooks JSON.");
	rc = load_object(manager, hooks, path, project);
	json_free(hooks);
	if (rc != 0)
		return hook_load_error(manager, path, rc, "Invalid inline hooks structure.");
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
		return -EINVAL;
	}
	memset(&manager->error, 0, sizeof(manager->error));
	morph_array_foreach (entry, &manager->entries, struct hook_entry)
		entry_cleanup(entry);
	morph_array_clear(&manager->entries);
	rc = load_json_file(manager, home, 0);
	free(home);
	if (rc == 0 && manager->config_path[0]) {
		char *directory = strdup(manager->config_path);
		if (!directory)
			return -ENOMEM;
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
			return -ENOMEM;
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
		rc = file_path_join(path, sizeof(path), manager->workdir, ".morph/hooks.json");
	if (rc == 0)
		rc = load_json_file(manager, path, 1);
	/* Project TOML is parsed with the normal schema, never applied to runtime. */
	if (rc == 0)
		rc = file_path_join(path, sizeof(path), manager->workdir, ".morph/config.toml");
	if (rc == 0 && file_exists(path) && strcmp(path, manager->config_path) != 0) {
		struct config *project = calloc(1, sizeof(*project));
		if (!project)
			return -ENOMEM;
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
	int rc;

	if (!manager || !db || !db->handle || !config || !workdir)
		return -EINVAL;
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
	manager->recent = json_array();
	if (!manager->recent) {
		morph_array_cleanup(&manager->entries);
		return -ENOMEM;
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
	morph_array_foreach (entry, &manager->entries, struct hook_entry)
		entry_cleanup(entry);
	morph_array_cleanup(&manager->entries);
	free(manager->session_context);
	json_free(manager->recent);
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
	morph_buf_t text;
	struct hook_entry *entry;
	int rc = morph_buf_init(&text, 256);

	if (rc != 0)
		return rc;
	rc = morph_buf_printf(&text, "%s\n%d\n", manager->workdir, manager->enabled);
	morph_array_foreach (entry, &manager->entries, struct hook_entry) {
		if (rc == 0)
			rc = morph_buf_printf(&text, "%s:%s:%d\n", entry->id, entry->digest,
					      entry->supported);
	}
	if (rc == 0)
		digest_text(morph_buf_cstr(&text), digest);
	morph_buf_cleanup(&text);
	return rc;
}

struct json *hook_manager_list(struct hook_manager *manager)
{
	struct json *list = json_array();
	struct hook_entry *entry;
	char review_digest[65];

	if (!manager || !manager->ready || !list ||
	    hook_review_digest(manager, review_digest) != 0) {
		json_free(list);
		return NULL;
	}
	morph_array_foreach (entry, &manager->entries, struct hook_entry) {
		struct json *row = json_object();
		if (!row || hook_refresh_trust(manager, entry) != 0) {
			json_free(row);
			json_free(list);
			return NULL;
		}
		json_add_string(row, "review_digest", review_digest);
		json_add_bool(row, "project", entry->project);
		json_add_string(row, "id", entry->id);
		json_add_string(row, "digest", entry->digest);
		json_add_string(row, "source", entry->source);
		json_add_string(row, "event", entry->event);
		json_add_string(row, "matcher", entry->matcher);
		json_add_string(row, "command", entry->command);
		json_add_string(row, "definition", entry->definition);
		json_add_string(row, "previous_definition", entry->previous ? entry->previous : "");
		json_add_string(row, "state", hook_entry_state(manager, entry));
		json_add_string(row, "diagnostic", entry->diagnostic);
		json_add_string(row, "statusMessage", entry->status_message);
		json_add_number(row, "additionalContextLimit", entry->context_limit);
		json_add_string(row, "cwd", manager->workdir);
		char workspace_digest[65];
		digest_text(manager->workdir, workspace_digest);
		json_add_string(row, "workspace_digest", workspace_digest);
		json_add_bool(row, "workspace_trusted", project_trusted(manager));
		json_add_number(row, "timeout", entry->timeout);
		const struct json *last;
		json_foreach (last, manager->recent) {
			if (strcmp(json_str_or(last, "hook_id", ""), entry->id) == 0) {
				json_object_remove_ci(row, "last_run");
				json_object_add(row, "last_run", json_clone(last, 1));
			}
		}
		json_array_add(list, row);
	}
	return list;
}

static int hook_manager_batch(struct hook_manager *manager, int trust, const char *expected_digest)
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
		return -ESTALE;
	rc = db_exec(manager->db, "SAVEPOINT hook_batch;");
	if (rc != 0)
		return rc;
	if (trust) {
		rc = sqlite3_prepare_v2(manager->db->handle,
					"INSERT OR IGNORE INTO hook_workspaces(path) VALUES(?)", -1,
					&stmt, NULL);
		if (rc == SQLITE_OK) {
			sqlite3_bind_text(stmt, 1, manager->workdir, -1, SQLITE_TRANSIENT);
			rc = sqlite3_step(stmt);
		}
		sqlite3_finalize(stmt);
		stmt = NULL;
		if (rc != SQLITE_DONE) {
			rc = (MORPH_ERR_DB);
			goto out_rollback_batch;
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
		goto out_rollback_batch;
	}
	morph_array_foreach (entry, &manager->entries, struct hook_entry) {
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
	return rc;
}

int hook_manager_action(struct hook_manager *manager, const char *action, const char *id,
			const char *expected_digest)
{
	struct hook_entry *entry;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!manager || !action || !id || !expected_digest)
		return -EINVAL;
	if (strcmp(action, "trust-all") == 0 || strcmp(action, "deny-all") == 0)
		return hook_manager_batch(manager, strcmp(action, "trust-all") == 0,
					  expected_digest);
	if (strcmp(action, "trust-project") == 0 || strcmp(action, "revoke-project") == 0) {
		char digest[65];
		digest_text(manager->workdir, digest);
		if (strcmp(expected_digest, digest) != 0)
			return -ESTALE;
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
		for (size_t i = 0; i < manager->entries.nelts; i++) {
			struct hook_entry *candidate = morph_array_get(&manager->entries, i);
			if (strcmp(candidate->id, id) == 0)
				entry = candidate;
		}
		if (!entry)
			return -ENOENT;
		if (strcmp(entry->digest, expected_digest) != 0)
			return -ESTALE;
		if (strcmp(action, "trust") == 0) {
			if (!entry->supported || (entry->project && !project_trusted(manager)))
				return -EPERM;
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
			return -EINVAL;
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
