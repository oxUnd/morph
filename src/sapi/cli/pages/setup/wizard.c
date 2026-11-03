#include "sapi/cli/pages/setup/wizard.h"
#include "sapi/cli/services/configuration/model.h"
#include "sapi/cli/services/configuration/role.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/pages/setup/controls.h"
#include "sapi/cli/services/configuration/config.h"
#include "sapi/cli/services/configuration/mcp.h"
#include "sapi/cli/services/management.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"
#include "util/name.h"
#include "tomlc17.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void cli_wizard_mcp_notes(int role, FILE *output)
{
	morph_buf_t notes;
	int rc = morph_buf_init(&notes, 128);

	if (rc)
		return;
	rc = cli_setup_mcp_notes(role, &notes);
	if (!rc)
		(void)cli_setup_print(output, "%s", morph_buf_cstr(&notes));
	morph_buf_cleanup(&notes);
}

static int cli_wizard_prompt(FILE *in, FILE *out, const char *label, const char *fallback,
			     char *value, size_t cap)
{
	for (;;) {
		char *line = NULL;
		size_t capacity = 0;
		fprintf(out, "%s%s%s%s: ", label, *fallback ? " [" : "", fallback,
			*fallback ? "]" : "");
		fflush(out);
		ssize_t len = getline(&line, &capacity, in);
		if (len < 0) {
			free(line);
			return -ECANCELED;
		}
		int embedded_nul = memchr(line, '\0', (size_t)len) != NULL;
		while (len > 0 && isspace((unsigned char)line[len - 1]))
			line[--len] = '\0';
		char *start = line;
		while (isspace((unsigned char)*start))
			start++;
		const char *chosen = *start ? start : fallback;
		int valid = !embedded_nul && *chosen && strlen(chosen) < cap &&
			    utf8valid(chosen) == NULL;
		for (const char *p = chosen; *p; p++)
			if ((unsigned char)*p < 32 || *p == 127)
				valid = 0;
		if (valid)
			memmove(value, chosen, strlen(chosen) + 1);
		free(line);
		if (valid)
			return 0;
		fprintf(out, "Enter a non-empty value shorter than %zu bytes.\n", cap);
	}
}

static int cli_wizard_choice(FILE *in, FILE *out, const char *label, const char *fallback, int max)
{
	char value[16];
	for (;;) {
		int rc = cli_wizard_prompt(in, out, label, fallback, value, sizeof(value));
		if (rc < 0)
			return rc;
		if (strlen(value) == 1 && value[0] >= '1' && value[0] <= '0' + max)
			return value[0] - '0';
		fprintf(out, "Choose a number from 1 to %d.\n", max);
	}
}

static int cli_wizard_prompt_token_count(FILE *in, FILE *out, const char *label, int minimum,
					 int maximum, int *value)
{
	char fallback[CLI_SETUP_TOKEN_INPUT_MAX] = "";
	char answer[CLI_SETUP_TOKEN_INPUT_MAX];
	if (*value > 0)
		snprintf(fallback, sizeof(fallback), "%d", *value);
	for (;;) {
		int rc = cli_wizard_prompt(in, out, label, fallback, answer, sizeof(answer));
		if (rc < 0)
			return rc;
		rc = cli_setup_parse_token_count(answer, minimum, maximum, value);
		if (rc == 0)
			return 0;
		fprintf(out, "Enter a whole token count from %d to %d.\n", minimum, maximum);
	}
}

