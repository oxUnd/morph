#ifndef CREDITS_H
#define CREDITS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "db/database.h"
#include <stdint.h>

struct credit_event {
	const char *user_id;
	const char *session_id;
	const char *kind;
	const char *provider;
	const char *model;
	int64_t input_tokens;
	int64_t cached_tokens;
	int64_t output_tokens;
	int64_t image_units;
	int64_t video_seconds;
	const char *metadata_json;
};

struct credit_charge {
	double estimated_cost;
	int64_t credits;
	int price_configured;
};

struct credit_summary {
	int64_t credits;
	double estimated_cost;
	int event_count;
};

int credit_init_schema(struct db *db);
int credit_calculate(const struct config_credits *cfg,
		     const struct credit_event *event,
		     struct credit_charge *out);
int credit_record_event(struct db *db, const struct config_credits *cfg,
			const struct credit_event *event,
			struct credit_charge *out);
int credit_summary_today(struct db *db, const char *user_id,
			 struct credit_summary *out);
int credit_summary_total(struct db *db, const char *user_id,
			 struct credit_summary *out);
int credit_summary_session(struct db *db, const char *session_id,
			   struct credit_summary *out);
int credit_image_units_from_size(int width, int height);

#ifdef __cplusplus
}
#endif

#endif
