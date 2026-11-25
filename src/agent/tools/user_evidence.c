#include "user_evidence.h"
#include "persistence/orm/business_store.h"

#include "agent/tool_runtime.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include "errors.h"
#include <nb/utf8.h>

#include <errno.h>
#include <string.h>

int tool_user_evidence_token_copy(const struct tool_runtime_context *runtime, const char *quote,
				  char **out)
{
	char *input NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf token NB_CLEANUP(nb_buf_cleanup) = {0};
	int64_t id = 0;

	if (!runtime || !db_is_open(runtime->db) || !out || !quote || strlen(quote) > 4096 ||
	    nb_utf8_validate(nb_str_from_cstr(quote), NULL) ||
	    nb_utf8_count(nb_str_from_cstr(quote)) < 4)
		return NB_ERR_INVAL;

	if (*out)
		return NB_ERR_EXISTS;

	int rc = evidence_store_latest_user_copy(runtime->db, runtime->memory_session_id, &id,
						 &input);

	if (rc != 0)
		return rc == NB_ERR_NOT_FOUND ? NB_ERR_NOT_PERMITTED : rc;

	if (!input || !strstr(input, quote))
		return NB_ERR_NOT_PERMITTED;

	rc = nb_buf_printf(&token, "history:%lld", (long long)id) ? 0 : NB_ERR_NOMEM;
	if (rc == 0) {
		*out = strdup(nb_buf_cstr(&token));
		if (!*out)
			rc = NB_ERR_NOMEM;
	}

	return rc;
}