static int cli_wizard_configure_model(FILE *in, FILE *out, int kind,
				      struct config_model_entry *entry)
{
	const char *menu =
		kind == CLI_SETUP_TEXT	   ? "Provider: 1) OpenAI  2) DeepSeek  3) Volcengine  4) "
					     "Other (OpenAI compatible)"
		: kind == CLI_SETUP_VISION ? "Provider: 1) OpenAI  2) DeepSeek  3) Volcengine  4) "
					     "Other (vision chat compatible)"
		: kind == CLI_SETUP_IMAGE
			? "Provider: 1) OpenAI  2) Volcengine  3) Other (OpenAI Images compatible)"
			: "Provider: 1) Volcengine  2) Other (Volcengine Video compatible)";
	int previous = cli_setup_provider_choice(kind, entry);
	char fallback[16];
	snprintf(fallback, sizeof(fallback), "%d", previous);
	int selected = cli_wizard_choice(in, out, menu, fallback,
					 kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION ? 4
					 : kind == CLI_SETUP_VIDEO			    ? 2
											    : 3);
	if (selected < 0)
		return selected;
	if (!entry->provider[0] || selected != previous)
		cli_setup_model_defaults(kind, selected, entry);
	char previous_model[sizeof(entry->model)];
	strcpy(previous_model, entry->model);
	int rc = cli_wizard_prompt(in, out, "Model name / endpoint ID", entry->model, entry->model,
				   sizeof(entry->model));
	if (rc < 0)
		return rc;
	if (kind == CLI_SETUP_TEXT || kind == CLI_SETUP_VISION) {
		if (strcmp(previous_model, entry->model) != 0)
			cli_setup_reset_token_limits(entry);
		rc = cli_wizard_prompt_token_count(in, out, "Context window (tokens)", 2,
						   cli_setup_token_ceiling(entry, 0),
						   &entry->context_limit);
		if (rc < 0)
			return rc;
		int ceiling = cli_setup_token_ceiling(entry, 1);
		if (ceiling >= entry->context_limit)
			ceiling = entry->context_limit - 1;
		if (entry->max_tokens > ceiling)
			entry->max_tokens = 0;
		rc = cli_wizard_prompt_token_count(in, out, "Max output (tokens)", 1, ceiling,
						   &entry->max_tokens);
		if (rc < 0)
			return rc;
	}
	if (strcmp(entry->adapter, "deepseek") == 0) {
		int thinking = cli_wizard_choice(
			in, out,
			"DeepSeek thinking: 1) Off (faster responses)  2) On (more time to reason)",
			cli_setup_thinking_enabled(entry) ? "2" : "1", 2);

		if (thinking < 0)
			return thinking;
		rc = cli_setup_set_thinking(entry, thinking == 2);
		if (rc != 0)
			return rc;
	}
	for (;;) {
		rc = cli_wizard_prompt(in, out, "API base URL", entry->api_base, entry->api_base,
				       sizeof(entry->api_base));
		if (rc < 0)
			return rc;
		if ((strncmp(entry->api_base, "https://", 8) == 0 && entry->api_base[8]) ||
		    (strncmp(entry->api_base, "http://", 7) == 0 && entry->api_base[7]))
			break;
		fprintf(out, "Use an http:// or https:// URL.\n");
	}
	if (entry->api_key[0]) {
		memset(entry->api_key_env, 0, sizeof(entry->api_key_env));
		return 0;
	}
	for (;;) {
		rc = cli_wizard_prompt(in, out, "API key environment variable (not the key)",
				       entry->api_key_env, entry->api_key_env,
				       sizeof(entry->api_key_env));
		if (rc < 0)
			return rc;
		if (morph_env_name_valid(entry->api_key_env))
			return 0;
		fprintf(out, "Use letters, digits and underscores; start with a letter or "
			     "underscore.\n");
	}
}

static void cli_wizard_append_field(morph_buf_t *buf, const char *key, const char *value)
{
	cJSON *string = cJSON_CreateString(value);
	char *encoded = string ? cJSON_PrintUnformatted(string) : NULL;
	cJSON_Delete(string);
	if (!encoded) {
		buf->failed = -ENOMEM;
		return;
	}
	morph_buf_printf(buf, "%s = %s\n", key, encoded);
	free(encoded);
}

static int cli_wizard_unique_file(const char *path, const char *suffix, const char *text,
				  char **created);

static int cli_wizard_save_config(const char *path, const char *text, int replace, int *published)
{
	if (published)
		*published = 0;
	char *parent = strdup(path);
	if (!parent)
		return -ENOMEM;
	char *slash = strrchr(parent, '/');
	if (slash) {
		if (slash == parent)
			slash[1] = '\0';
		else
			*slash = '\0';
	} else
		strcpy(parent, ".");
	int rc = file_ensure_dir(parent);
	int directory = rc == 0 ? open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
	if (rc == 0 && directory < 0)
		rc = morph_error_from_errno(errno);
	free(parent);
	if (rc != 0)
		return rc;
	char *temporary = NULL;
	rc = cli_wizard_unique_file(path, ".setup-XXXXXX", text, &temporary);
	if (rc == 0) {
		int result = replace ? rename(temporary, path) : link(temporary, path);
		if (result != 0)
			rc = morph_error_from_errno(errno);
		else if (published)
			*published = 1;
	}
	if (temporary)
		unlink(temporary);
	free(temporary);
	if (rc == 0 && fsync(directory) != 0)
		rc = morph_error_from_errno(errno);
	if (close(directory) != 0 && rc == 0)
		rc = morph_error_from_errno(errno);
	return rc;
}

