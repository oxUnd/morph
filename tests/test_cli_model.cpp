#include "runtime_test_support.hpp"

extern "C" {
#include "runtime/runtime_internal.h"
#include "runtime/model.h"
#include "sapi/cli/models/composer.h"
#include "util/cleanup.h"
#include "util/file.h"
}

#include <cstring>

TEST_F(RuntimeFacadeTest, ModelModeChangesPersistWithoutChangingProviderParameters)
{
	WriteConfig("[memory]\nenabled=false\n[model.text]\n"
		    "extra_body_json='''{\"temperature\":0.25,\"reasoning_effort\":\"high\"}'''\n");
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	options.enable_multi_agent = 1;
	Open(&options);
	int ultra = -1;
	ASSERT_EQ(runtime_model_get_ultra(instance, &ultra), 0);
	EXPECT_EQ(ultra, 0);
	const std::string before = instance->context.llm->extra_body_json;
	ASSERT_EQ(runtime_model_set_ultra(instance, 1), 0);
	ASSERT_EQ(runtime_model_get_ultra(instance, &ultra), 0);
	EXPECT_EQ(ultra, 1);
	EXPECT_EQ(before, instance->context.llm->extra_body_json);
	EXPECT_STREQ(instance->context.config.agents.multi_agent_mode, "proactive");
	morph_buf_t prompt{};
	ASSERT_EQ(morph_buf_init(&prompt, 1024), 0);
	ASSERT_EQ(agent_control_prompt(instance->context.agents, "/root", &prompt), 0);
	EXPECT_NE(strstr(morph_buf_cstr_borrow(&prompt), "Proactive multi-agent"), nullptr);
	morph_buf_cleanup(&prompt);
	struct config saved{};
	ASSERT_EQ(config_load(nullptr, &saved, config_path.c_str()), 0);
	EXPECT_STREQ(saved.agents.multi_agent_mode, "proactive");
	EXPECT_EQ(before, saved.models.text.extra_body_json);
	ASSERT_EQ(runtime_model_set_ultra(instance, 0), 0);
	ASSERT_EQ(runtime_model_get_ultra(instance, &ultra), 0);
	EXPECT_EQ(ultra, 0);
	ASSERT_EQ(config_load(nullptr, &saved, config_path.c_str()), 0);
	EXPECT_STREQ(saved.agents.multi_agent_mode, "explicit");
	EXPECT_EQ(before, instance->context.llm->extra_body_json);
}

TEST_F(RuntimeFacadeTest, ModelModeFailureAndBusyRuntimePreserveSelection)
{
	WriteConfig("[memory]\nenabled=false\n");
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	options.enable_multi_agent = 1;
	Open(&options);
	pthread_mutex_lock(&instance->context.execution_lock);
	int rc = runtime_model_set_ultra(instance, 1);
	pthread_mutex_unlock(&instance->context.execution_lock);
	EXPECT_EQ(rc, -EBUSY);
	std::filesystem::remove(config_path);
	std::filesystem::create_directory(config_path);
	EXPECT_LT(runtime_model_set_ultra(instance, 1), 0);
	int ultra = -1;
	ASSERT_EQ(runtime_model_get_ultra(instance, &ultra), 0);
	EXPECT_EQ(ultra, 0);
	EXPECT_STREQ(instance->context.config.agents.multi_agent_mode, "auto");
}

TEST_F(RuntimeFacadeTest, ModelModeLoadsConfiguredUltraWithoutChangingReasoning)
{
	WriteConfig("[memory]\nenabled=false\n[model.text]\n"
		    "extra_body_json='''{\"reasoning\":{\"effort\":\"ultra\"}}'''\n");
	struct runtime_options options{};
	options.config_path = config_path.c_str();
	options.enable_multi_agent = 1;
	Open(&options);
	int ultra = 0;
	ASSERT_EQ(runtime_model_get_ultra(instance, &ultra), 0);
	EXPECT_EQ(ultra, 1);
	ASSERT_EQ(runtime_model_set_ultra(instance, 0), 0);
	ASSERT_EQ(runtime_model_get_ultra(instance, &ultra), 0);
	EXPECT_EQ(ultra, 0);
}

TEST(CliModelTest, ComposerModeAnimationStartsOnFirstVisibleFrameAndEnds)
{
	struct cli_composer_model composer{};
	cli_composer_model_set_ultra(&composer, 0, 0);
	EXPECT_EQ(cli_composer_model_prompt_width(&composer), 2);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 100), -1);
	cli_composer_model_set_ultra(&composer, 1, 1);
	EXPECT_EQ(cli_composer_model_prompt_width(&composer), 2);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 1000), 0);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 1330), 10);
	cli_composer_model_set_ultra(&composer, 1, 1);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 1660), 20);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 1990), 30);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 2023), -1);
	cli_composer_model_set_ultra(&composer, 0, 1);
	EXPECT_EQ(cli_composer_model_prompt_width(&composer), 2);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 2056), -1);
}

TEST(CliModelTest, InitialUltraAndNoColorSelectionDoNotAnimate)
{
	struct cli_composer_model composer{};
	cli_composer_model_set_ultra(&composer, 1, 1);
	EXPECT_EQ(cli_composer_model_prompt_width(&composer), 2);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 1000), -1);
	cli_composer_model_set_ultra(&composer, 0, 0);
	cli_composer_model_set_ultra(&composer, 1, 0);
	EXPECT_EQ(cli_composer_model_animation_frame(&composer, 2000), -1);
}
