#ifndef MORPH_ORM_FCGI_QUOTA_INTERNAL_H
#define MORPH_ORM_FCGI_QUOTA_INTERNAL_H

#include "persistence/orm/internal.h"

struct orm_fcgi_quota_view {
	nb_str profile;
	int64_t daily_turns;
	int64_t daily_images;
	int64_t daily_videos;
	int64_t storage_bytes;
	int64_t max_concurrent_turns;
	int64_t used_turns;
	bool missing_used_turns;
	int64_t used_images;
	bool missing_used_images;
	int64_t used_videos;
	bool missing_used_videos;
	int64_t used_bytes;
	bool missing_used_bytes;
};

int orm_fcgi_quota_copy(struct orm_scope *scope, const char *user_id, struct norm_rows **rows);

int orm_fcgi_usage_add(struct orm_scope *scope, const char *user_id, size_t counter,
		       int64_t amount);
int orm_fcgi_storage_add(struct orm_scope *scope, const char *user_id, int64_t amount);

#endif