static int cli_wizard_collect_plain_custom(struct cli_setup_state *state, FILE *input, FILE *output)
{
	if (state->existing_prompt) {
		int selected = cli_wizard_choice(
			input, output, "Instructions: 1) Keep existing  2) Use role instructions",
			"1", 2);
		if (selected < 0)
			return selected;
		state->replace_prompt = selected == 2;
	}
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		int required = cli_setup_role_requires(state->role, i);
		fprintf(output, "\n%s (%s)\n", cli_setup_model_label(i),
			required ? "required" : "optional");
		if (!required) {
			int selected = cli_wizard_choice(input, output, "1) Skip  2) Configure",
							 state->entries[i].model[0] ? "2" : "1", 2);
			if (selected < 0)
				return selected;
			if (selected == 1) {
				memset(&state->entries[i], 0, sizeof(state->entries[i]));
				continue;
			}
		}
		int rc = cli_wizard_configure_model(input, output, i, &state->entries[i]);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int cli_wizard_collect_plain(struct cli_setup_state *state, const char *path, FILE *input,
				    FILE *output)
{
	fprintf(output,
		"Welcome to morph setup: %s.\n"
		"Choose a role, then accept the defaults or customize. Ctrl-D cancels.\n",
		path);
	int choose_role = 1;
	state->replace_prompt = !state->existing_prompt;
	for (;;) {
		if (choose_role) {
			fputs("\nChoose your role:\n", output);
			for (int i = 0; i < CLI_SETUP_ROLE_COUNT; i++) {
				struct cli_setup_role role = cli_setup_role_get(i);
				fprintf(output, "  %d) %s - %s\n", i + 1, role.label,
					role.description);
			}
			char fallback[16];
			snprintf(fallback, sizeof(fallback), "%d", state->role + 1);
			int selected = cli_wizard_choice(input, output, "Role", fallback,
							 CLI_SETUP_ROLE_COUNT);
			if (selected < 0)
				return selected;
			state->role = selected - 1;
			cli_setup_role_defaults(state);
			choose_role = 0;
		}
		fprintf(output, "\nReview your setup · %s\n",
			cli_setup_role_get(state->role).label);
		int complete = 1;
		for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
			struct config_model_entry *entry = &state->entries[i];
			int required = cli_setup_role_requires(state->role, i);
			fprintf(output, "%s (%s): %s\n", cli_setup_model_label(i),
				required ? "required" : "optional",
				entry->model[0] ? entry->model : "Off");
			if ((required || entry->model[0]) && !cli_setup_model_complete(i, entry))
				complete = 0;
		}
		fprintf(output,
			"%s\nRead-only/media tools: auto-approved. Host execution: enabled.\n"
			"Instructions: %s.\n",
			cli_setup_mcp_summary(state->role),
			state->replace_prompt ? "Use role prompt" : "Keep existing");
		int selected = cli_wizard_choice(
			input, output, "1) Save and exit  2) Customize  3) Change role  4) Cancel",
			"1", 4);
		if (selected < 0)
			return selected;
		if (selected == 4)
			return -ECANCELED;
		if (selected == 1 && complete)
			return 0;
		if (selected == 1) {
			fputs("Complete the model name, API URL and valid token limits before "
			      "saving.\n",
			      output);
			continue;
		}
		if (selected == 3) {
			choose_role = 1;
			continue;
		}
		int rc = cli_wizard_collect_plain_custom(state, input, output);
		if (rc != 0)
			return rc;
	}
}

static int cli_wizard_load_existing(const char *path, struct cli_setup_state *state,
				    char **original)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : morph_error_from_errno(errno);
	if (!S_ISREG(st.st_mode))
		return -EINVAL;
	if (!state->reconfigure)
		return -EEXIST;
	if (st.st_size > 1024 * 1024)
		return -EFBIG;
	size_t len;
	*original = file_read_all(path, &len);
	if (!*original)
		return morph_error_from_errno(errno);
	if (memchr(*original, '\0', len) || utf8nvalid(*original, len))
		return -EILSEQ;
	struct config *config = calloc(1, sizeof(*config));
	if (!config)
		return -ENOMEM;
	int rc = config_load(NULL, config, path);
	if (rc == 0) {
		const struct config_model_entry *models[] = {
			&config->models.text, &config->models.vision, &config->models.image,
			&config->models.video};
		const char *names[] = {"text", "vision", "image", "video"};
		toml_result_t parsed = toml_parse(*original, (int)len);
		toml_datum_t model = toml_get(parsed.toptab, "model");
		for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
			if (toml_get(model, names[i]).type == TOML_TABLE)
				state->entries[i] = *models[i];
		toml_free(parsed);
		state->role = cli_setup_config_role(*original);
		state->existing_config = 1;
		state->existing_prompt =
			config->prompt.system_prompt_file[0] || config->prompt.system_prompt_dir[0];
	}
	memset(config, 0, sizeof(*config));
	free(config);
	return rc;
}

