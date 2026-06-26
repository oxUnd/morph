#include "credits.h"
#include "util/error.h"
#include <errno.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>

static const char *credit_schema_sql =
	"CREATE TABLE IF NOT EXISTS credit_events ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"user_id TEXT,"
	"session_id TEXT,"
	"kind TEXT NOT NULL,"
	"provider TEXT,"
	"model TEXT,"
	"input_tokens INTEGER NOT NULL DEFAULT 0,"
	"output_tokens INTEGER NOT NULL DEFAULT 0,"
	"image_units INTEGER NOT NULL DEFAULT 0,"
	"video_seconds INTEGER NOT NULL DEFAULT 0,"
	"estimated_cost REAL NOT NULL DEFAULT 0,"
	"currency TEXT NOT NULL DEFAULT 'USD',"
	"credits INTEGER NOT NULL DEFAULT 0,"
	"metadata_json TEXT,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_credit_events_user_day "
	"ON credit_events(user_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_credit_events_session "
	"ON credit_events(session_id, created_at);";

int credit_init_schema(struct db *db)
{
	if (!db || !db->handle)
		MORPH_RETURN(-EINVAL);
	return db_exec(db, credit_schema_sql);
}

static const struct config_credit_price *
credit_find_price(const struct config_credits *cfg,
		  const struct credit_event *event)
{
	if (!cfg || !event || !event->provider || !event->model)
		return NULL;
	for (int i = 0; i < cfg->price_count; i++) {
		const struct config_credit_price *p = &cfg->prices[i];
		if (p->provider[0] && strcmp(p->provider, event->provider) != 0)
			continue;
		if (p->model[0] && strcmp(p->model, event->model) != 0)
			continue;
		if (p->kind[0] && event->kind &&
		    strcmp(p->kind, event->kind) != 0)
			continue;
		return p;
	}
	return NULL;
}

static int64_t credit_ceil_nonnegative(double value)
{
	int64_t whole;

	if (value <= 0.0)
		return 0;
	whole = (int64_t)value;
	if (value > (double)whole)
		whole++;
	return whole;
}

int credit_calculate(const struct config_credits *cfg,
		     const struct credit_event *event,
		     struct credit_charge *out)
{
	const struct config_credit_price *price;
	double cost = 0.0;
	double direct = 0.0;
	double credits = 0.0;

	if (!cfg || !event || !out)
		MORPH_RETURN(-EINVAL);

	memset(out, 0, sizeof(*out));
	price = credit_find_price(cfg, event);
	if (price) {
		cost += ((double)event->input_tokens *
			 price->input_per_million) / 1000000.0;
		cost += ((double)event->output_tokens *
			 price->output_per_million) / 1000000.0;
		cost += ((double)event->image_units *
			 price->image_unit_per_million) / 1000000.0;
		cost += ((double)event->video_seconds *
			 price->video_second_per_million) / 1000000.0;
		out->price_configured = 1;
	}

	direct += (double)event->input_tokens *
		cfg->input_token_credit_coef;
	direct += (double)event->output_tokens *
		cfg->output_token_credit_coef;
	direct += (double)event->image_units *
		cfg->image_unit_credit_coef;
	direct += (double)event->video_seconds *
		cfg->video_second_credit_coef;

	credits = cost > 0.0 ? cost * cfg->cost_to_credit_coef : direct;
	out->estimated_cost = cost;
	out->credits = credit_ceil_nonnegative(credits);
	return 0;
}

int credit_record_event(struct db *db, const struct config_credits *cfg,
			const struct credit_event *event,
			struct credit_charge *out)
{
	const char *sql =
		"INSERT INTO credit_events"
		"(user_id,session_id,kind,provider,model,input_tokens,"
		"output_tokens,image_units,video_seconds,estimated_cost,"
		"currency,credits,metadata_json,created_at)"
		" VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
	sqlite3_stmt *stmt = NULL;
	struct credit_charge charge;
	int rc;

	if (!db || !db->handle || !cfg || !event || !event->kind)
		MORPH_RETURN(-EINVAL);

	rc = credit_calculate(cfg, event, &charge);
	if (rc != 0)
		return rc;

	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, event->user_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, event->session_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 3, event->kind, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 4, event->provider, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 5, event->model, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 6, event->input_tokens);
	sqlite3_bind_int64(stmt, 7, event->output_tokens);
	sqlite3_bind_int64(stmt, 8, event->image_units);
	sqlite3_bind_int64(stmt, 9, event->video_seconds);
	sqlite3_bind_double(stmt, 10, charge.estimated_cost);
	sqlite3_bind_text(stmt, 11, cfg->currency, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 12, charge.credits);
	sqlite3_bind_text(stmt, 13, event->metadata_json, -1,
			  SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 14, (int64_t)time(NULL));
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	if (out)
		*out = charge;
	return 0;
}

int credit_summary_today(struct db *db, const char *user_id,
			 struct credit_summary *out)
{
	const char *sql =
		"SELECT COALESCE(SUM(credits),0),"
		"COALESCE(SUM(estimated_cost),0),COUNT(*) "
		"FROM credit_events WHERE user_id=? "
		"AND created_at >= strftime('%s','now','start of day')";
	sqlite3_stmt *stmt = NULL;

	if (!db || !db->handle || !user_id || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, user_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		out->credits = sqlite3_column_int64(stmt, 0);
		out->estimated_cost = sqlite3_column_double(stmt, 1);
		out->event_count = sqlite3_column_int(stmt, 2);
	}
	sqlite3_finalize(stmt);
	return 0;
}

int credit_summary_session(struct db *db, const char *session_id,
			   struct credit_summary *out)
{
	const char *sql =
		"SELECT COALESCE(SUM(credits),0),"
		"COALESCE(SUM(estimated_cost),0),COUNT(*) "
		"FROM credit_events WHERE session_id=?";
	sqlite3_stmt *stmt = NULL;

	if (!db || !db->handle || !session_id || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		out->credits = sqlite3_column_int64(stmt, 0);
		out->estimated_cost = sqlite3_column_double(stmt, 1);
		out->event_count = sqlite3_column_int(stmt, 2);
	}
	sqlite3_finalize(stmt);
	return 0;
}

int credit_image_units_from_size(int width, int height)
{
	int64_t pixels;

	if (width <= 0 || height <= 0)
		return 0;
	pixels = (int64_t)width * (int64_t)height;
	return (int)((pixels + 999999) / 1000000);
}
