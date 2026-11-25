#ifndef MORPH_ORM_FCGI_MODELS_H
#define MORPH_ORM_FCGI_MODELS_H

#include "persistence/orm/database.h"
#include <norm/orm.h>

struct orm_fcgi_actions_row {
	int64_t id;
	nb_str session_id;
	nb_str type;
	nb_str payload;
	int64_t ts;
	int64_t consumed;
};
extern const struct norm_model orm_fcgi_actions;

struct orm_fcgi_artifacts_row {
	nb_str id;
	nb_str user_id;
	nb_str session_id;
	nb_str kind;
	nb_str mime;
	nb_str filename;
	nb_str relative_path;
	int64_t size_bytes;
	nb_str status;
	int64_t created_at;
	int64_t expires_at;
	bool missing_expires_at;
};
extern const struct norm_model orm_fcgi_artifacts;

struct orm_fcgi_canvas_nodes_row {
	nb_str id;
	nb_str session_id;
	nb_str parent_id;
	bool missing_parent_id;
	nb_str kind;
	double x;
	bool missing_x;
	double y;
	bool missing_y;
	double w;
	bool missing_w;
	double h;
	bool missing_h;
	nb_str content_ref;
	bool missing_content_ref;
	nb_str meta;
	bool missing_meta;
	int64_t created_at;
	int64_t updated_at;
};
extern const struct norm_model orm_fcgi_canvas_nodes;

struct orm_fcgi_events_row {
	int64_t id;
	nb_str session_id;
	nb_str type;
	nb_str payload;
	int64_t ts;
};
extern const struct norm_model orm_fcgi_events;

struct orm_fcgi_quota_profiles_row {
	nb_str name;
	int64_t daily_turns;
	int64_t daily_images;
	int64_t daily_videos;
	int64_t storage_bytes;
	int64_t max_concurrent_turns;
};
extern const struct norm_model orm_fcgi_quota_profiles;

struct orm_fcgi_running_turns_row {
	nb_str id;
	nb_str user_id;
	nb_str session_id;
	int64_t started_at;
	int64_t heartbeat_at;
};
extern const struct norm_model orm_fcgi_running_turns;

struct orm_fcgi_session_owner_row {
	nb_str session_id;
	nb_str user_id;
};
extern const struct norm_model orm_fcgi_session_owner;

struct orm_fcgi_usage_daily_row {
	nb_str user_id;
	nb_str day;
	int64_t turns;
	int64_t images;
	int64_t videos;
};
extern const struct norm_model orm_fcgi_usage_daily;

struct orm_fcgi_usage_storage_row {
	nb_str user_id;
	int64_t used_bytes;
};
extern const struct norm_model orm_fcgi_usage_storage;

struct orm_fcgi_user_quota_row {
	nb_str user_id;
	nb_str profile;
};
extern const struct norm_model orm_fcgi_user_quota;

struct orm_fcgi_users_row {
	int64_t id;
	nb_str user_id;
	nb_str username;
	nb_str password_hash;
	nb_str role;
	int64_t disabled;
	int64_t created_at;
	int64_t updated_at;
};
extern const struct norm_model orm_fcgi_users;

#endif