static int cli_wizard_unique_file(const char *path, const char *suffix, const char *text,
				  char **created)
{
	*created = file_path_append_alloc(path, suffix);
	if (!*created)
		return -ENOMEM;
	size_t suffix_len = strlen(suffix);
	int markdown = suffix_len >= 3 && strcmp(suffix + suffix_len - 3, ".md") == 0;
	int fd = markdown ? mkstemps(*created, 3) : mkstemp(*created);
	if (fd < 0) {
		int rc = morph_error_from_errno(errno);
		free(*created);
		*created = NULL;
		return rc;
	}
	size_t offset = 0;
	size_t len = strlen(text);
	int rc = 0;
	while (offset < len) {
		ssize_t n = write(fd, text + offset, len - offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			rc = morph_error_from_errno(errno);
			break;
		}
		offset += (size_t)n;
	}
	if (rc == 0 && fsync(fd) != 0)
		rc = morph_error_from_errno(errno);
	if (close(fd) != 0 && rc == 0)
		rc = morph_error_from_errno(errno);
	if (rc != 0) {
		unlink(*created);
		free(*created);
		*created = NULL;
	}
	return rc;
}

static int cli_wizard_role_snapshot(const char *path, int role, const char *text, char **snapshot,
				    int *created)
{
	*snapshot = NULL;
	*created = 0;
	char *directory = file_path_append_alloc(path, ".prompts");
	if (!directory)
		return -ENOMEM;
	int rc = file_ensure_dir(directory);
	char *base = rc == 0 ? file_path_join_alloc(directory, cli_setup_role_get(role).id) : NULL;
	char *named = base ? file_path_append_alloc(base, ".md") : NULL;
	if (rc == 0 && !named)
		rc = -ENOMEM;
	if (rc == 0) {
		struct stat st;
		if (lstat(named, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
		    (size_t)st.st_size == strlen(text)) {
			char *current = file_read_all(named, NULL);
			int same = current && strcmp(current, text) == 0;
			free(current);
			if (same) {
				*snapshot = named;
				free(base);
				free(directory);
				return 0;
			}
		}
		rc = cli_wizard_save_config(named, text, 0, created);
		if (rc == -EEXIST) {
			/* Preserve a customized or concurrently published named snapshot. */
			rc = cli_wizard_unique_file(base, "-XXXXXX.md", text, snapshot);
			if (rc == 0) {
				*created = 1;
				int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
				if (fd < 0)
					rc = morph_error_from_errno(errno);
				else {
					if (fsync(fd) != 0)
						rc = morph_error_from_errno(errno);
					if (close(fd) != 0 && rc == 0)
						rc = morph_error_from_errno(errno);
				}
			}
		} else if (rc == 0 || *created) {
			*snapshot = named;
			named = NULL;
		}
	}
	free(named);
	free(base);
	free(directory);
	return rc;
}

static int cli_wizard_publish_setup(const char *path, const char *original, const char *text,
				    FILE *output)
{
	if (!original)
		return cli_wizard_save_config(path, text, 0, NULL);
	char *lock_path = file_path_append_alloc(path, ".lock");
	if (!lock_path)
		return -ENOMEM;
	int lock = cli_management_lock(lock_path);
	free(lock_path);
	if (lock < 0)
		return lock;
	struct stat st;
	int rc = lstat(path, &st) == 0 ? 0 : morph_error_from_errno(errno);
	if (rc == 0 && !S_ISREG(st.st_mode))
		rc = -EINVAL;
	char *current = rc == 0 ? file_read_all(path, NULL) : NULL;
	if (rc == 0 && (!current || strcmp(current, original) != 0))
		rc = -ESTALE;
	free(current);
	char *backup = NULL;
	if (rc == 0)
		rc = cli_wizard_unique_file(path, ".backup-XXXXXX", original, &backup);
	if (rc == 0)
		rc = cli_wizard_save_config(path, text, 1, NULL);
	if (backup)
		cli_setup_print(output, "Previous configuration backed up to %s.\n", backup);
	free(backup);
	close(lock);
	return rc;
}

static int cli_wizard_save_setup(struct cli_setup_state *state, const char *path,
				 const char *original, FILE *output)
{
	const char *names[] = {"text", "vision", "image", "video"};
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
		if (cli_setup_role_requires(state->role, i) &&
		    !cli_setup_model_complete(i, &state->entries[i]))
			return MORPH_ERR_NOT_CONFIGURED;
	char *prompt_text = NULL;
	char *snapshot = NULL;
	int snapshot_created = 0;
	char *merged = NULL;
	morph_buf_t patch = {0};
	int rc = morph_buf_init(&patch, 0);
	if (rc != 0)
		return rc;
	if (state->replace_prompt) {
		rc = cli_setup_role_render(state->role, &prompt_text);
		if (rc == 0)
			rc = cli_wizard_role_snapshot(path, state->role, prompt_text, &snapshot,
						      &snapshot_created);
		if (rc == 0) {
			morph_buf_puts(&patch, "[prompt]\nmode = \"replace\"\n");
			cli_wizard_append_field(&patch, "system_prompt_file", snapshot);
			cli_wizard_append_field(&patch, "system_prompt_dir", "");
		}
	}
	morph_buf_puts(&patch, "\n[setup]\n");
	cli_wizard_append_field(&patch, "role", cli_setup_role_get(state->role).id);
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &state->entries[i];
		morph_buf_printf(&patch, "\n[model.%s]\n", names[i]);
		cli_wizard_append_field(&patch, "provider", entry->provider);
		cli_wizard_append_field(&patch, "adapter", entry->adapter);
		cli_wizard_append_field(&patch, "model", entry->model);
		cli_wizard_append_field(&patch, "api_base", entry->api_base);
		cli_wizard_append_field(&patch, "api_key", entry->api_key);
		cli_wizard_append_field(&patch, "api_key_env", entry->api_key_env);
		cli_wizard_append_field(&patch, "extra_body_json", entry->extra_body_json);
		if ((i == CLI_SETUP_TEXT || i == CLI_SETUP_VISION) && entry->model[0])
			morph_buf_printf(&patch, "context_limit = %d\nmax_tokens = %d\n",
					 entry->context_limit, entry->max_tokens);
		if (!entry->model[0])
			continue;
		if (entry->timeout_seconds > 0)
			morph_buf_printf(&patch, "timeout_seconds = %d\n", entry->timeout_seconds);
		if (i == CLI_SETUP_TEXT || i == CLI_SETUP_VISION)
			morph_buf_printf(&patch, "retry_count = %d\n", entry->retry_count);
		if (i == CLI_SETUP_VIDEO) {
			if (entry->poll_interval_seconds > 0)
				morph_buf_printf(&patch, "poll_interval_seconds = %d\n",
						 entry->poll_interval_seconds);
			if (entry->poll_timeout_seconds > 0)
				morph_buf_printf(&patch, "poll_timeout_seconds = %d\n",
						 entry->poll_timeout_seconds);
		}
	}
	if (!original)
		morph_buf_puts(&patch, "\n[exec]\nnetwork = true\n");
	else
		morph_buf_puts(&patch, "\n[exec]\n");
	morph_buf_puts(
		&patch,
		"allow_host_execution = true\n"
		"\n[react]\nhitl_auto_approve_readonly = true\nhitl_auto_approve_media = true\n");
	if (rc == 0)
		rc = patch.failed;
	if (rc == 0)
		rc = cli_setup_config_merge(original ? original : "", morph_buf_cstr(&patch),
					    &merged);
	if (rc == 0) {
		char *credentials = NULL;
		rc = cli_setup_config_credentials(merged, &credentials);
		free(merged);
		merged = credentials;
	}
	if (rc == 0) {
		char *mcp = NULL;
		rc = cli_setup_mcp_apply(merged, state->role, &mcp);
		free(merged);
		merged = mcp;
	}
	struct config_validation_error error = {0};
	if (rc == 0)
		rc = config_validate_text(merged, &error);
	if (rc == 0)
		rc = cli_wizard_publish_setup(path, original, merged, output);
	if (rc != 0 && snapshot && snapshot_created) {
		/* A directory sync failure can follow a successful atomic replacement. */
		char *published = file_read_all(path, NULL);
		if (!published || !merged || strcmp(published, merged) != 0)
			unlink(snapshot);
		free(published);
	}
	free(prompt_text);
	free(snapshot);
	free(merged);
	morph_buf_cleanup(&patch);
	return rc;
}

