#ifndef MORPH_RUNTIME_JOBS_SERVICE_H
#define MORPH_RUNTIME_JOBS_SERVICE_H

struct morph_event;
struct runtime;

#ifdef __cplusplus
extern "C" {
#endif

/* Exec's event adapter persists process snapshots and forwards every event.
 * Other background events, including waits, have no process owner payload. */
int runtime_jobs_event(const struct morph_event *event, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
