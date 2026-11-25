#include <norm/query.h>
#include "persistence/orm/fcgi_store.h"
#include "persistence/orm/fcgi_models.h"
#include "persistence/orm/internal.h"
#include <stddef.h>

/* Existing SQLite text values have no application length bound. */
static const struct norm_field fcgi_actions_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_actions_row, id),
		.size = sizeof(((struct orm_fcgi_actions_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_actions_row, session_id),
		.size = sizeof(((struct orm_fcgi_actions_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("type"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_actions_row, type),
		.size = sizeof(((struct orm_fcgi_actions_row *)0)->type),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("payload"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_actions_row, payload),
		.size = sizeof(((struct orm_fcgi_actions_row *)0)->payload),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("ts"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_actions_row, ts),
		.size = sizeof(((struct orm_fcgi_actions_row *)0)->ts),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("consumed"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_actions_row, consumed),
		.size = sizeof(((struct orm_fcgi_actions_row *)0)->consumed),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
};

static const size_t idx_actions_sess_fields[] = {1, 5, 0};
static const struct norm_index fcgi_actions_indexes[] = {
	{.name = NB_STR_LIT("idx_actions_sess"),
	 .fields = idx_actions_sess_fields,
	 .field_count = 3,
	 .unique = false},
};

const struct norm_model orm_fcgi_actions = {
	.table = NB_STR_LIT("fcgi_actions"),
	.size = sizeof(struct orm_fcgi_actions_row),
	.alignment = _Alignof(struct orm_fcgi_actions_row),
	.fields = fcgi_actions_fields,
	.field_count = sizeof(fcgi_actions_fields) / sizeof(fcgi_actions_fields[0]),
	.indexes = fcgi_actions_indexes,
	.index_count = sizeof(fcgi_actions_indexes) / sizeof(fcgi_actions_indexes[0]),
};

static const struct norm_field fcgi_artifacts_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_artifacts_row, id),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("user_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_artifacts_row, user_id),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->user_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_artifacts_row, session_id),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_artifacts_row, kind),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("mime"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_artifacts_row, mime),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->mime),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("filename"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_artifacts_row, filename),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->filename),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("relative_path"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_artifacts_row, relative_path),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->relative_path),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("size_bytes"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_artifacts_row, size_bytes),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->size_bytes),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("status"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_artifacts_row, status),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->status),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("'ready'"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_artifacts_row, created_at),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("expires_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_artifacts_row, expires_at),
		.size = sizeof(((struct orm_fcgi_artifacts_row *)0)->expires_at),
		.null_offset = offsetof(struct orm_fcgi_artifacts_row, missing_expires_at),
		.flags = NORM_FIELD_NULLABLE,
	},
};

static const size_t idx_artifacts_user_session_fields[] = {1, 2};
static const struct norm_index fcgi_artifacts_indexes[] = {
	{.name = NB_STR_LIT("idx_artifacts_user_session"),
	 .fields = idx_artifacts_user_session_fields,
	 .field_count = 2,
	 .unique = false},
};

const struct norm_model orm_fcgi_artifacts = {
	.table = NB_STR_LIT("fcgi_artifacts"),
	.size = sizeof(struct orm_fcgi_artifacts_row),
	.alignment = _Alignof(struct orm_fcgi_artifacts_row),
	.fields = fcgi_artifacts_fields,
	.field_count = sizeof(fcgi_artifacts_fields) / sizeof(fcgi_artifacts_fields[0]),
	.indexes = fcgi_artifacts_indexes,
	.index_count = sizeof(fcgi_artifacts_indexes) / sizeof(fcgi_artifacts_indexes[0]),
};

static const struct norm_field fcgi_canvas_nodes_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, id),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, session_id),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("parent_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, parent_id),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->parent_id),
		.null_offset = offsetof(struct orm_fcgi_canvas_nodes_row, missing_parent_id),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("kind"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, kind),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->kind),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("x"),
		.type = NORM_TYPE_F64,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, x),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->x),
		.null_offset = offsetof(struct orm_fcgi_canvas_nodes_row, missing_x),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("y"),
		.type = NORM_TYPE_F64,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, y),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->y),
		.null_offset = offsetof(struct orm_fcgi_canvas_nodes_row, missing_y),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("w"),
		.type = NORM_TYPE_F64,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, w),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->w),
		.null_offset = offsetof(struct orm_fcgi_canvas_nodes_row, missing_w),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("h"),
		.type = NORM_TYPE_F64,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, h),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->h),
		.null_offset = offsetof(struct orm_fcgi_canvas_nodes_row, missing_h),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("content_ref"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, content_ref),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->content_ref),
		.null_offset = offsetof(struct orm_fcgi_canvas_nodes_row, missing_content_ref),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("meta"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, meta),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->meta),
		.null_offset = offsetof(struct orm_fcgi_canvas_nodes_row, missing_meta),
		.flags = NORM_FIELD_NULLABLE,
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, created_at),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_canvas_nodes_row, updated_at),
		.size = sizeof(((struct orm_fcgi_canvas_nodes_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t idx_canvas_sess_fields[] = {1, 11};
static const struct norm_index fcgi_canvas_nodes_indexes[] = {
	{.name = NB_STR_LIT("idx_canvas_sess"),
	 .fields = idx_canvas_sess_fields,
	 .field_count = 2,
	 .unique = false},
};

const struct norm_model orm_fcgi_canvas_nodes = {
	.table = NB_STR_LIT("fcgi_canvas_nodes"),
	.size = sizeof(struct orm_fcgi_canvas_nodes_row),
	.alignment = _Alignof(struct orm_fcgi_canvas_nodes_row),
	.fields = fcgi_canvas_nodes_fields,
	.field_count = sizeof(fcgi_canvas_nodes_fields) / sizeof(fcgi_canvas_nodes_fields[0]),
	.indexes = fcgi_canvas_nodes_indexes,
	.index_count = sizeof(fcgi_canvas_nodes_indexes) / sizeof(fcgi_canvas_nodes_indexes[0]),
};

static const struct norm_field fcgi_events_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_events_row, id),
		.size = sizeof(((struct orm_fcgi_events_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_events_row, session_id),
		.size = sizeof(((struct orm_fcgi_events_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("type"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_events_row, type),
		.size = sizeof(((struct orm_fcgi_events_row *)0)->type),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("payload"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_events_row, payload),
		.size = sizeof(((struct orm_fcgi_events_row *)0)->payload),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("ts"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_events_row, ts),
		.size = sizeof(((struct orm_fcgi_events_row *)0)->ts),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

static const size_t idx_events_sess_fields[] = {1, 0};
static const struct norm_index fcgi_events_indexes[] = {
	{.name = NB_STR_LIT("idx_events_sess"),
	 .fields = idx_events_sess_fields,
	 .field_count = 2,
	 .unique = false},
};

const struct norm_model orm_fcgi_events = {
	.table = NB_STR_LIT("fcgi_events"),
	.size = sizeof(struct orm_fcgi_events_row),
	.alignment = _Alignof(struct orm_fcgi_events_row),
	.fields = fcgi_events_fields,
	.field_count = sizeof(fcgi_events_fields) / sizeof(fcgi_events_fields[0]),
	.indexes = fcgi_events_indexes,
	.index_count = sizeof(fcgi_events_indexes) / sizeof(fcgi_events_indexes[0]),
};

static const struct norm_field fcgi_quota_profiles_fields[] = {
	{
		.column = NB_STR_LIT("name"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_quota_profiles_row, name),
		.size = sizeof(((struct orm_fcgi_quota_profiles_row *)0)->name),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("daily_turns"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_quota_profiles_row, daily_turns),
		.size = sizeof(((struct orm_fcgi_quota_profiles_row *)0)->daily_turns),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("100"),
	},
	{
		.column = NB_STR_LIT("daily_images"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_quota_profiles_row, daily_images),
		.size = sizeof(((struct orm_fcgi_quota_profiles_row *)0)->daily_images),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("20"),
	},
	{
		.column = NB_STR_LIT("daily_videos"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_quota_profiles_row, daily_videos),
		.size = sizeof(((struct orm_fcgi_quota_profiles_row *)0)->daily_videos),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("5"),
	},
	{
		.column = NB_STR_LIT("storage_bytes"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_quota_profiles_row, storage_bytes),
		.size = sizeof(((struct orm_fcgi_quota_profiles_row *)0)->storage_bytes),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("1073741824"),
	},
	{
		.column = NB_STR_LIT("max_concurrent_turns"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_quota_profiles_row, max_concurrent_turns),
		.size = sizeof(((struct orm_fcgi_quota_profiles_row *)0)->max_concurrent_turns),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("1"),
	},
};

const struct norm_model orm_fcgi_quota_profiles = {
	.table = NB_STR_LIT("fcgi_quota_profiles"),
	.size = sizeof(struct orm_fcgi_quota_profiles_row),
	.alignment = _Alignof(struct orm_fcgi_quota_profiles_row),
	.fields = fcgi_quota_profiles_fields,
	.field_count = sizeof(fcgi_quota_profiles_fields) / sizeof(fcgi_quota_profiles_fields[0]),
};

static const struct norm_field fcgi_running_turns_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_running_turns_row, id),
		.size = sizeof(((struct orm_fcgi_running_turns_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("user_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_running_turns_row, user_id),
		.size = sizeof(((struct orm_fcgi_running_turns_row *)0)->user_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_running_turns_row, session_id),
		.size = sizeof(((struct orm_fcgi_running_turns_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("started_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_running_turns_row, started_at),
		.size = sizeof(((struct orm_fcgi_running_turns_row *)0)->started_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("heartbeat_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_running_turns_row, heartbeat_at),
		.size = sizeof(((struct orm_fcgi_running_turns_row *)0)->heartbeat_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

const struct norm_model orm_fcgi_running_turns = {
	.table = NB_STR_LIT("fcgi_running_turns"),
	.size = sizeof(struct orm_fcgi_running_turns_row),
	.alignment = _Alignof(struct orm_fcgi_running_turns_row),
	.fields = fcgi_running_turns_fields,
	.field_count = sizeof(fcgi_running_turns_fields) / sizeof(fcgi_running_turns_fields[0]),
};

static const struct norm_field fcgi_session_owner_fields[] = {
	{
		.column = NB_STR_LIT("session_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_session_owner_row, session_id),
		.size = sizeof(((struct orm_fcgi_session_owner_row *)0)->session_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("user_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_session_owner_row, user_id),
		.size = sizeof(((struct orm_fcgi_session_owner_row *)0)->user_id),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

const struct norm_model orm_fcgi_session_owner = {
	.table = NB_STR_LIT("fcgi_session_owner"),
	.size = sizeof(struct orm_fcgi_session_owner_row),
	.alignment = _Alignof(struct orm_fcgi_session_owner_row),
	.fields = fcgi_session_owner_fields,
	.field_count = sizeof(fcgi_session_owner_fields) / sizeof(fcgi_session_owner_fields[0]),
};

static const struct norm_field fcgi_usage_daily_fields[] = {
	{
		.column = NB_STR_LIT("user_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_usage_daily_row, user_id),
		.size = sizeof(((struct orm_fcgi_usage_daily_row *)0)->user_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("day"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_usage_daily_row, day),
		.size = sizeof(((struct orm_fcgi_usage_daily_row *)0)->day),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("turns"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_usage_daily_row, turns),
		.size = sizeof(((struct orm_fcgi_usage_daily_row *)0)->turns),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("images"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_usage_daily_row, images),
		.size = sizeof(((struct orm_fcgi_usage_daily_row *)0)->images),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("videos"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_usage_daily_row, videos),
		.size = sizeof(((struct orm_fcgi_usage_daily_row *)0)->videos),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
};

const struct norm_model orm_fcgi_usage_daily = {
	.table = NB_STR_LIT("fcgi_usage_daily"),
	.size = sizeof(struct orm_fcgi_usage_daily_row),
	.alignment = _Alignof(struct orm_fcgi_usage_daily_row),
	.fields = fcgi_usage_daily_fields,
	.field_count = sizeof(fcgi_usage_daily_fields) / sizeof(fcgi_usage_daily_fields[0]),
};

static const struct norm_field fcgi_usage_storage_fields[] = {
	{
		.column = NB_STR_LIT("user_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_usage_storage_row, user_id),
		.size = sizeof(((struct orm_fcgi_usage_storage_row *)0)->user_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("used_bytes"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_usage_storage_row, used_bytes),
		.size = sizeof(((struct orm_fcgi_usage_storage_row *)0)->used_bytes),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
};

const struct norm_model orm_fcgi_usage_storage = {
	.table = NB_STR_LIT("fcgi_usage_storage"),
	.size = sizeof(struct orm_fcgi_usage_storage_row),
	.alignment = _Alignof(struct orm_fcgi_usage_storage_row),
	.fields = fcgi_usage_storage_fields,
	.field_count = sizeof(fcgi_usage_storage_fields) / sizeof(fcgi_usage_storage_fields[0]),
};

static const struct norm_field fcgi_user_quota_fields[] = {
	{
		.column = NB_STR_LIT("user_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_user_quota_row, user_id),
		.size = sizeof(((struct orm_fcgi_user_quota_row *)0)->user_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY,
	},
	{
		.column = NB_STR_LIT("profile"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_user_quota_row, profile),
		.size = sizeof(((struct orm_fcgi_user_quota_row *)0)->profile),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("'free'"),
	},
};

const struct norm_model orm_fcgi_user_quota = {
	.table = NB_STR_LIT("fcgi_user_quota"),
	.size = sizeof(struct orm_fcgi_user_quota_row),
	.alignment = _Alignof(struct orm_fcgi_user_quota_row),
	.fields = fcgi_user_quota_fields,
	.field_count = sizeof(fcgi_user_quota_fields) / sizeof(fcgi_user_quota_fields[0]),
};

static const struct norm_field fcgi_users_fields[] = {
	{
		.column = NB_STR_LIT("id"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_users_row, id),
		.size = sizeof(((struct orm_fcgi_users_row *)0)->id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED,
	},
	{
		.column = NB_STR_LIT("user_id"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_users_row, user_id),
		.size = sizeof(((struct orm_fcgi_users_row *)0)->user_id),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_UNIQUE,
	},
	{
		.column = NB_STR_LIT("username"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_users_row, username),
		.size = sizeof(((struct orm_fcgi_users_row *)0)->username),
		.null_offset = SIZE_MAX,
		.flags = NORM_FIELD_UNIQUE,
	},
	{
		.column = NB_STR_LIT("password_hash"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_users_row, password_hash),
		.size = sizeof(((struct orm_fcgi_users_row *)0)->password_hash),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("role"),
		.type = NORM_TYPE_TEXT,
		.max_length = SIZE_MAX,
		.offset = offsetof(struct orm_fcgi_users_row, role),
		.size = sizeof(((struct orm_fcgi_users_row *)0)->role),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("'user'"),
	},
	{
		.column = NB_STR_LIT("disabled"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_users_row, disabled),
		.size = sizeof(((struct orm_fcgi_users_row *)0)->disabled),
		.null_offset = SIZE_MAX,
		.flags = 0,
		.default_sql = NB_STR_LIT("0"),
	},
	{
		.column = NB_STR_LIT("created_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_users_row, created_at),
		.size = sizeof(((struct orm_fcgi_users_row *)0)->created_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
	{
		.column = NB_STR_LIT("updated_at"),
		.type = NORM_TYPE_I64,
		.offset = offsetof(struct orm_fcgi_users_row, updated_at),
		.size = sizeof(((struct orm_fcgi_users_row *)0)->updated_at),
		.null_offset = SIZE_MAX,
		.flags = 0,
	},
};

const struct norm_model orm_fcgi_users = {
	.table = NB_STR_LIT("fcgi_users"),
	.size = sizeof(struct orm_fcgi_users_row),
	.alignment = _Alignof(struct orm_fcgi_users_row),
	.fields = fcgi_users_fields,
	.field_count = sizeof(fcgi_users_fields) / sizeof(fcgi_users_fields[0]),
};

int orm_fcgi_schema(struct db *db)
{
	static const struct norm_model *const models[] = {
		&orm_fcgi_actions,	 &orm_fcgi_artifacts,	   &orm_fcgi_canvas_nodes,
		&orm_fcgi_events,	 &orm_fcgi_quota_profiles, &orm_fcgi_running_turns,
		&orm_fcgi_session_owner, &orm_fcgi_usage_daily,	   &orm_fcgi_usage_storage,
		&orm_fcgi_user_quota,	 &orm_fcgi_users,
	};
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	const struct norm_schema_options options = {.if_not_exists = true};
	int rc = orm_scope_begin(db, &scope);

	for (size_t i = 0; rc == 0 && i < sizeof(models) / sizeof(models[0]); i++) {
		rc = orm_status(db,
				norm_schema_create(scope.connection, models[i], &options,
						   orm_scope_call(&scope), &error),
				&error);
	}

	const struct orm_fcgi_quota_profiles_row defaults[] = {
		{.name = NB_STR_LIT("admin"),
		 .daily_turns = -1,
		 .daily_images = -1,
		 .daily_videos = -1,
		 .storage_bytes = -1,
		 .max_concurrent_turns = 4},
		{.name = NB_STR_LIT("free"),
		 .daily_turns = 50,
		 .daily_images = 10,
		 .daily_videos = 2,
		 .storage_bytes = 1073741824,
		 .max_concurrent_turns = 1},
	};
	const struct norm_conflict conflict = {.action = NORM_CONFLICT_NOTHING};
	const struct norm_insert_options insert = {.conflict = &conflict};

	if (rc == 0)
		rc = orm_status(db,
				norm_insert_many(scope.connection, &orm_fcgi_quota_profiles,
						 defaults, NB_ARRAY_SIZE(defaults),
						 sizeof(defaults[0]), &insert,
						 orm_scope_call(&scope), NULL, &error),
				&error);

	return rc;
}