static int cli_wizard_setup_run(struct cli_application *ctx, const char *path, FILE *input,
				FILE *output, int reconfigure)
{
	struct cli_setup_state state = {.reconfigure = reconfigure};
	char *original = NULL;
	char current[PATH_MAX];
	char *expanded = path ? file_expand_path(path) : NULL;
	char *full = expanded && getcwd(current, sizeof(current))
			     ? file_path_full_alloc(current, expanded)
			     : NULL;
	free(expanded);
	if (!full)
		return -EINVAL;
	int rc = cli_wizard_load_existing(full, &state, &original);
	if (rc == 0)
		rc = cli_setup_controls_available(input, output)
			     ? cli_setup_controls_collect(ctx, full, input, output, &state)
			     : cli_wizard_collect_plain(&state, full, input, output);
	if (rc == 0)
		rc = cli_wizard_save_setup(&state, full, original, output);
	if (rc == 0) {
		cli_setup_print(output, "\nConfiguration saved to %s.\n", full);
		cli_wizard_mcp_notes(state.role, output);
		for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
			struct config_model_entry *entry = &state.entries[i];
			if (!entry->model[0] || entry->api_key[0])
				continue;
			const char *key = getenv(entry->api_key_env);
			if (!key || !*key) {
				rc = 1;
				int shown = 0;
				for (int previous = 0; previous < i; previous++)
					if (state.entries[previous].model[0] &&
					    !state.entries[previous].api_key[0] &&
					    strcmp(state.entries[previous].api_key_env,
						   entry->api_key_env) == 0)
						shown = 1;
				if (shown)
					continue;
				cli_setup_print(output,
						"Set the %s model key in your shell:\n"
						"  export %s='<your-api-key>'\n",
						i == CLI_SETUP_TEXT	? "text"
						: i == CLI_SETUP_VISION ? "vision"
						: i == CLI_SETUP_IMAGE	? "image"
									: "video",
						entry->api_key_env);
				rc = 1;
			}
		}
		cli_setup_print(
			output,
			"Setup complete. Run morph again with the same -c configuration to start.\n"
			"To change these settings, run morph setup with the same -c path.\n");
	}
	memset(&state, 0, sizeof(state));
	free(original);
	free(full);
	return rc;
}

