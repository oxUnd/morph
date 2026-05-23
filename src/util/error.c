#include "error.h"

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
	default:
		if (err < 0 && err > MORPH_ERR_BASE)
			return strerror(-err);
		return "unknown error";
	}
}
