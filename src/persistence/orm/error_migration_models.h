#ifndef MORPH_ORM_ERROR_MIGRATION_MODELS_H
#define MORPH_ORM_ERROR_MIGRATION_MODELS_H

#include <norm/query.h>

#define ERROR_MIGRATION_MAPPING_COUNT 39

struct error_migration_expression {
	struct norm_term input;
	struct norm_term legacy[ERROR_MIGRATION_MAPPING_COUNT];
	struct norm_term current[ERROR_MIGRATION_MAPPING_COUNT];
	struct norm_case cases[ERROR_MIGRATION_MAPPING_COUNT];
	struct norm_term zero;
	struct norm_term unknown;
	struct norm_case positive;
	struct norm_term fallback;
	struct norm_term value;
};

void db_error_migration_expression(struct error_migration_expression *out, struct norm_term input);

#endif