int cli_setup(struct cli_application *ctx, const char *path, FILE *input, FILE *output)
{
	return cli_wizard_setup_run(ctx, path, input, output, 0);
}

int cli_setup_reconfigure(struct cli_application *ctx, const char *path, FILE *input, FILE *output)
{
	return cli_wizard_setup_run(ctx, path, input, output, 1);
}

int cli_setup_command(struct cli_application *ctx, int argc, char **argv, const char *path)
{
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			fputs("Usage: morph setup [-c PATH]\n"
			      "Choose a role, configure its models, save and exit.\n",
			      stdout);
			return 0;
		}
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) &&
		    i + 1 < argc)
			path = argv[++i];
		else if (strncmp(argv[i], "--config=", 9) == 0 && argv[i][9])
			path = argv[i] + 9;
		else {
			fprintf(stderr, "morph: unexpected setup argument: %s\n", argv[i]);
			return 2;
		}
	}
	int rc = cli_setup_reconfigure(ctx, path ? path : "~/.morph/config.toml", stdin, stderr);
	if (rc < 0)
		fprintf(stderr, "morph: setup failed: %s\n", morph_strerror(rc));
	return rc < 0 ? 1 : 0;
}

int cli_setup_if_missing(struct cli_application *ctx, const char *path, int interactive)
{
	char *expanded = file_expand_path(path ? path : "~/.morph/config.toml");
	if (!expanded)
		return -ENOMEM;
	struct stat st;
	if (lstat(expanded, &st) == 0) {
		free(expanded);
		return 0;
	}
	int rc = -errno;
	if (rc == -ENOENT) {
		if (interactive && isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
			rc = cli_setup(ctx, expanded, stdin, stderr);
			if (rc >= 0)
				rc = 1;
		} else {
			fprintf(stderr,
				"morph: configuration not found: %s\n"
				"Run morph setup to configure it"
				" (use the same -c path if specified).\n",
				expanded);
		}
	}
	if (rc == -ECANCELED)
		fprintf(stderr, "Setup canceled. No configuration was saved.\n");
	else if (rc < 0)
		fprintf(stderr, "morph: setup failed: %s\n", morph_strerror(rc));
	free(expanded);
	if (rc < 0)
		return rc;
	return rc;
}
