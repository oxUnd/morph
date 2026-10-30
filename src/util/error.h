#ifndef MORPH_ERROR_H
#define MORPH_ERROR_H

#include <errno.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int morph_err_t;

#define MORPH_ERR_BASE (-256)

enum morph_error {
	MORPH_ERR_NOT_CONFIGURED = MORPH_ERR_BASE - 1,
	MORPH_ERR_NOT_INITIALIZED = MORPH_ERR_BASE - 2,
	MORPH_ERR_API = MORPH_ERR_BASE - 3,
	MORPH_ERR_NETWORK = MORPH_ERR_BASE - 4,
	MORPH_ERR_PARSE = MORPH_ERR_BASE - 5,
	MORPH_ERR_PROTOCOL = MORPH_ERR_BASE - 6,
	MORPH_ERR_DB = MORPH_ERR_BASE - 7,
	MORPH_ERR_FORMAT = MORPH_ERR_BASE - 8,
	MORPH_ERR_PROCESSING = MORPH_ERR_BASE - 9,
	MORPH_ERR_SANDBOX = MORPH_ERR_BASE - 10,
	MORPH_ERR_LOAD = MORPH_ERR_BASE - 11,
	MORPH_ERR_LLM = MORPH_ERR_BASE - 12,
	MORPH_ERR_REACT_MAX_ITERATIONS = MORPH_ERR_BASE - 13,
	MORPH_ERR_CONFIG = MORPH_ERR_BASE - 14,
	MORPH_ERR_CONTEXT_LIMIT = MORPH_ERR_BASE - 15,
};

const char *morph_strerror(morph_err_t err);
const char *morph_errname(morph_err_t err);
int morph_err_is_errno(morph_err_t err);
int morph_err_is_domain(morph_err_t err);

struct morph_log;

/* Capture errno at the failing operation, before cleanup can overwrite it. */
static inline morph_err_t morph_error_from_errno(int captured_errno)
{
	return captured_errno ? -captured_errno : -EIO;
}

/* Return the code unchanged; tracing is controlled by the explicit logger's level.
 * All arguments are evaluated once in every build configuration. */
morph_err_t morph_error_trace(struct morph_log *logger, morph_err_t code,
			      const char *file, int line);

/* This expression only supplies the call-site location; it never returns or assigns. */
#define MORPH_TRACE_ERROR(logger, code) \
	morph_error_trace((logger), (code), __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif
