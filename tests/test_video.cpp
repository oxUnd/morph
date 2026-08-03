#include <gtest/gtest.h>

extern "C" {
#include "agent/tool.h"
#include "agent/tools/vid_gen.h"
#include "cJSON.h"
#include "models/llm.h"
#include "models/video_gen.h"
#include "models/video_provider.h"
#include "util/error.h"
}

#include <cerrno>
#include <cstdio>
#include <cstring>

static struct model make_video_model(const char *provider,
				     const char *adapter,
				     const char *model_id)
{
	struct model model{};

	std::snprintf(model.provider, sizeof(model.provider), "%s", provider);
	std::snprintf(model.adapter, sizeof(model.adapter), "%s", adapter);
	std::snprintf(model.model_id, sizeof(model.model_id), "%s", model_id);
	return model;
}

TEST(VideoAdapterTest, ResolvesExplicitAndInferredAdapter)
{
	struct model inferred = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-260128");
	struct model explicit_adapter = make_video_model(
		"gateway", "volcengine-videos", "doubao-seedance-2-0-260128");
	struct model unsupported = make_video_model(
		"gateway", "unknown-videos", "doubao-seedance-2-0-260128");

	EXPECT_STREQ(video_gen_adapter_name(&inferred), "volcengine-videos");
	EXPECT_STREQ(video_gen_adapter_name(&explicit_adapter),
		     "volcengine-videos");
	EXPECT_EQ(video_gen_adapter_name(&unsupported), nullptr);
	EXPECT_TRUE(video_gen_adapter_supported("volcengine", ""));
	EXPECT_TRUE(video_gen_adapter_supported("gateway", "volcengine-videos"));
	EXPECT_FALSE(video_gen_adapter_supported("gateway", ""));
}

TEST(VideoAdapterTest, CapabilitiesFollowSeedanceVersion)
{
	struct model one = make_video_model(
		"volcengine", "", "doubao-seedance-1-0-pro-fast-251015");
	struct model one_five = make_video_model(
		"volcengine", "", "doubao-seedance-1-5-pro-251215");
	struct model two = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-260128");
	struct model fast = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-fast-260128");
	struct model endpoint = make_video_model("volcengine", "", "ep-test");
	struct video_capabilities caps{};

	ASSERT_EQ(video_gen_capabilities(&one, &caps), 0);
	EXPECT_TRUE(caps.supports_reference_images);
	EXPECT_FALSE(caps.supports_multi_reference_images);
	EXPECT_FALSE(caps.supports_generate_audio);
	EXPECT_FALSE(caps.supports_reference_videos);
	EXPECT_FALSE(caps.supports_reference_audios);

	ASSERT_EQ(video_gen_capabilities(&one_five, &caps), 0);
	EXPECT_TRUE(caps.supports_generate_audio);
	EXPECT_FALSE(caps.supports_reference_videos);

	ASSERT_EQ(video_gen_capabilities(&two, &caps), 0);
	EXPECT_TRUE(caps.supports_multi_reference_images);
	EXPECT_TRUE(caps.supports_generate_audio);
	EXPECT_TRUE(caps.supports_reference_videos);
	EXPECT_TRUE(caps.supports_reference_audios);

	ASSERT_EQ(video_gen_capabilities(&fast, &caps), 0);
	EXPECT_TRUE(caps.supports_reference_videos);
	EXPECT_TRUE(caps.supports_reference_audios);

	ASSERT_EQ(video_gen_capabilities(&endpoint, &caps), 0);
	EXPECT_TRUE(caps.supports_reference_images);
	EXPECT_FALSE(caps.supports_multi_reference_images);
	EXPECT_FALSE(caps.supports_reference_videos);
	EXPECT_FALSE(caps.supports_reference_audios);
}

TEST(VideoAdapterTest, EmptyModelDoesNotUseHiddenFallback)
{
	struct model model = make_video_model("volcengine", "", "");
	struct video_capabilities caps{};
	struct video_result result{};

	EXPECT_EQ(video_gen_capabilities(&model, &caps),
		  MORPH_ERR_NOT_CONFIGURED);
	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, nullptr, 0,
				   nullptr, 0, -1, 5, nullptr, &result),
		  MORPH_ERR_NOT_CONFIGURED);
	EXPECT_NE(std::strstr(result.error_msg, "no model configured"), nullptr);
}

TEST(VideoAdapterTest, RejectsUnsupportedReferencesBeforeNetworkRequest)
{
	struct model model = make_video_model(
		"volcengine", "", "doubao-seedance-1-5-pro-251215");
	struct video_result result{};
	const char *videos[] = {"reference.mp4"};

	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, videos, 1,
				   nullptr, 0, -1, 5, nullptr, &result),
		  -ENOTSUP);
	EXPECT_NE(std::strstr(result.error_msg, "reference videos"), nullptr);
}

TEST(VideoAdapterTest, ValidatesReferenceAudioCombination)
{
	struct model model = make_video_model(
		"volcengine", "", "doubao-seedance-2-0-260128");
	struct video_result result{};
	const char *images[] = {"reference.png"};
	const char *audios[] = {"reference.mp3"};

	EXPECT_EQ(video_gen_create(&model, "test", nullptr, 0, nullptr, 0,
				   audios, 1, -1, 5, nullptr, &result),
		  -EINVAL);
	EXPECT_NE(std::strstr(result.error_msg, "requires a reference image"),
		  nullptr);

	memset(&result, 0, sizeof(result));
	EXPECT_EQ(video_gen_create(&model, "test", images, 1, nullptr, 0,
				   audios, 1, 0, 5, nullptr, &result),
		  -EINVAL);
	EXPECT_NE(std::strstr(result.error_msg, "generate_audio=true"), nullptr);
}

TEST(VideoToolTest, SchemaExposesAudioInputs)
{
	struct tool_registry registry;

	tool_registry_init(&registry);
	ASSERT_EQ(vid_gen_init(&registry, nullptr, nullptr), 0);
	struct tool_entry *entry = tool_lookup(&registry, "vid_gen");
	ASSERT_NE(entry, nullptr);
	EXPECT_NE(std::strstr(entry->desc.input_schema, "reference_audios"),
		  nullptr);
	EXPECT_NE(std::strstr(entry->desc.input_schema, "generate_audio"),
		  nullptr);
	EXPECT_NE(std::strstr(entry->desc.input_schema, "\"maxItems\":3"),
		  nullptr);
	cJSON *schema = cJSON_Parse(entry->desc.input_schema);
	EXPECT_NE(schema, nullptr);
	cJSON_Delete(schema);
	tool_registry_cleanup(&registry);
}
