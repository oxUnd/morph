#ifndef MORPH_PERMISSION_TRUSTED_PATHS_H
#define MORPH_PERMISSION_TRUSTED_PATHS_H

#include "nb/buf.h"
#include "nb/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PERMISSION_TEMPORARY_ROOTS_MAX 4u

/* Canonical roots approved by the shared temporary-directory policy. The owner
 * outlives every borrowed name used by a permission check or sandbox rule. */
struct permission_temporary_paths {
	nb_buf roots[PERMISSION_TEMPORARY_ROOTS_MAX];
	size_t count;
};

/* Output must be empty. Missing/invalid optional roots are omitted; allocation
 * and other construction failures preserve output. TMPDIR must be an existing
 * absolute directory, cannot be filesystem root, and cannot contain home.
 * Inputs are borrowed only during collection; NULL omits each optional value. */
nb_status permission_temporary_paths_collect(struct permission_temporary_paths *out,
					     const char *temporary, const char *home);
void permission_temporary_paths_cleanup(struct permission_temporary_paths *paths);
int permission_temporary_path_is_default(const char *path);

#ifdef __cplusplus
}
#endif
#endif
