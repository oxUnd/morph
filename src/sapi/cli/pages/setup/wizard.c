#include "sapi/cli/pages/setup/wizard.h"
#include "sapi/cli/services/configuration/model.h"
#include "sapi/cli/services/configuration/role.h"
#include "config/config.h"
#include "cJSON.h"
#include "sapi/cli/pages/setup/controls.h"
#include "sapi/cli/services/configuration/config.h"
#include "sapi/cli/services/configuration/mcp.h"
#include "sapi/cli/services/management.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/random.h"
#include "nb/utf8.h"
#include "nb/str.h"
#include "nb/toml.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void cli_wizard_mcp_notes(int role, FILE *output)
{
	nb_buf notes NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&notes), nb_buf_reserve(&notes, 128) ? 0 : NB_ERR_NOMEM);

	if (rc)
		return;
	rc = cli_setup_mcp_notes(role, &notes);
	if (!rc)
		(void)cli_setup_print(output, "%s", nb_buf_cstr(&notes));
	nb_buf_cleanup(&notes);
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
			return NB_ERR_CANCELLED;
		}
		int embedded_nul = memchr(line, '\0', (size_t)len) != NULL;
		while (len > 0 && isspace((unsigned char)line[len - 1]))
			line[--len] = '\0';
		char *start = line;
		while (isspace((unsigned char)*start))
			start++;
		const char *chosen = *start ? start : fallback;
		int valid = !embedded_nul && *chosen && strlen(chosen) < cap &&
			    nb_utf8_validate(nb_str_from_cstr(chosen), NULL) == NB_UTF8_OK;
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
		if ((nb_str_is_alnum(nb_str_from_cstr(entry->api_key_env), "_") &&
		     !((entry->api_key_env)[0] >= '0' && (entry->api_key_env)[0] <= '9')))
			return 0;
		fprintf(out, "Use letters, digits and underscores; start with a letter or "
			     "underscore.\n");
	}
}

static void cli_wizard_append_field(nb_buf *buf, const char *key, const char *value)
{
	cJSON *string = cJSON_CreateString(value);
	char *encoded = string ? cJSON_PrintUnformatted(string) : NULL;
	cJSON_Delete(string);
	if (!encoded) {
		nb_buf_set_error(buf, NB_ERR_NOMEM);
		return;
	}
	nb_buf_printf(buf, "%s = %s\n", key, encoded);
	free(encoded);
}

static int cli_wizard_unique_file(const char *path, const char *suffix, const char *extension,
				  const char *text, char **created);

static int cli_wizard_save_config(const char *path, const char *text, int replace, int *published)
{
	if (published)
		*published = 0;
	char *parent NB_CLEANUP(nb_charp_cleanup) = strdup(path);
	if (!parent)
		return NB_ERR_NOMEM;
	char *slash = strrchr(parent, '/');
	if (slash) {
		if (slash == parent)
			slash[1] = '\0';
		else
			*slash = '\0';
	} else {
		strcpy(parent, ".");
	}
	int rc = nb_file_mkdir(parent, true, NULL);
	if (rc != NB_OK)
		return rc;
	bool committed = false;
	nb_file_write_options options = {
		.mode = 0600,
		.create_only = !replace,
		.sync_dir = true,
		.published = &committed,
	};
	rc = nb_file_write_all_atomic(path, text, strlen(text), &options, NULL);
	if (published)
		*published = committed;
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
		"Choose a role, then accept the defaults or customize. Ctrl+D cancels.\n",
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
			return NB_ERR_CANCELLED;
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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	int read_status_1 = NB_OK;
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	nb_err parse_error = {0};
	struct stat st;
	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : nb_status_from_errno(errno, NULL);
	if (!S_ISREG(st.st_mode))
		return NB_ERR_INVAL;
	if (!state->reconfigure)
		return NB_ERR_EXISTS;
	if (st.st_size > 1024 * 1024)
		return NB_ERR_TOO_BIG;
	size_t len;
	*original = ((read_status_1 = nb_file_read_all(path, &file_value_1, 0, NULL)) == NB_OK &&
				     (file_copy_1 =
					      (char *)malloc(nb_buf_len(&file_value_1) + 1)) != NULL
			     ? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
				       nb_buf_len(&file_value_1) + 1),
				*(&len) = nb_buf_len(&file_value_1), nb_charp_steal(&file_copy_1))
			     : NULL);
	if (!*original)
		return read_status_1 != NB_OK ? read_status_1 : NB_ERR_NOMEM;
	if (memchr(*original, '\0', len) ||
	    nb_utf8_validate(nb_str_from_parts(*original, len), NULL))
		return NB_ERR_ENCODING;
	struct config *config = calloc(1, sizeof(*config));
	if (!config)
		return NB_ERR_NOMEM;
	int rc = config_load(NULL, config, path);
	if (rc == 0) {
		const struct config_model_entry *models[] = {
			&config->models.text, &config->models.vision, &config->models.image,
			&config->models.video};
		const char *names[] = {"text", "vision", "image", "video"};
		nb_toml *parsed NB_CLEANUP(nb_toml_cleanup) =
			nb_toml_parse(nb_str_from_parts(*original, len), &parse_error);
		const nb_toml_value *model = nb_toml_table_get_cstr(nb_toml_root(parsed), "model");
		for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
			if (nb_toml_type_of(nb_toml_table_get_cstr(model, names[i])) ==
			    NB_TOML_TABLE)
				state->entries[i] = *models[i];
		nb_toml_cleanup(&parsed);
		state->role = cli_setup_config_role(*original);
		state->existing_config = 1;
		state->existing_prompt =
			config->prompt.system_prompt_file[0] || config->prompt.system_prompt_dir[0];
	}
	memset(config, 0, sizeof(*config));
	free(config);
	return rc;
}

