#include <gtest/gtest.h>
#include "credits.h"
#include "db/database.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

class CreditsTest : public ::testing::Test {
protected:
	struct db db;
	char db_path[256];

	void SetUp() override {
		memset(&db, 0, sizeof(db));
		snprintf(db_path, sizeof(db_path), "/tmp/morph_credits_%d.db",
			 getpid());
		std::remove(db_path);
	}

	void TearDown() override {
		db_close(&db);
		std::remove(db_path);
	}
};

TEST_F(CreditsTest, CalculatesFromConfiguredPrice) {
	struct config cfg;
	struct credit_event event;
	struct credit_charge charge;

	config_set_defaults(&cfg);
	cfg.credits.cost_to_credit_coef = 1000.0;
	cfg.credits.price_count = 1;
	snprintf(cfg.credits.prices[0].provider,
		 sizeof(cfg.credits.prices[0].provider), "openai");
	snprintf(cfg.credits.prices[0].model,
		 sizeof(cfg.credits.prices[0].model), "gpt-test");
	snprintf(cfg.credits.prices[0].kind,
		 sizeof(cfg.credits.prices[0].kind), "chat_text");
	cfg.credits.prices[0].input_per_million = 2.0;
	cfg.credits.prices[0].output_per_million = 10.0;

	memset(&event, 0, sizeof(event));
	event.kind = "chat_text";
	event.provider = "openai";
	event.model = "gpt-test";
	event.input_tokens = 1000000;
	event.output_tokens = 100000;

	ASSERT_EQ(credit_calculate(&cfg.credits, &event, &charge), 0);
	EXPECT_DOUBLE_EQ(charge.estimated_cost, 3.0);
	EXPECT_EQ(charge.credits, 3000);
	EXPECT_EQ(charge.price_configured, 1);
}

TEST_F(CreditsTest, FallsBackToDirectCoefficients) {
	struct config cfg;
	struct credit_event event;
	struct credit_charge charge;

	config_set_defaults(&cfg);
	cfg.credits.input_token_credit_coef = 0.01;
	cfg.credits.output_token_credit_coef = 0.02;

	memset(&event, 0, sizeof(event));
	event.kind = "chat_text";
	event.provider = "missing";
	event.model = "missing";
	event.input_tokens = 10;
	event.output_tokens = 10;

	ASSERT_EQ(credit_calculate(&cfg.credits, &event, &charge), 0);
	EXPECT_DOUBLE_EQ(charge.estimated_cost, 0.0);
	EXPECT_EQ(charge.credits, 1);
	EXPECT_EQ(charge.price_configured, 0);
}

TEST_F(CreditsTest, CalculatesVolcengineImagePerImagePrice) {
	struct config cfg;
	struct credit_event event;
	struct credit_charge charge;

	config_set_defaults(&cfg);
	snprintf(cfg.credits.currency, sizeof(cfg.credits.currency), "CNY");
	cfg.credits.cost_to_credit_coef = 1000.0;
	cfg.credits.price_count = 1;
	snprintf(cfg.credits.prices[0].provider,
		 sizeof(cfg.credits.prices[0].provider), "volcengine");
	snprintf(cfg.credits.prices[0].model,
		 sizeof(cfg.credits.prices[0].model),
		 "doubao-seedream-5-0-lite");
	snprintf(cfg.credits.prices[0].kind,
		 sizeof(cfg.credits.prices[0].kind), "image_gen");
	cfg.credits.prices[0].image_unit_per_million = 220000.0;

	memset(&event, 0, sizeof(event));
	event.kind = "image_gen";
	event.provider = "volcengine";
	event.model = "doubao-seedream-5-0-lite";
	event.image_units = 1;

	ASSERT_EQ(credit_calculate(&cfg.credits, &event, &charge), 0);
	EXPECT_DOUBLE_EQ(charge.estimated_cost, 0.22);
	EXPECT_EQ(charge.credits, 220);
	EXPECT_EQ(charge.price_configured, 1);
}

