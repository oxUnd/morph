#include "errors.h"
#include "nb/log.h"

int error_probe_return(struct nb_log *logger, int code, int *code_evaluations,
		       int *logger_evaluations)
{
	(void)logger;
	(void)logger_evaluations;
	return nb_err_trace((++*logger_evaluations, logger), (++*code_evaluations, code),
			    morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__, __LINE__);
}

int error_probe_set(struct nb_log *logger, int code, int *evaluations)
{
	int result = 0;

	(void)logger;
	result = nb_err_trace(logger, (++*evaluations, code), morph_error_descriptions(),
			      MORPH_ERROR_COUNT, __FILE__, __LINE__);
	return result;
}

int error_probe_errno(struct nb_log *logger, int assign)
{
	int result;

	(void)logger;
	if (assign) {
		result = nb_err_trace(logger, nb_status_from_errno(errno, NULL),
				      morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
				      __LINE__);
		return result;
	}
	return nb_err_trace(logger, nb_status_from_errno(errno, NULL), morph_error_descriptions(),
			    MORPH_ERROR_COUNT, __FILE__, __LINE__);
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

	return nb_err_trace(NULL, _rc_, morph_error_descriptions(), MORPH_ERROR_COUNT, __FILE__,
			    __LINE__);
}

int error_probe_assignment(void)
{
	int rc = 0;

	return (rc = -5);
}
