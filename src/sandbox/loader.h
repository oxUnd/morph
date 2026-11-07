#ifndef LOADER_H
#define LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

struct ext;

/* ex must not already hold a library/entrypoint; occupied: -EEXIST.
 * Failure preserves it. The manifest is borrowed for loading the backend. */
int ext_load_so(struct ext *ex, const char *path);
int ext_load_exec(struct ext *ex, const char *path);
void ext_unload_so(struct ext *ex);

#ifdef __cplusplus
}
#endif

#endif
