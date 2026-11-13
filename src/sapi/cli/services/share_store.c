#include "sapi/cli/services/share_store.h"
#include "nb/err.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "sapi/cli/presentation/share.h"
#include "nb/file.h"
#include "nb/file.h"
#include "nb/random.h"
#include "nb/encoding.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Reserve a private random filename; publication never overwrites a share. */
static int cli_share_store_share_publish(const char *directory, const char *stamp,
					 const nb_buf *html, char **out_path)
{
	unsigned char random[12];
	nb_buf name NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf suffix NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = nb_random_bytes(random, sizeof(random), NULL);
	if (rc != NB_OK)
		return rc;
	if (!nb_buf_printf(&name, "morph-share-%s-", stamp))
		return NB_ERR_NOMEM;
	rc = nb_hex_encode(random, sizeof(random), false, &suffix, NULL);
	if (rc != NB_OK)
		return rc;
	if (!nb_buf_append_str(&name, nb_buf_str(&suffix)) || !nb_buf_append_cstr(&name, ".html"))
		return NB_ERR_NOMEM;
	rc = nb_path_join(&path, directory, nb_buf_cstr(&name));
	if (rc != NB_OK)
		return rc;
	char *published NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&path));
	if (!published)
		return NB_ERR_NOMEM;
	nb_file_write_options options = {
		.new_file_mode = 0600,
		.create_only = true,
		.sync_dir = true,
	};
	rc = nb_file_write_all_atomic(nb_buf_cstr(&path), nb_buf_cstr(html), nb_buf_len(html),
				      &options, NULL);
	if (rc != NB_OK)
		return rc;
	*out_path = nb_charp_steal(&published);
	return NB_OK;
}

int cli_share_store_save(const cJSON *document, char **out_path)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	if (!out_path)
		return NB_ERR_INVAL;
	if (*out_path)
		return NB_ERR_EXISTS;
	nb_buf html NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&html), nb_buf_reserve(&html, 4096) ? 0 : NB_ERR_NOMEM);
	if (rc)
		return rc;
	char *directory NB_CLEANUP(nb_charp_cleanup) = NULL;
	time_t now = time(NULL);
	rc = cli_share_html(document, now, &html);
	if (rc)
		return rc;
	const char *home = getenv("HOME");
	if (!home || !home[0]) {
		rc = (NB_ERR_NOT_FOUND);
		return rc;
	}
	directory = (nb_path_concat(&file_value_1, home, "Downloads") == NB_OK
			     ? strdup(nb_buf_cstr(&file_value_1))
			     : NULL);
	if (!directory) {
		rc = (NB_ERR_NOMEM);
		return rc;
	}
	rc = nb_file_mkdir(directory, true, NULL);
	if (rc)
		return rc;
	struct tm local;
	char stamp[32];
	if (!localtime_r(&now, &local) ||
	    !strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local)) {
		rc = (NB_ERR_INVAL);
		return rc;
	}
	rc = cli_share_store_share_publish(directory, stamp, &html, out_path);
	return rc;
}
