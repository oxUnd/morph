#include "util/error.h"
#include "util/log.h"

int error_probe_debug(void)
{
#ifdef DEBUG
	return 1;
#else
	return 0;
#endif
}

int error_probe_return(struct morph_log *logger, int code,
		       int *code_evaluations, int *logger_evaluations)
{
	(void)logger;
	(void)logger_evaluations;
	MORPH_RETURN_LOG((++*logger_evaluations, logger), (++*code_evaluations, code));
}

int error_probe_set(struct morph_log *logger, int code, int *evaluations)
{
	int result = 0;

	(void)logger;
	MORPH_SET_ERR_LOG(logger, result, (++*evaluations, code));
	return result;
}

int error_probe_errno(struct morph_log *logger, int assign)
{
	int result;

	(void)logger;
	if (assign) {
		MORPH_SET_ERRNO_LOG(logger, result);
		return result;
	}
	MORPH_RETURN_ERRNO_LOG(logger);
}

int error_probe_without_context(int assign)
{
	int result;

	if (assign) {
		MORPH_SET_ERR(result, MORPH_ERR_DB);
		return result;
	}
	MORPH_RETURN(MORPH_ERR_DB);
}