TEST_F(CreditsTest, CalculatesDoubaoSeedLiteBasePriceCeiling) {
	struct config cfg;
	struct credit_event event;
	struct credit_charge charge;

	config_set_defaults(&cfg);
	snprintf(cfg.credits.currency, sizeof(cfg.credits.currency), "CNY");
	cfg.credits.cost_to_credit_coef = 1000.0;
	cfg.credits.price_count = 1;
	snprintf(cfg.credits.prices[0].provider,
		 sizeof(cfg.credits.prices[0].provider), "volcengine");
	snprintf(cfg.credits.prices[0].model,
		 sizeof(cfg.credits.prices[0].model),
		 "doubao-seed-2-0-lite");
	snprintf(cfg.credits.prices[0].kind,
		 sizeof(cfg.credits.prices[0].kind), "chat_text");
	cfg.credits.prices[0].input_per_million = 1.8;
	cfg.credits.prices[0].output_per_million = 10.8;

	memset(&event, 0, sizeof(event));
	event.kind = "chat_text";
	event.provider = "volcengine";
	event.model = "doubao-seed-2-0-lite";
	event.input_tokens = 1000;
	event.output_tokens = 1000;

	ASSERT_EQ(credit_calculate(&cfg.credits, &event, &charge), 0);
	EXPECT_DOUBLE_EQ(charge.estimated_cost, 0.0126);
	EXPECT_EQ(charge.credits, 13);
	EXPECT_EQ(charge.price_configured, 1);
}

TEST_F(CreditsTest, CalculatesVolcengineVideoTokenPrice) {
	struct config cfg;
	struct credit_event event;
	struct credit_charge charge;

	config_set_defaults(&cfg);
	snprintf(cfg.credits.currency, sizeof(cfg.credits.currency), "CNY");
	cfg.credits.cost_to_credit_coef = 1000.0;
	cfg.credits.price_count = 1;
	snprintf(cfg.credits.prices[0].provider,
		 sizeof(cfg.credits.prices[0].provider), "volcengine");
	snprintf(cfg.credits.prices[0].model,
		 sizeof(cfg.credits.prices[0].model),
		 "doubao-seedance-1-0-pro-fast-251015");
	snprintf(cfg.credits.prices[0].kind,
		 sizeof(cfg.credits.prices[0].kind), "video_gen");
	cfg.credits.prices[0].input_per_million = 4.2;
	cfg.credits.prices[0].output_per_million = 2.1;

	memset(&event, 0, sizeof(event));
	event.kind = "video_gen";
	event.provider = "volcengine";
	event.model = "doubao-seedance-1-0-pro-fast-251015";
	event.input_tokens = 1000;
	event.output_tokens = 1000;

	ASSERT_EQ(credit_calculate(&cfg.credits, &event, &charge), 0);
	EXPECT_DOUBLE_EQ(charge.estimated_cost, 0.0063);
	EXPECT_EQ(charge.credits, 7);
	EXPECT_EQ(charge.price_configured, 1);
}

TEST_F(CreditsTest, RecordsAndSummarizesEvents) {
	struct config cfg;
	struct credit_event event;
	struct credit_summary today;
	struct credit_summary session;

	config_set_defaults(&cfg);
	cfg.credits.input_token_credit_coef = 1.0;

	ASSERT_EQ(db_open(&db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);

	memset(&event, 0, sizeof(event));
	event.user_id = "local";
	event.session_id = "sess";
	event.kind = "chat_text";
	event.provider = "openai";
	event.model = "gpt-test";
	event.input_tokens = 7;

	ASSERT_EQ(credit_record_event(&db, &cfg.credits, &event, NULL), 0);
	ASSERT_EQ(credit_summary_today(&db, "local", &today), 0);
	ASSERT_EQ(credit_summary_session(&db, "sess", &session), 0);
	EXPECT_EQ(today.credits, 7);
	EXPECT_EQ(today.event_count, 1);
	EXPECT_EQ(session.credits, 7);
	EXPECT_EQ(session.event_count, 1);
}

TEST(CreditImageUnits, MegapixelCeil) {
	EXPECT_EQ(credit_image_units_from_size(1, 1), 1);
	EXPECT_EQ(credit_image_units_from_size(1000, 1000), 1);
	EXPECT_EQ(credit_image_units_from_size(1001, 1000), 2);
	EXPECT_EQ(credit_image_units_from_size(0, 1000), 0);
}
