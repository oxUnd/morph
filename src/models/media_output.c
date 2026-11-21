#include "media_output.h"
#include "http/request.h"
#include "media/image.h"
#include "errors.h"
#include <nb/cleanup.h>
#include <nb/file.h>
#include <nb/random.h>
#include <nb/log.h>
#include <string.h>

int media_output_download(const struct http_control *control, const char *url,
	const char *output_dir, const char *prefix, const char *extension,
	char *path, size_t path_size)
{
	if (!url || !prefix || !path || !path_size)
		return NB_ERR_INVAL;

	path[0] = '\0';
	nb_buf body NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_http_client *client NB_CLEANUP(http_client_cleanup) = NULL;
	nb_http_call *call NB_CLEANUP(http_call_cleanup) = NULL;
	nb_http_request request;
	http_request_init(&request, "GET", url);
	int rc = http_client_open(control, &client);
	if (rc == 0)
		rc = http_request_run(control, client, &request, NULL, &call);

	if (rc == 0)
		rc = nb_http_call_take_body(call, &body, NULL);

	if (rc != 0)
		return rc;

	const nb_http_result *result = nb_http_call_result(call);
	const nb_http_response *response = result ? result->response : NULL;
	if (!response)
		return NB_ERR_PROTOCOL;

	if (response->status_code != 200) {
		nb_log_write(control ? control->logger : NULL, NB_LOG_ERROR,
			"media download returned HTTP %d", response->status_code);
		return MORPH_ERR_API;
	}

	if (!extension) {
		nb_str type = NB_STR_NULL;
		(void)nb_http_headers_get(response->headers, NB_STR_LIT("Content-Type"), &type);
		extension = image_format_extension(image_format_from_mime(type));
		if (!extension)
			extension = image_format_extension(image_format_detect(
				(const unsigned char *)body.ptr, body.len));

		if (!extension)
			extension = "png";
	}

	return media_output_write(output_dir, prefix, extension, body.ptr, body.len,
		path, path_size);
}

static int media_output_attempt(const char *output_dir, const char *prefix, const char *extension,
				const void *data, size_t len, char *path, size_t path_size)
{
	nb_buf directory NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf name NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf destination NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_file_write_options options = {.mode = 0600, .create_only = true, .sync_dir = true};

	if (!path || path_size == 0)
		return NB_ERR_INVAL;
	path[0] = '\0';
	if (!prefix || !*prefix || !extension || !*extension || strchr(prefix, '/') ||
	    strchr(extension, '/') || (!data && len))
		return NB_ERR_INVAL;
	int rc = nb_path_expand(&directory,
				output_dir && *output_dir ? output_dir : "~/.morph/output", NULL);
	if (rc != 0)
		return rc;
	rc = nb_file_mkdir(nb_buf_cstr(&directory), true, NULL);
	if (rc != 0)
		return rc;
	if (!nb_buf_printf(&name, "%s_", prefix))
		return nb_buf_status(&name);
	rc = nb_random_hex(&name, 8, NULL);
	if (rc != 0)
		return rc;
	if (!nb_buf_printf(&name, ".%s", extension))
		return nb_buf_status(&name);
	rc = nb_path_concat(&destination, nb_buf_cstr(&directory), nb_buf_cstr(&name));
	if (rc != 0)
		return rc;
	if (nb_buf_len(&destination) >= path_size)
		return NB_ERR_NAME_TOO_LONG;
	rc = nb_file_write_all_atomic(nb_buf_cstr(&destination), data, len, &options, NULL);
	if (rc == 0)
		memcpy(path, nb_buf_cstr(&destination), nb_buf_len(&destination) + 1);
	return rc;
}

int media_output_write(const char *output_dir, const char *prefix, const char *extension,
		       const void *data, size_t len, char *path, size_t path_size)
{
	int rc;

	for (int attempt = 0; attempt < 16; attempt++) {
		rc = media_output_attempt(output_dir, prefix, extension, data, len, path,
					  path_size);
		if (rc != NB_ERR_EXISTS)
			return rc;
	}
	return rc;
}
