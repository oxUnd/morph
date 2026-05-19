/* action_pump.c — drain-with-wait wrapper around the actions queue */
#include "action_pump.h"

int action_pump_wait(struct session_store *s, const char *session_id,
		     int timeout_sec, struct action_record *out) {
	if (actions_drain_one(s, session_id, out)) return 1;
	if (!actions_wait(s, session_id, timeout_sec)) return 0;
	return actions_drain_one(s, session_id, out);
}
