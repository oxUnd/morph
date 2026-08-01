#include "error.h"

int morph_err_is_errno(morph_err_t err)
{
	return err < 0 && err > MORPH_ERR_BASE;
}

int morph_err_is_domain(morph_err_t err)
{
	return err <= MORPH_ERR_BASE;
}

const char *morph_errname(morph_err_t err)
{
	switch (err) {
	case 0:                         return "OK";
	case -E2BIG:                    return "E2BIG";
	case -EACCES:                   return "EACCES";
	case -EAGAIN:                   return "EAGAIN";
	case -EBADF:                    return "EBADF";
	case -EBUSY:                    return "EBUSY";
	case -ECANCELED:                return "ECANCELED";
	case -EEXIST:                   return "EEXIST";
	case -EFBIG:                    return "EFBIG";
	case -EINVAL:                   return "EINVAL";
	case -EIO:                      return "EIO";
	case -EINTR:                    return "EINTR";
	case -ELOOP:                    return "ELOOP";
	case -ENOMEM:                   return "ENOMEM";
	case -ENAMETOOLONG:             return "ENAMETOOLONG";
#ifdef ENODATA
	case -ENODATA:                  return "ENODATA";
#endif
	case -ENOENT:                   return "ENOENT";
	case -ENOSPC:                   return "ENOSPC";
	case -ENOSYS:                   return "ENOSYS";
#ifdef ENOTSUP
	case -ENOTSUP:                  return "ENOTSUP";
#endif
	case -ENOTTY:                   return "ENOTTY";
	case -EOVERFLOW:                return "EOVERFLOW";
	case -EPERM:                    return "EPERM";
#ifdef EPROTO
	case -EPROTO:                   return "EPROTO";
#endif
	case -ETIMEDOUT:                return "ETIMEDOUT";
	case MORPH_ERR_NOT_CONFIGURED:  return "MORPH_ERR_NOT_CONFIGURED";
	case MORPH_ERR_NOT_INITIALIZED: return "MORPH_ERR_NOT_INITIALIZED";
	case MORPH_ERR_API:             return "MORPH_ERR_API";
	case MORPH_ERR_NETWORK:         return "MORPH_ERR_NETWORK";
	case MORPH_ERR_PARSE:           return "MORPH_ERR_PARSE";
	case MORPH_ERR_PROTOCOL:        return "MORPH_ERR_PROTOCOL";
	case MORPH_ERR_DB:              return "MORPH_ERR_DB";
	case MORPH_ERR_FORMAT:          return "MORPH_ERR_FORMAT";
	case MORPH_ERR_PROCESSING:      return "MORPH_ERR_PROCESSING";
	case MORPH_ERR_SANDBOX:         return "MORPH_ERR_SANDBOX";
	case MORPH_ERR_LOAD:            return "MORPH_ERR_LOAD";
	case MORPH_ERR_LLM:             return "MORPH_ERR_LLM";
	case MORPH_ERR_REACT_MAX_ITERATIONS:
		return "MORPH_ERR_REACT_MAX_ITERATIONS";
	case MORPH_ERR_CONFIG:          return "MORPH_ERR_CONFIG";
	default:
		return "MORPH_ERR_UNKNOWN";
	}
}

const char *morph_strerror(morph_err_t err)
{
	switch (err) {
	case MORPH_ERR_NOT_CONFIGURED:  return "not configured";
	case MORPH_ERR_NOT_INITIALIZED: return "not initialized";
	case MORPH_ERR_API:             return "API error";
	case MORPH_ERR_NETWORK:         return "network error";
	case MORPH_ERR_PARSE:           return "parse error";
	case MORPH_ERR_PROTOCOL:        return "protocol error";
	case MORPH_ERR_DB:              return "database error";
	case MORPH_ERR_FORMAT:          return "invalid format";
	case MORPH_ERR_PROCESSING:      return "processing error";
	case MORPH_ERR_SANDBOX:         return "sandbox violation";
	case MORPH_ERR_LOAD:            return "load error";
	case MORPH_ERR_LLM:             return "LLM error";
	case MORPH_ERR_REACT_MAX_ITERATIONS:
		return "maximum ReAct iterations reached";
	case MORPH_ERR_CONFIG:          return "invalid configuration";
	default:
		if (morph_err_is_errno(err))
			return strerror(-err);
		return "unknown error";
	}
}
