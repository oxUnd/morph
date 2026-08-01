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
	MORPH_ERR_NOT_CONFIGURED  = MORPH_ERR_BASE - 1,
	MORPH_ERR_NOT_INITIALIZED = MORPH_ERR_BASE - 2,
	MORPH_ERR_API             = MORPH_ERR_BASE - 3,
	MORPH_ERR_NETWORK         = MORPH_ERR_BASE - 4,
	MORPH_ERR_PARSE           = MORPH_ERR_BASE - 5,
	MORPH_ERR_PROTOCOL        = MORPH_ERR_BASE - 6,
	MORPH_ERR_DB              = MORPH_ERR_BASE - 7,
	MORPH_ERR_FORMAT          = MORPH_ERR_BASE - 8,
	MORPH_ERR_PROCESSING      = MORPH_ERR_BASE - 9,
	MORPH_ERR_SANDBOX         = MORPH_ERR_BASE - 10,
	MORPH_ERR_LOAD            = MORPH_ERR_BASE - 11,
	MORPH_ERR_LLM             = MORPH_ERR_BASE - 12,
	MORPH_ERR_REACT_MAX_ITERATIONS = MORPH_ERR_BASE - 13,
	MORPH_ERR_CONFIG          = MORPH_ERR_BASE - 14,
};

const char *morph_strerror(morph_err_t err);
const char *morph_errname(morph_err_t err);
int morph_err_is_errno(morph_err_t err);
int morph_err_is_domain(morph_err_t err);

#ifdef DEBUG
#include "log.h"

#define MORPH_RETURN(code)                                              \
	do {                                                            \
		morph_err_t _rc_ = (code);                              \
		if (_rc_ < 0)                                           \
			log_err("MORPH_RETURN: %s (%s:%d)",             \
				morph_strerror(_rc_), __FILE__, __LINE__); \
		return _rc_;                                            \
	} while (0)

#define MORPH_SET_ERR(var, code)                                        \
	do {                                                            \
		(var) = (code);                                         \
		if ((var) < 0)                                          \
			log_err("MORPH_SET_ERR: %s (%s:%d)",            \
				morph_strerror(var), __FILE__, __LINE__); \
	} while (0)
#else
#define MORPH_RETURN(code) return (code)
#define MORPH_SET_ERR(var, code) ((var) = (code))
#endif

#define MORPH_RETURN_ERRNO()                                            \
	do {                                                            \
		int _err_ = errno;                                      \
		if (_err_ == 0)                                        \
			_err_ = EIO;                                   \
		MORPH_RETURN(-_err_);                                  \
	} while (0)

#define MORPH_SET_ERRNO(var)                                            \
	do {                                                            \
		int _err_ = errno;                                      \
		if (_err_ == 0)                                        \
			_err_ = EIO;                                   \
		MORPH_SET_ERR((var), -_err_);                          \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif
