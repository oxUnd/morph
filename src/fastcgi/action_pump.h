/* action_pump.h — bridges fcgi_actions queue → ReAct
 *
 * In the MVP build, ReAct doesn't know about the action pump.  When the
 * agent reaches a guardrail/awaiting-approval point it should call
 * `action_pump_wait_decision()` and consume one entry from the queue.
 *
 * For now, this lives behind a weak symbol on the agent side; if not
 * linked, the FastCGI process simply ignores actions other than persisting
 * them for audit.  See PATCHES.md for the optional 1-line agent hook.
 */
#ifndef MORPH_FCGI_ACTION_PUMP_H
#define MORPH_FCGI_ACTION_PUMP_H

#include "session_store.h"

/* Block until an action arrives or `timeout_sec` expires.
 * On success: fills `out`, returns 1.  On timeout/empty: returns 0.
 * Caller frees `out->payload_json`.
 */
int action_pump_wait(struct session_store *s, const char *session_id,
		     int timeout_sec, struct action_record *out);

#endif
