#ifndef MORPH_ORM_SESSION_IMPORT_H
#define MORPH_ORM_SESSION_IMPORT_H

#include "persistence/orm/database.h"

struct runtime_session_import_result {
	int imported;
	int unchanged;
};

int orm_session_import_file(struct db *target, const char *path, const char *origin,
			    struct runtime_session_import_result *result);

#endif
