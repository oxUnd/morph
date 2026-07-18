#ifndef MORPH_RUNTIME_USAGE_H
#define MORPH_RUNTIME_USAGE_H

#include "config.h"
#include "db/database.h"
#include "models/llm.h"

void *runtime_usage_bind(void *user_data);
void runtime_usage_restore(void *previous);
int runtime_model_usage_is_billable(const struct model_usage *usage);
char *runtime_model_usage_metadata(const struct model_usage *usage);
int runtime_record_model_usage(struct db *db,
			       const struct config *config,
			       const char *session_id,
			       const struct model_usage *usage);

#endif
