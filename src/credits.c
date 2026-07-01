#include "credits.h"
#include "persistence/credit_store.h"
#include "util/error.h"
#include <errno.h>
#include <string.h>

int credit_init_schema(struct db *db)
{
	return credit_store_init_schema(db);
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
	struct credit_charge charge;
	struct credit_store_event store_event;
	int rc;

	if (!db || !db->handle || !cfg || !event || !event->kind)
		MORPH_RETURN(-EINVAL);

	rc = credit_calculate(cfg, event, &charge);
	if (rc != 0)
		return rc;

	memset(&store_event, 0, sizeof(store_event));
	store_event.user_id = event->user_id;
	store_event.session_id = event->session_id;
	store_event.kind = event->kind;
	store_event.provider = event->provider;
	store_event.model = event->model;
	store_event.input_tokens = event->input_tokens;
	store_event.output_tokens = event->output_tokens;
	store_event.image_units = event->image_units;
	store_event.video_seconds = event->video_seconds;
	store_event.estimated_cost = charge.estimated_cost;
	store_event.currency = cfg->currency;
	store_event.credits = charge.credits;
	store_event.metadata_json = event->metadata_json;
	rc = credit_store_record_event(db, &store_event);
	if (rc != 0)
		return rc;
	if (out)
		*out = charge;
	return 0;
}

static void credit_summary_from_store(
	struct credit_summary *out, const struct credit_store_summary *in)
{
	out->credits = in->credits;
	out->estimated_cost = in->estimated_cost;
	out->event_count = in->event_count;
}

int credit_summary_today(struct db *db, const char *user_id,
			 struct credit_summary *out)
{
	struct credit_store_summary summary;
	int rc;

	if (!db || !db->handle || !user_id || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	rc = credit_store_summary_today(db, user_id, &summary);
	if (rc != 0)
		return rc;
	credit_summary_from_store(out, &summary);
	return 0;
}

int credit_summary_total(struct db *db, const char *user_id,
			 struct credit_summary *out)
{
	struct credit_store_summary summary;
	int rc;

	if (!db || !db->handle || !user_id || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	rc = credit_store_summary_total(db, user_id, &summary);
	if (rc != 0)
		return rc;
	credit_summary_from_store(out, &summary);
	return 0;
}

int credit_summary_session(struct db *db, const char *session_id,
			   struct credit_summary *out)
{
	struct credit_store_summary summary;
	int rc;

	if (!db || !db->handle || !session_id || !out)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	rc = credit_store_summary_session(db, session_id, &summary);
	if (rc != 0)
		return rc;
	credit_summary_from_store(out, &summary);
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
