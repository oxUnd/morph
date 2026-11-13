#include "sapi/cli/services/composer.h"
#include "sapi/cli/adapters/media/image_probe.h"
#include "sapi/cli/models/composer.h"
#include "util/buf.h"
#include "util/cleanup.h"
#include "util/error.h"
#include "util/file.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cli_composer_service_add_image(struct cli_composer_model *composer, const char *path,
				   const char **label)
{
	char *expanded;
	char *absolute;
	int width;
	int height;
	int rc;

	if (!composer || !path || !label)
		return -EINVAL;
	expanded = file_expand_path_copy(path);
	if (!expanded)
		return -ENOMEM;
	absolute = realpath(expanded, NULL);
	free(expanded);
	if (!absolute)
		return morph_error_from_errno(errno);
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
	morph_buf_t path MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t result MORPH_CLEANUP(morph_buf_cleanup) = {0};
	const char *cursor = input;
	const char *consumed = input;
	int converted = 0;
	int rc;

	if (!composer || !input || !output)
		return -EINVAL;
	if (*output)
		return -EEXIST;
	rc = morph_buf_init(&path, BUFSIZ);
	if (rc != 0)
		return rc;
	rc = morph_buf_init(&result, strlen(input) + 1);
	if (rc != 0) {
		morph_buf_cleanup(&path);
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
		morph_buf_reset(&path);
		while (*cursor && (quote || !isspace((unsigned char)*cursor))) {
			char ch = *cursor++;

			if (ch == '\\' && quote != '\'' && *cursor) {
				(void)morph_buf_putc(&path, *cursor++);
			} else if (ch == quote) {
				quote = 0;
			} else if (!quote && (ch == '\'' || ch == '"')) {
				quote = ch;
			} else {
				(void)morph_buf_putc(&path, ch);
			}
		}
		if (quote || path.failed)
			break;
		expanded = file_expand_path_copy(morph_buf_cstr_borrow(&path));
		if (!expanded)
			break;
		if (!file_exists(expanded)) {
			free(expanded);
			continue;
		}
		rc = cli_composer_service_add_image(composer, expanded, &label);
		free(expanded);
		if (rc != 0)
			continue;
		(void)morph_buf_append(&result, consumed, (size_t)(start - consumed));
		(void)morph_buf_puts(&result, label);
		consumed = cursor;
		converted++;
	}
	(void)morph_buf_puts(&result, consumed);
	if (result.failed || path.failed)
		rc = -ENOMEM;
	else {
		*output = morph_buf_detach(&result);
		rc = converted;
	}
	morph_buf_cleanup(&result);
	morph_buf_cleanup(&path);
	return rc;
}
