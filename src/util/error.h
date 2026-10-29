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

/* Context-free returns never emit diagnostics. Pass an owned logger explicitly
 * with the _LOG variants to retain source locations in Debug file logs.
 * Release builds do not evaluate the logger expression. */
#ifdef DEBUG
#include "log.h"

#define MORPH_RETURN_LOG(logger, code)                                                 \
	do {                                                                          \
		morph_err_t _rc_ = (code);                                             \
		if (_rc_ < 0) {                                                       \
			int _saved_errno_ = errno;                                    \
			log_diagnostic((logger), "MORPH_RETURN: %s (%s:%d)",            \
				morph_strerror(_rc_), __FILE__, __LINE__);              \
			errno = _saved_errno_;                                        \
		}                                                                     \
		return _rc_;                                                          \
	} while (0)

#define MORPH_SET_ERR_LOG(logger, var, code)                                           \
	do {                                                                          \
		morph_err_t _rc_ = (code);                                             \
		(var) = _rc_;                                                         \
		if (_rc_ < 0) {                                                       \
			int _saved_errno_ = errno;                                    \
			log_diagnostic((logger), "MORPH_SET_ERR: %s (%s:%d)",           \
				morph_strerror(_rc_), __FILE__, __LINE__);              \
			errno = _saved_errno_;                                        \
		}                                                                     \
	} while (0)
#else
#define MORPH_RETURN_LOG(logger, code) return (code)
#define MORPH_SET_ERR_LOG(logger, var, code) ((var) = (code))
#endif

#define MORPH_RETURN(code) MORPH_RETURN_LOG(NULL, code)
#define MORPH_SET_ERR(var, code) MORPH_SET_ERR_LOG(NULL, var, code)

#define MORPH_RETURN_ERRNO_LOG(logger)                                                \
	do {                                                                          \
		int _err_ = errno;                                                    \
		if (_err_ == 0)                                                       \
			_err_ = EIO;                                                  \
		MORPH_RETURN_LOG(logger, -_err_);                                      \
	} while (0)

#define MORPH_SET_ERRNO_LOG(logger, var)                                               \
	do {                                                                          \
		int _err_ = errno;                                                    \
		if (_err_ == 0)                                                       \
			_err_ = EIO;                                                  \
		MORPH_SET_ERR_LOG(logger, var, -_err_);                                \
	} while (0)

#define MORPH_RETURN_ERRNO() MORPH_RETURN_ERRNO_LOG(NULL)
#define MORPH_SET_ERRNO(var) MORPH_SET_ERRNO_LOG(NULL, var)

#ifdef __cplusplus
}
#endif

#endif
