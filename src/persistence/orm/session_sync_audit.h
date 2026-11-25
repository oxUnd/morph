#ifndef MORPH_SESSION_SYNC_AUDIT_H
#define MORPH_SESSION_SYNC_AUDIT_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads only owned private copies; input databases and sidecars are untouched. */
int session_sync_audit_copy(const char *core_path, const char *ui_path, const char *platform,
			    cJSON **out);

#ifdef __cplusplus
}
#endif

#endif
