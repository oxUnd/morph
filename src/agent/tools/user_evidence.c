#include "user_evidence.h"

#include "agent/tool_runtime.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/utf8.h"

#include <errno.h>
#include <string.h>

static void user_evidence_statement_cleanup(sqlite3_stmt **statement)
{
	sqlite3_finalize(*statement);
}

int tool_user_evidence_token_copy(const struct tool_runtime_context *runtime, const char *quote,
				  char **out)
{
	sqlite3_stmt *stmt NB_CLEANUP(user_evidence_statement_cleanup) = NULL;
	nb_buf token NB_CLEANUP(nb_buf_cleanup) = {0};

	if (!runtime || !runtime->db || !runtime->db->handle || !out || !quote ||
	    strlen(quote) > 4096 || nb_utf8_validate(nb_str_from_cstr(quote), NULL) ||
	    nb_utf8_count(nb_str_from_cstr(quote)) < 4)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (sqlite3_prepare_v2(
		    runtime->db->handle,
		    "SELECT id,content FROM model_history_items WHERE session_id=? AND role='user' "
		    "AND active=1 AND kind IN ('user_message','user_steer') "
		    "ORDER BY sequence_no DESC LIMIT 1",
		    -1, &stmt, NULL) != SQLITE_OK)
		return MORPH_ERR_DB;
	sqlite3_bind_int64(stmt, 1, runtime->memory_session_id);
	int step = sqlite3_step(stmt);

	if (step != SQLITE_ROW)
		return step == SQLITE_DONE ? NB_ERR_NOT_PERMITTED : MORPH_ERR_DB;
	const char *input = (const char *)sqlite3_column_text(stmt, 1);

	if (!input || !strstr(input, quote))
		return NB_ERR_NOT_PERMITTED;
	int rc = (nb_buf_printf(&token, "history:%lld", (long long)sqlite3_column_int64(stmt, 0))
			  ? 0
			  : NB_ERR_NOMEM);

	if (rc == 0) {
		*out = (nb_buf_ok(&token) ? strdup(nb_buf_cstr(&token)) : NULL);
		if (!*out)
			rc = NB_ERR_NOMEM;
	}
	return rc;
}
