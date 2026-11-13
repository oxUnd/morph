#include "sapi/cli/services/composer.h"
#include "sapi/cli/adapters/media/image_probe.h"
#include "sapi/cli/models/composer.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cli_composer_service_add_image(struct cli_composer_model *composer, const char *path,
				   const char **label)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	char *expanded;
	char *absolute;
	int width;
	int height;
	int rc;

	if (!composer || !path || !label)
		return NB_ERR_INVAL;
	expanded = (nb_path_expand(&file_value_1, path, NULL) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_1))
			    : NULL);
	if (!expanded)
		return NB_ERR_NOMEM;
	absolute = realpath(expanded, NULL);
	free(expanded);
	if (!absolute)
		return nb_status_from_errno(errno, NULL);
	rc = cli_media_image_probe_info(absolute, &width, &height);
	if (rc != 0) {
		free(absolute);
		return rc;
	}
	rc = cli_composer_model_add_image_resolved(composer, absolute, width, height, label);
	free(absolute);
	return rc;
}

int cli_composer_service_convert_paths_copy(struct cli_composer_model *composer,
					    const char *input, char **output)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};

	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf result NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *cursor = input;
	const char *consumed = input;
	int converted = 0;
	int rc;

	if (!composer || !input || !output)
		return NB_ERR_INVAL;
	if (*output)
		return NB_ERR_EXISTS;
	rc = (nb_buf_init(&path), nb_buf_reserve(&path, BUFSIZ) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = (nb_buf_init(&result), nb_buf_reserve(&result, strlen(input) + 1) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		nb_buf_cleanup(&path);
		return rc;
	}
	while (*cursor) {
		const char *start;
		const char *label;
		char quote = 0;
		char *expanded;

		while (isspace((unsigned char)*cursor))
			cursor++;
		if (!*cursor)
			break;
		start = cursor;
		nb_buf_clear(&path);
		while (*cursor && (quote || !isspace((unsigned char)*cursor))) {
			char ch = *cursor++;

			if (ch == '\\' && quote != '\'' && *cursor) {
				(void)nb_buf_append_char(&path, *cursor++);
			} else if (ch == quote) {
				quote = 0;
			} else if (!quote && (ch == '\'' || ch == '"')) {
				quote = ch;
			} else {
				(void)nb_buf_append_char(&path, ch);
			}
		}
		if (quote || nb_buf_status(&path))
			break;
		expanded = (nb_path_expand(&file_value_2, nb_buf_cstr(&path), NULL) == NB_OK
				    ? strdup(nb_buf_cstr(&file_value_2))
				    : NULL);
		if (!expanded)
			break;
		if (!nb_file_exists(expanded)) {
			free(expanded);
			continue;
		}
		rc = cli_composer_service_add_image(composer, expanded, &label);
		free(expanded);
		if (rc != 0)
			continue;
		(void)nb_buf_append(&result, consumed, (size_t)(start - consumed));
		(void)nb_buf_append_cstr(&result, label);
		consumed = cursor;
		converted++;
	}
	(void)nb_buf_append_cstr(&result, consumed);
	if (nb_buf_status(&result) || nb_buf_status(&path))
		rc = NB_ERR_NOMEM;
	else {
		*output = (nb_buf_ok(&result) ? strdup(nb_buf_cstr(&result)) : NULL);
		rc = *output ? converted : NB_ERR_NOMEM;
	}
	nb_buf_cleanup(&result);
	nb_buf_cleanup(&path);
	return rc;
}
