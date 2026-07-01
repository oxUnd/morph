#ifndef CREDIT_STORE_H
#define CREDIT_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "db/database.h"
#include <stdint.h>

struct credit_store_event {
	const char *user_id;
	const char *session_id;
	const char *kind;
	const char *provider;
	const char *model;
	int64_t input_tokens;
	int64_t output_tokens;
	int64_t image_units;
	int64_t video_seconds;
	double estimated_cost;
	const char *currency;
	int64_t credits;
	const char *metadata_json;
};

struct credit_store_summary {
	int64_t credits;
	double estimated_cost;
	int event_count;
};

int credit_store_init_schema(struct db *db);
int credit_store_record_event(struct db *db,
			      const struct credit_store_event *event);
int credit_store_summary_today(struct db *db, const char *user_id,
			       struct credit_store_summary *out);
int credit_store_summary_total(struct db *db, const char *user_id,
			       struct credit_store_summary *out);
int credit_store_summary_session(struct db *db, const char *session_id,
				 struct credit_store_summary *out);

#ifdef __cplusplus
}
#endif

#endif
