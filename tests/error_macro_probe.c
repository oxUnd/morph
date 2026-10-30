#include "util/error.h"
#include "util/log.h"

int error_probe_return(struct morph_log *logger, int code,
		       int *code_evaluations, int *logger_evaluations)
{
	(void)logger;
	(void)logger_evaluations;
	return MORPH_TRACE_ERROR((++*logger_evaluations, logger), (++*code_evaluations, code));
}

int error_probe_set(struct morph_log *logger, int code, int *evaluations)
{
	int result = 0;

	(void)logger;
	result = MORPH_TRACE_ERROR(logger, (++*evaluations, code));
	return result;
}

int error_probe_errno(struct morph_log *logger, int assign)
{
	int result;

	(void)logger;
	if (assign) {
		result = MORPH_TRACE_ERROR(logger, morph_error_from_errno(errno));
		return result;
	}
	return MORPH_TRACE_ERROR(logger, morph_error_from_errno(errno));
}

int error_probe_without_context(int assign)
{
	int result;

	if (assign) {
		result = (MORPH_ERR_DB);
		return result;
	}
	return MORPH_ERR_DB;
}

long long error_probe_wide(void)
{
	long long value = 4294967296LL;

	return value;
}

int error_probe_local_name(void)
{
	int _rc_ = -5;

	return MORPH_TRACE_ERROR(NULL, _rc_);
}

int error_probe_assignment(void)
{
	int rc = 0;

	return (rc = -5);
}