static int cli_wizard_unique_file(const char *path, const char *suffix, const char *extension,
				  const char *text, char **created)
{
	nb_buf candidate NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf random NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_file_write_options options = {
		.mode = 0600,
		.create_only = true,
		.sync_dir = true,
	};
	*created = NULL;
	for (int attempt = 0; attempt < 16; attempt++) {
		nb_buf_clear(&random);
		int rc = nb_random_hex(&random, 6, NULL);
		if (rc != NB_OK)
			return rc;
		nb_buf_clear(&candidate);
		if (!nb_buf_printf(&candidate, "%s%s%s%s", path, suffix, nb_buf_cstr(&random),
				   extension))
			return nb_buf_status(&candidate);
		char *name NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&candidate));
		if (!name)
			return NB_ERR_NOMEM;
		bool published = false;
		options.published = &published;
		rc = nb_file_write_all_atomic(name, text, strlen(text), &options, NULL);
		if (published)
			*created = nb_charp_steal(&name);
		if (rc != NB_ERR_EXISTS)
			return rc;
	}
	return NB_ERR_EXISTS;
}

static int cli_wizard_role_snapshot(const char *path, int role, const char *text, char **snapshot,
				    int *created)
{
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_6 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_6 NB_CLEANUP(nb_charp_cleanup) = NULL;

	*snapshot = NULL;
	*created = 0;
	char *directory =
		(nb_buf_clear(&file_value_3), nb_buf_printf(&file_value_3, "%s%s", path, ".prompts")
						      ? strdup(nb_buf_cstr(&file_value_3))
						      : NULL);
	if (!directory)
		return NB_ERR_NOMEM;
	int rc = nb_file_mkdir(directory, true, NULL);
	char *base = rc == 0 ? (nb_path_concat(&file_value_4, directory,
					       cli_setup_role_get(role).id) == NB_OK
					? strdup(nb_buf_cstr(&file_value_4))
					: NULL)
			     : NULL;
	char *named = base ? (nb_buf_clear(&file_value_5),
			      nb_buf_printf(&file_value_5, "%s%s", base, ".md")
				      ? strdup(nb_buf_cstr(&file_value_5))
				      : NULL)
			   : NULL;
	if (rc == 0 && !named)
		rc = NB_ERR_NOMEM;
	if (rc == 0) {
		struct stat st;
		if (lstat(named, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
		    (size_t)st.st_size == strlen(text)) {
			char *current =
				(nb_file_read_all(named, &file_value_6, 0, NULL) == NB_OK &&
						 (file_copy_6 = (char *)malloc(
							  nb_buf_len(&file_value_6) + 1)) != NULL
					 ? (memcpy(file_copy_6, nb_buf_cstr(&file_value_6),
						   nb_buf_len(&file_value_6) + 1),
					    nb_charp_steal(&file_copy_6))
					 : NULL);
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
		if (rc == NB_ERR_EXISTS) {
			/* Preserve a customized or concurrently published named snapshot. */
			rc = cli_wizard_unique_file(base, "-", ".md", text, snapshot);
			if (*snapshot)
				*created = 1;
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
	nb_buf file_value_7 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_8 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_8 NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (!original)
		return cli_wizard_save_config(path, text, 0, NULL);
	char *lock_path =
		(nb_buf_clear(&file_value_7), nb_buf_printf(&file_value_7, "%s%s", path, ".lock")
						      ? strdup(nb_buf_cstr(&file_value_7))
						      : NULL);
	if (!lock_path)
		return NB_ERR_NOMEM;
	int lock = cli_management_lock(lock_path);
	free(lock_path);
	if (lock < 0)
		return lock;
	struct stat st;
	int rc = lstat(path, &st) == 0 ? 0 : nb_status_from_errno(errno, NULL);
	if (rc == 0 && !S_ISREG(st.st_mode))
		rc = NB_ERR_INVAL;
	char *current = rc == 0 ? (nb_file_read_all(path, &file_value_8, 0, NULL) == NB_OK &&
						   (file_copy_8 = (char *)malloc(
							    nb_buf_len(&file_value_8) + 1)) != NULL
					   ? (memcpy(file_copy_8, nb_buf_cstr(&file_value_8),
						     nb_buf_len(&file_value_8) + 1),
					      nb_charp_steal(&file_copy_8))
					   : NULL)
				: NULL;
	if (rc == 0 && (!current || strcmp(current, original) != 0))
		rc = NB_ERR_STALE;
	free(current);
	char *backup = NULL;
	if (rc == 0)
		rc = cli_wizard_unique_file(path, ".backup-", "", original, &backup);
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
	nb_buf file_value_9 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_9 NB_CLEANUP(nb_charp_cleanup) = NULL;

	const char *names[] = {"text", "vision", "image", "video"};
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
		if (cli_setup_role_requires(state->role, i) &&
		    !cli_setup_model_complete(i, &state->entries[i]))
			return MORPH_ERR_NOT_CONFIGURED;
	char *prompt_text = NULL;
	char *snapshot = NULL;
	int snapshot_created = 0;
	char *merged = NULL;
	nb_buf patch NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&patch), nb_buf_reserve(&patch, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (state->replace_prompt) {
		rc = cli_setup_role_render_copy(state->role, &prompt_text);
		if (rc == 0)
			rc = cli_wizard_role_snapshot(path, state->role, prompt_text, &snapshot,
						      &snapshot_created);
		if (rc == 0) {
			nb_buf_append_cstr(&patch, "[prompt]\nmode = \"replace\"\n");
			cli_wizard_append_field(&patch, "system_prompt_file", snapshot);
			cli_wizard_append_field(&patch, "system_prompt_dir", "");
		}
	}
	nb_buf_append_cstr(&patch, "\n[setup]\n");
	cli_wizard_append_field(&patch, "role", cli_setup_role_get(state->role).id);
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++) {
		struct config_model_entry *entry = &state->entries[i];
		nb_buf_printf(&patch, "\n[model.%s]\n", names[i]);
		cli_wizard_append_field(&patch, "provider", entry->provider);
		cli_wizard_append_field(&patch, "adapter", entry->adapter);
		cli_wizard_append_field(&patch, "model", entry->model);
		cli_wizard_append_field(&patch, "api_base", entry->api_base);
		cli_wizard_append_field(&patch, "api_key", entry->api_key);
		cli_wizard_append_field(&patch, "api_key_env", entry->api_key_env);
		cli_wizard_append_field(&patch, "extra_body_json", entry->extra_body_json);
		if ((i == CLI_SETUP_TEXT || i == CLI_SETUP_VISION) && entry->model[0])
			nb_buf_printf(&patch, "context_limit = %d\nmax_tokens = %d\n",
				      entry->context_limit, entry->max_tokens);
		if (!entry->model[0])
			continue;
		if (entry->timeout_seconds > 0)
			nb_buf_printf(&patch, "timeout_seconds = %d\n", entry->timeout_seconds);
		if (i == CLI_SETUP_TEXT || i == CLI_SETUP_VISION)
			nb_buf_printf(&patch, "retry_count = %d\n", entry->retry_count);
		if (i == CLI_SETUP_VIDEO) {
			if (entry->poll_interval_seconds > 0)
				nb_buf_printf(&patch, "poll_interval_seconds = %d\n",
					      entry->poll_interval_seconds);
			if (entry->poll_timeout_seconds > 0)
				nb_buf_printf(&patch, "poll_timeout_seconds = %d\n",
					      entry->poll_timeout_seconds);
		}
	}
	if (!original)
		nb_buf_append_cstr(&patch, "\n[exec]\nnetwork = true\n");
	else
		nb_buf_append_cstr(&patch, "\n[exec]\n");
	nb_buf_append_cstr(
		&patch,
		"allow_host_execution = true\n"
		"\n[react]\nhitl_auto_approve_readonly = true\nhitl_auto_approve_media = true\n");
	if (rc == 0)
		rc = nb_buf_status(&patch);
	if (rc == 0)
		rc = cli_setup_config_merge_copy(original ? original : "", nb_buf_cstr(&patch),
						 &merged);
	if (rc == 0) {
		char *credentials = NULL;
		rc = cli_setup_config_credentials_copy(merged, &credentials);
		free(merged);
		merged = credentials;
	}
	if (rc == 0) {
		char *mcp = NULL;
		rc = cli_setup_mcp_apply_copy(merged, state->role, &mcp);
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
		char *published = (nb_file_read_all(path, &file_value_9, 0, NULL) == NB_OK &&
						   (file_copy_9 = (char *)malloc(
							    nb_buf_len(&file_value_9) + 1)) != NULL
					   ? (memcpy(file_copy_9, nb_buf_cstr(&file_value_9),
						     nb_buf_len(&file_value_9) + 1),
					      nb_charp_steal(&file_copy_9))
					   : NULL);
		if (!published || !merged || strcmp(published, merged) != 0)
			unlink(snapshot);
		free(published);
	}
	free(prompt_text);
	free(snapshot);
	free(merged);
	nb_buf_cleanup(&patch);
	return rc;
}

static int cli_wizard_setup_run(struct cli_application *ctx, const char *path, FILE *input,
				FILE *output, int reconfigure)
{
	nb_buf file_value_10 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_11 NB_CLEANUP(nb_buf_destroy) = {0};

	struct cli_setup_state state = {.reconfigure = reconfigure};
	char *original = NULL;
	char current[PATH_MAX];
	char *expanded = path ? (nb_path_expand(&file_value_10, path, NULL) == NB_OK
					 ? strdup(nb_buf_cstr(&file_value_10))
					 : NULL)
			      : NULL;
	char *full = expanded && getcwd(current, sizeof(current))
			     ? (nb_path_join(&file_value_11, current, expanded) == NB_OK
					? strdup(nb_buf_cstr(&file_value_11))
					: NULL)
			     : NULL;
	free(expanded);
	if (!full)
		return NB_ERR_INVAL;
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
		fprintf(stderr, "morph: setup failed: %s\n",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	return rc < 0 ? 1 : 0;
}

int cli_setup_if_missing(struct cli_application *ctx, const char *path, int interactive)
{
	nb_buf file_value_12 NB_CLEANUP(nb_buf_destroy) = {0};
	int expand_status_12 = NB_OK;

	char *expanded =
		((expand_status_12 = nb_path_expand(&file_value_12,
			path ? path : "~/.morph/config.toml", NULL)) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_12))
			 : NULL);
	if (!expanded)
		return expand_status_12 != NB_OK ? expand_status_12 : NB_ERR_NOMEM;
	struct stat st;
	if (lstat(expanded, &st) == 0) {
		free(expanded);
		return 0;
	}
	int rc = nb_status_from_errno(errno, NULL);
	if (rc == NB_ERR_NOT_FOUND) {
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
	if (rc == NB_ERR_CANCELLED)
		fprintf(stderr, "Setup canceled. No configuration was saved.\n");
	else if (rc < 0)
		fprintf(stderr, "morph: setup failed: %s\n",
			nb_err_str(morph_error_descriptions(), MORPH_ERROR_COUNT, rc));
	free(expanded);
	if (rc < 0)
		return rc;
	return rc;
}
