#include <gtest/gtest.h>
#include "models/image_gen.h"
#include "models/media_output.h"
#include "agent/tools/annotation.h"
#include <filesystem>
#include <string>
#include <set>
#include <thread>
#include <vector>
#include "models/image_provider.h"
#include "models/llm.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_qa.h"
#include "agent/tools/img_inpaint.h"
#include "agent/tools/img_compose.h"
#include "agent/tools/img_info.h"
#include "agent/tools/img_resize.h"
#include "agent/tools/image_input.h"
#include "agent/tools/img_convert.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tool_runtime.h"
#include "event/event.h"
#include "render/image.h"
#include "nb/file.h"
#include "errors.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "media/image.h"
#include "encoding_fixture.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

struct FakeImageQaState {
	char prompt[256]{};
	char path[512]{};
	int max_tokens{};
	long timeout_seconds{};
	int max_dim{};
	const struct nb_cancel *cancel_token{};
};

static int fake_chat_with_image(struct model *self, const char *system_prompt, const char *prompt,
				const char *image_path, const struct model_image_chat_options *opts,
				sse_callback cb, void *user_data)
{
	auto &qa = *static_cast<FakeImageQaState *>(self->handle);
	qa.cancel_token = self->http.cancel_token;
	(void)system_prompt;
	snprintf(qa.prompt, sizeof(qa.prompt), "%s", prompt ? prompt : "");
	snprintf(qa.path, sizeof(qa.path), "%s", image_path ? image_path : "");
	qa.max_tokens = opts ? opts->max_tokens : 0;
	qa.timeout_seconds = opts ? opts->timeout_seconds : 0;
	qa.max_dim = opts ? opts->max_dim : 0;
	if (cb) {
		int rc = cb("fake ", user_data);
		if (rc != 0)
			return rc;
		return cb("image answer", user_data);
	}
	return 0;
}

static int fake_chat_with_image_fail(struct model *self, const char *system_prompt,
				     const char *prompt, const char *image_path,
				     const struct model_image_chat_options *opts, sse_callback cb,
				     void *user_data)
{
	(void)system_prompt;
	(void)prompt;
	(void)image_path;
	(void)opts;
	(void)cb;
	(void)user_data;
	snprintf(self->last_error, sizeof(self->last_error),
		 "HTTP 400: model does not support image input");
	return MORPH_ERR_API;
}

static int create_test_png(const char *path) {
	unsigned char buf[] = {
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
		0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08,
		0x08, 0x02, 0x00, 0x00, 0x00, 0x4B, 0x6D, 0x6B,
		0xC4, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41,
		0x54, 0x18, 0xD3, 0x63, 0xF8, 0xCF, 0xC0, 0x80,
		0x15, 0x71, 0xD1, 0x41, 0x2B, 0x00, 0x28, 0x3F,
		0x4F, 0xC1, 0x6E, 0xEC, 0xDF, 0x61, 0x00, 0x00,
		0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
		0x60, 0x82
	};
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	size_t written = fwrite(buf, 1, sizeof(buf), f);
	fclose(f);
	return (written == sizeof(buf)) ? 0 : -1;
}

static int create_solid_png(const char *path, int w, int h)
{
	unsigned char *pixels = (unsigned char *)calloc((size_t)w * (size_t)h * 4, 1);
	if (!pixels)
		return -1;
	for (int i = 0; i < w * h; i++) {
		pixels[i * 4 + 0] = 0x40;
		pixels[i * 4 + 1] = 0x80;
		pixels[i * 4 + 2] = 0xC0;
		pixels[i * 4 + 3] = 0xFF;
	}
	int rc = stbi_write_png(path, w, h, 4, pixels, w * 4) ? 0 : -1;
	free(pixels);
	return rc;
}

static int create_solid_jpeg(const char *path, int w, int h)
{
	unsigned char *pixels = (unsigned char *)calloc(
		(size_t)w * (size_t)h * 3, 1);
	if (!pixels)
		return -1;
	for (int i = 0; i < w * h; i++) {
		pixels[i * 3 + 0] = 0x40;
		pixels[i * 3 + 1] = 0x80;
		pixels[i * 3 + 2] = 0xC0;
	}
	int rc = stbi_write_jpg(path, w, h, 3, pixels, 90) ? 0 : -1;
	free(pixels);
	return rc;
}

TEST(ImageGen, InvalidPrompt) {
	struct image_result result;
	int rc = image_gen_create(NULL, NULL, NULL, NULL, NULL, 0, NULL,
				  &result);
	EXPECT_NE(rc, 0);
}

TEST(ImageGen, NullResult) {
	int rc = image_gen_create(NULL, "test", NULL, NULL, NULL, 0, NULL,
				  NULL);
	EXPECT_NE(rc, 0);
}

TEST(ImageGen, RejectsMultipleReferencesForUnsupportedModel) {
	struct model image_model = {};
	struct image_result result = {};
	const char *references[] = {"first.png", "second.png"};

	snprintf(image_model.provider, sizeof(image_model.provider), "openai");
	snprintf(image_model.model_id, sizeof(image_model.model_id), "dall-e-3");
	snprintf(image_model.api_key, sizeof(image_model.api_key), "test-key");
	int rc = image_gen_create(&image_model, "combine them", NULL,
				  "2k", references, 2, NULL, &result);
	EXPECT_EQ(rc, NB_ERR_UNSUPPORTED);
	EXPECT_NE(strstr(image_model.last_error, "multiple reference images"),
		  nullptr);
}

TEST(ImageGen, RejectsTooManyReferences) {
	struct image_result result = {};
	const char *references[IMAGE_GEN_MAX_REFERENCE_IMAGES + 1] = {};

	int rc = image_gen_create(NULL, "combine them", NULL, "2k",
				  references,
				  IMAGE_GEN_MAX_REFERENCE_IMAGES + 1,
				  NULL, &result);
	EXPECT_EQ(rc, NB_ERR_INVAL);
}

TEST(ImageGen, ValidateSizeAllowsAliasesAndBounds) {
	EXPECT_EQ(image_gen_validate_size(NULL), 0);
	EXPECT_EQ(image_gen_validate_size("2k"), 0);
	EXPECT_EQ(image_gen_validate_size("4k"), 0);
	EXPECT_EQ(image_gen_validate_size("2560x1440"), 0);
	EXPECT_EQ(image_gen_validate_size("2048x2048"), 0);
	EXPECT_EQ(image_gen_validate_size("4096x4096"), 0);
}

TEST(ImageGen, ValidateSizeRejectsOutsidePixelRange) {
	EXPECT_NE(image_gen_validate_size("1920x1080"), 0);
	EXPECT_NE(image_gen_validate_size("4097x4096"), 0);
	EXPECT_NE(image_gen_validate_size("4096x4097"), 0);
	EXPECT_NE(image_gen_validate_size("1024x1024"), 0);
}

TEST(ImageGen, ValidateSizeRejectsMalformedValues) {
	EXPECT_NE(image_gen_validate_size("2560X1440"), 0);
	EXPECT_NE(image_gen_validate_size("2560x1440px"), 0);
	EXPECT_NE(image_gen_validate_size("0x4096"), 0);
	EXPECT_NE(image_gen_validate_size("-1x4096"), 0);
	EXPECT_NE(image_gen_validate_size("5k"), 0);
	EXPECT_NE(image_gen_validate_size("3k"), 0);
}

TEST(ImageGen, InfersAndOverridesImageAdapters) {
	struct model image_model = {};

	snprintf(image_model.provider, sizeof(image_model.provider), "openai");
	EXPECT_STREQ(image_gen_adapter_name(&image_model), "openai-images");
	EXPECT_TRUE(image_gen_adapter_supported("openai", ""));
	EXPECT_TRUE(image_gen_adapter_supported("custom", "openai-images"));

	snprintf(image_model.adapter, sizeof(image_model.adapter),
		 "volcengine-images");
	EXPECT_STREQ(image_gen_adapter_name(&image_model),
		     "volcengine-images");
	EXPECT_FALSE(image_gen_adapter_supported("custom", ""));
	EXPECT_FALSE(image_gen_adapter_supported("openai", "unknown-images"));
}

TEST(ImageGen, VolcengineRequestsDisableProviderWatermarkByDefault) {
	struct model image_model = {};
	struct image_request request = {};
	char *body = nullptr;

	snprintf(image_model.model_id, sizeof(image_model.model_id),
		 "doubao-seedream-4-5-251128");
	request.prompt = "a cinematic landscape";
	request.size = "2k";
	ASSERT_EQ(image_volcengine_request_body_create(&image_model, &request, &body), 0);
	ASSERT_NE(body, nullptr);
	cJSON *json = cJSON_Parse(body);
	free(body);
	ASSERT_NE(json, nullptr);
	cJSON *watermark = cJSON_GetObjectItem(json, "watermark");
	EXPECT_TRUE(cJSON_IsFalse(watermark));
	cJSON_Delete(json);
}

TEST(ImageGen, ValidatesGptImageSizes) {
	struct model image_model = {};

	snprintf(image_model.provider, sizeof(image_model.provider), "openai");
	snprintf(image_model.model_id, sizeof(image_model.model_id),
		 "gpt-image-2");
	EXPECT_EQ(image_gen_validate_size_for_model(&image_model, NULL), 0);
	EXPECT_EQ(image_gen_validate_size_for_model(&image_model, "auto"), 0);
	EXPECT_EQ(image_gen_validate_size_for_model(&image_model, "2k"), 0);
	EXPECT_EQ(image_gen_validate_size_for_model(&image_model, "4k"), 0);
	EXPECT_EQ(image_gen_validate_size_for_model(&image_model,
						    "1024x1024"), 0);
	EXPECT_EQ(image_gen_validate_size_for_model(&image_model,
						    "3840x2160"), 0);
	EXPECT_NE(image_gen_validate_size_for_model(&image_model,
						    "4096x4096"), 0);
	EXPECT_NE(image_gen_validate_size_for_model(&image_model,
						    "1025x1024"), 0);
	EXPECT_NE(image_gen_validate_size_for_model(&image_model,
						    "3840x1024"), 0);
}

TEST(ImageGen, KeepsDalleSizeProfile) {
	struct model image_model = {};

	snprintf(image_model.provider, sizeof(image_model.provider), "openai");
	snprintf(image_model.model_id, sizeof(image_model.model_id), "dall-e-3");
	EXPECT_EQ(image_gen_validate_size_for_model(&image_model,
						    "1024x1024"), 0);
	EXPECT_EQ(image_gen_validate_size_for_model(&image_model,
						    "1792x1024"), 0);
	EXPECT_NE(image_gen_validate_size_for_model(&image_model,
						    "2048x2048"), 0);
}

TEST(ImageGen, ParsesUrlAndBase64Responses) {
	struct image_payload payload NB_CLEANUP(image_payload_cleanup) = {};
	const char *url_json =
		"{\"data\":[{\"url\":\"https://example.com/a.png\"}]}";
	const char *b64_json =
		"{\"data\":[{\"b64_json\":\"aGVsbG8=\"}],"
		"\"output_format\":\"webp\","
		"\"usage\":{\"input_tokens\":2,\"output_tokens\":3,"
		"\"total_tokens\":5}}";

	ASSERT_EQ(image_provider_parse_response(NULL, url_json, &payload), 0);
	EXPECT_EQ(payload.kind, IMAGE_PAYLOAD_URL);
	EXPECT_STREQ(payload.data, "https://example.com/a.png");
	image_payload_cleanup(&payload);

	ASSERT_EQ(image_provider_parse_response(NULL, b64_json, &payload), 0);
	EXPECT_EQ(payload.kind, IMAGE_PAYLOAD_BASE64);
	EXPECT_STREQ(payload.data, "aGVsbG8=");
	EXPECT_STREQ(payload.output_format, "webp");
	EXPECT_TRUE(payload.has_usage);
	EXPECT_EQ(payload.usage.total_tokens, 5);
	image_payload_cleanup(&payload);
}

TEST(ImageGen, RejectsMalformedImageResponses) {
	struct image_payload payload NB_CLEANUP(image_payload_cleanup) = {};

	EXPECT_NE(image_provider_parse_response(NULL, "{}", &payload), 0);
	EXPECT_NE(image_provider_parse_response(
			  NULL, "{\"data\":[{}]}", &payload), 0);
	EXPECT_NE(image_provider_parse_response(NULL, "not json", &payload), 0);
}

TEST(ImageGen, ResponseParsingRejectsOccupiedPayloadAndPreservesFailureOutput)
{
	struct image_payload payload NB_CLEANUP(image_payload_cleanup) = {};
	payload.data = strdup("original");
	ASSERT_NE(payload.data, nullptr);
	payload.kind = IMAGE_PAYLOAD_BASE64;
	strcpy(payload.output_format, "webp");
	char *original = payload.data;
	EXPECT_EQ(image_provider_parse_response(NULL, "{\"data\":[{\"url\":\"new\"}]}", &payload),
		  NB_ERR_EXISTS);
	EXPECT_EQ(payload.data, original);
	EXPECT_EQ(payload.kind, IMAGE_PAYLOAD_BASE64);
	EXPECT_STREQ(payload.output_format, "webp");
	struct model model = {};
	struct image_request request = {};
	request.prompt = "example";
	EXPECT_EQ(image_openai_provider_borrow()->execute(&model, &request, &payload),
		  NB_ERR_EXISTS);
	EXPECT_EQ(image_volcengine_provider_borrow()->execute(&model, &request, &payload),
		  NB_ERR_EXISTS);
	EXPECT_EQ(payload.data, original);
	image_payload_cleanup(&payload);
	payload.kind = IMAGE_PAYLOAD_BASE64;
	payload.usage.total_tokens = 73;
	strcpy(payload.output_format, "unchanged");
	EXPECT_EQ(image_provider_parse_response(NULL, "not json", &payload), NB_ERR_PARSE);
	EXPECT_EQ(image_provider_parse_response(NULL, "{\"data\":[{}]}", &payload),
		  NB_ERR_PROTOCOL);
	EXPECT_EQ(payload.data, nullptr);
	EXPECT_EQ(payload.kind, IMAGE_PAYLOAD_BASE64);
	EXPECT_EQ(payload.usage.total_tokens, 73);
	EXPECT_STREQ(payload.output_format, "unchanged");
}

TEST(ImageGen, ParsedPayloadOutlivesItsBorrowedResponse)
{
	struct image_payload payload NB_CLEANUP(image_payload_cleanup) = {};
	{
		char response[] = "{\"data\":[{\"url\":\"independent\"}]}";
		ASSERT_EQ(image_provider_parse_response(NULL, response, &payload), 0);
		memset(response, 'x', sizeof(response) - 1);
	}
	EXPECT_STREQ(payload.data, "independent");
}

TEST(ImageGen, RequestBuildersPreserveOccupiedOutputs)
{
	struct model model = {};
	struct image_request request = {};
	request.prompt = "example";
	cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateString("original");
	ASSERT_NE(value, nullptr);
	char *body NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(value);
	ASSERT_NE(body, nullptr);
	char *original = body;
	EXPECT_EQ(image_volcengine_request_body_create(&model, &request, &body), NB_ERR_EXISTS);
	EXPECT_EQ(body, original);
	nb_buf url NB_CLEANUP(nb_buf_cleanup) = {};
	nb_buf auth NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ((nb_buf_init(&auth), nb_buf_reserve(&auth, 32) ? 0 : NB_ERR_NOMEM), 0);
	ASSERT_EQ((nb_buf_append_cstr(&auth, "original") ? 0 : NB_ERR_NOMEM), 0);
	char *auth_storage = auth.ptr;
	EXPECT_EQ(image_provider_build_request_meta(&model, "/images", &url, &auth), NB_ERR_EXISTS);
	EXPECT_EQ(url.ptr, nullptr);
	EXPECT_EQ(auth.ptr, auth_storage);
	EXPECT_STREQ(nb_buf_cstr(&auth), "original");
}

TEST(ImageGen, DecodesBase64Payloads) {
	size_t decoded_len = 0;
	unsigned char *decoded = test_decode_bytes("aGVsbG8=", &decoded_len);

	ASSERT_NE(decoded, nullptr);
	ASSERT_EQ(decoded_len, 5);
	EXPECT_EQ(memcmp(decoded, "hello", decoded_len), 0);
	free(decoded);
	EXPECT_EQ(test_decode_bytes("bad", &decoded_len), nullptr);
	EXPECT_EQ(decoded_len, 0);
}

TEST(ImageEncode, PreservesSmallPngWithoutTranscoding) {
	const char *path = "/tmp/morph_encode_small.png";
	struct image_encoded encoded = {};
	size_t decoded_len = 0;

	ASSERT_EQ(create_solid_png(path, 64, 32), 0);
	ASSERT_EQ(image_encode_base64_copy(path, 360, &encoded), 0);
	ASSERT_NE(encoded.base64, nullptr);
	EXPECT_STREQ(encoded.mime_type, "image/png");
	char *original = test_media_file_payload(path);
	ASSERT_NE(original, nullptr);
	EXPECT_STREQ(encoded.base64, original);
	free(original);
	unsigned char *decoded = test_decode_bytes(encoded.base64, &decoded_len);
	ASSERT_NE(decoded, nullptr);
	ASSERT_GE(decoded_len, 8u);
	EXPECT_EQ(memcmp(decoded, "\x89PNG\r\n\x1a\n", 8), 0);
	free(decoded);
	image_encoded_cleanup(&encoded);
	remove(path);
}

TEST(ImageEncode, PreservesSmallJpegWithoutTranscoding) {
	const char *path = "/tmp/morph_encode_small.jpg";
	struct image_encoded encoded = {};
	size_t decoded_len = 0;

	ASSERT_EQ(create_solid_jpeg(path, 64, 32), 0);
	ASSERT_EQ(image_encode_base64_copy(path, 360, &encoded), 0);
	ASSERT_NE(encoded.base64, nullptr);
	EXPECT_STREQ(encoded.mime_type, "image/jpeg");
	char *original = test_media_file_payload(path);
	ASSERT_NE(original, nullptr);
	EXPECT_STREQ(encoded.base64, original);
	free(original);
	unsigned char *decoded = test_decode_bytes(encoded.base64, &decoded_len);
	ASSERT_NE(decoded, nullptr);
	ASSERT_GE(decoded_len, 3u);
	EXPECT_EQ(decoded[0], 0xff);
	EXPECT_EQ(decoded[1], 0xd8);
	EXPECT_EQ(decoded[2], 0xff);
	free(decoded);
	image_encoded_cleanup(&encoded);
	remove(path);
}

TEST(ImageEncode, ResizesJpegAndKeepsJpegMime) {
	const char *path = "/tmp/morph_encode_large.jpg";
	struct image_encoded encoded = {};
	size_t decoded_len = 0;
	int width = 0;
	int height = 0;
	int channels = 0;

	ASSERT_EQ(create_solid_jpeg(path, 800, 400), 0);
	ASSERT_EQ(image_encode_base64_copy(path, 360, &encoded), 0);
	EXPECT_STREQ(encoded.mime_type, "image/jpeg");
	unsigned char *decoded = test_decode_bytes(encoded.base64, &decoded_len);
	ASSERT_NE(decoded, nullptr);
	ASSERT_TRUE(stbi_info_from_memory(decoded, (int)decoded_len,
					  &width, &height, &channels));
	EXPECT_EQ(width, 360);
	EXPECT_EQ(height, 180);
	free(decoded);
	image_encoded_cleanup(&encoded);
	remove(path);
}

TEST(ImageEncode, ResizesPngAndKeepsPngMime) {
	const char *path = "/tmp/morph_encode_large.png";
	struct image_encoded encoded = {};
	size_t decoded_len = 0;
	int width = 0;
	int height = 0;
	int channels = 0;

	ASSERT_EQ(create_solid_png(path, 800, 400), 0);
	ASSERT_EQ(image_encode_base64_copy(path, 360, &encoded), 0);
	EXPECT_STREQ(encoded.mime_type, "image/png");
	unsigned char *decoded = test_decode_bytes(encoded.base64, &decoded_len);
	ASSERT_NE(decoded, nullptr);
	ASSERT_TRUE(stbi_info_from_memory(decoded, (int)decoded_len,
					  &width, &height, &channels));
	EXPECT_EQ(width, 360);
	EXPECT_EQ(height, 180);
	free(decoded);
	image_encoded_cleanup(&encoded);
	remove(path);
}

TEST(ImageGen, NormalizeReferenceSizeKeepsInRangeDimensions) {
	int w = 0;
	int h = 0;
	ASSERT_EQ(image_gen_normalize_reference_size(2048, 2048, &w, &h), 0);
	EXPECT_EQ(w, 2048);
	EXPECT_EQ(h, 2048);
}

TEST(ImageGen, NormalizeReferenceSizeUpscalesSmallImages) {
	int w = 0;
	int h = 0;
	ASSERT_EQ(image_gen_normalize_reference_size(1024, 1024, &w, &h), 0);
	EXPECT_EQ(w, 1920);
	EXPECT_EQ(h, 1920);
	EXPECT_EQ(image_gen_validate_size("1920x1920"), 0);
}

TEST(ImageGen, NormalizeReferenceSizeDownscalesLargeImages) {
	int w = 0;
	int h = 0;
	ASSERT_EQ(image_gen_normalize_reference_size(8000, 4000, &w, &h), 0);
	long long pixels = (long long)w * (long long)h;
	EXPECT_GE(pixels, 2560LL * 1440LL);
	EXPECT_LE(pixels, 4096LL * 4096LL);
	EXPECT_NEAR((double)w / (double)h, 2.0, 0.01);
}

TEST(ImageGen, ProbeAndResizeFileExact) {
	const char *path = "/tmp/morph_image_resize_exact.png";
	ASSERT_EQ(create_solid_png(path, 8, 8), 0);

	int w = 0;
	int h = 0;
	ASSERT_EQ(image_probe_size(path, &w, &h), 0);
	EXPECT_EQ(w, 8);
	EXPECT_EQ(h, 8);

	ASSERT_EQ(image_resize_file_exact(path, 37, 19), 0);
	ASSERT_EQ(image_probe_size(path, &w, &h), 0);
	EXPECT_EQ(w, 37);
	EXPECT_EQ(h, 19);
	remove(path);
}

TEST(ImageGen, ResizeFiltersPreserveConstantChannelsAcrossVerticalRatios)
{
	constexpr int input_width = 13;
	constexpr int input_height = 64;
	constexpr int output_width = 19;
	const stbir_pixel_layout layouts[] = {STBIR_1CHANNEL, STBIR_2CHANNEL, STBIR_RGB,
					      STBIR_4CHANNEL};
	for (int channels = 1; channels <= 4; channels++) {
		std::vector<float> input(
			static_cast<size_t>(input_width * input_height * channels));
		for (size_t i = 0; i < input.size(); i++)
			input[i] = static_cast<float>(i % static_cast<size_t>(channels) + 1) / 8.0f;
		for (int output_height : {1, 2, 3, 4, 7, 8, 9, 17, 31, 63, 65, 127}) {
			for (stbir_filter filter :
			     {STBIR_FILTER_BOX, STBIR_FILTER_TRIANGLE, STBIR_FILTER_CUBICBSPLINE,
			      STBIR_FILTER_CATMULLROM, STBIR_FILTER_MITCHELL}) {
				SCOPED_TRACE(::testing::Message() << "channels=" << channels
								  << " height=" << output_height
								  << " filter=" << filter);
				std::vector<float> output(
					static_cast<size_t>(output_width * output_height *
							    channels),
					-1.0f);
				STBIR_RESIZE resize{};
				stbir_resize_init(&resize, input.data(), input_width, input_height,
						  0, output.data(), output_width, output_height, 0,
						  layouts[channels - 1], STBIR_TYPE_FLOAT);
				ASSERT_TRUE(stbir_set_filters(&resize, filter, filter));
				int resized = stbir_resize_extended(&resize);
				stbir_free_samplers(&resize);
				ASSERT_TRUE(resized);
				for (size_t i = 0; i < output.size(); i++) {
					float expected =
						static_cast<float>(
							i % static_cast<size_t>(channels) + 1) /
						8.0f;
					ASSERT_NEAR(output[i], expected, 0.00001f)
						<< "sample=" << i;
				}
			}
		}
	}
}

TEST(ImageGen, ResizePreservesModesAndUnrelatedTemporaryNamesAcrossFormats)
{
	char directory[] = "/tmp/morph-resize-atomic-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	for (const char *extension : {"png", "jpg", "bmp", "tga"}) {
		std::string path = std::string(directory) + "/image." + extension;
		std::string sentinel = path + ".tmp";
		ASSERT_EQ(create_solid_png(path.c_str(), 8, 8), 0);
		ASSERT_EQ(chmod(path.c_str(), 0600), 0);
		ASSERT_EQ(nb_file_write_all(sentinel.c_str(), "keep", 4, nullptr), NB_OK);
		ASSERT_EQ(image_resize_file_exact(path.c_str(), 11, 7), 0);
		int width = 0, height = 0;
		ASSERT_EQ(image_probe_size(path.c_str(), &width, &height), 0);
		EXPECT_EQ(width, 11);
		EXPECT_EQ(height, 7);
		struct stat metadata{};
		ASSERT_EQ(stat(path.c_str(), &metadata), 0);
		EXPECT_EQ(metadata.st_mode & 0777, 0600u);
		nb_buf retained NB_CLEANUP(nb_buf_cleanup) = {};
		ASSERT_EQ(nb_file_read_all(sentinel.c_str(), &retained, 0, nullptr), NB_OK);
		EXPECT_TRUE(nb_str_eq_cstr(nb_buf_str(&retained), "keep"));
		EXPECT_EQ(unlink(path.c_str()), 0);
		EXPECT_EQ(unlink(sentinel.c_str()), 0);
	}
	EXPECT_TRUE(std::filesystem::is_empty(directory));
	EXPECT_EQ(rmdir(directory), 0);
}

TEST(ImageGen, ResizeRejectsSymlinkWithoutChangingTarget)
{
	char directory[] = "/tmp/morph-resize-link-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::string target = std::string(directory) + "/target.png";
	std::string link = std::string(directory) + "/link.png";
	ASSERT_EQ(create_solid_png(target.c_str(), 8, 8), 0);
	ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0);
	EXPECT_EQ(image_resize_file_exact(link.c_str(), 11, 7), NB_ERR_IO);
	struct stat metadata{};
	ASSERT_EQ(lstat(link.c_str(), &metadata), 0);
	EXPECT_TRUE(S_ISLNK(metadata.st_mode));
	int width = 0, height = 0;
	ASSERT_EQ(image_probe_size(target.c_str(), &width, &height), 0);
	EXPECT_EQ(width, 8);
	EXPECT_EQ(height, 8);
	EXPECT_EQ(unlink(link.c_str()), 0);
	EXPECT_EQ(unlink(target.c_str()), 0);
	EXPECT_TRUE(std::filesystem::is_empty(directory));
	EXPECT_EQ(rmdir(directory), 0);
}

TEST(ImageRender, NullPath) {
	int rc = image_render_terminal(NULL, NULL);
	EXPECT_NE(rc, 0);
}

TEST(ImageRender, EmptyPath) {
	int rc = image_render_terminal(NULL, "");
	EXPECT_NE(rc, 0);
}

TEST(ImageRender, ValidFileFallback) {
	const char *path = "/tmp/test_render.png";
	ASSERT_EQ(create_test_png(path), 0);
	int rc = image_render_terminal(NULL, path);
	EXPECT_EQ(rc, 0);
	remove(path);
}

TEST(ImageRender, KittyProtocol) {
	const char *path = "/tmp/test_render.png";
	ASSERT_EQ(create_test_png(path), 0);
	setenv("KITTY_WINDOW_ID", "12345", 1);
	int rc = image_render_terminal(NULL, path);
	EXPECT_EQ(rc, 0);
	unsetenv("KITTY_WINDOW_ID");
	remove(path);
}

TEST(ImageRender, Iterm2Protocol) {
	const char *path = "/tmp/test_render.png";
	ASSERT_EQ(create_test_png(path), 0);
	setenv("ITERM_PROFILE", "Default", 1);
	int rc = image_render_terminal(NULL, path);
	EXPECT_EQ(rc, 0);
	unsetenv("ITERM_PROFILE");
	remove(path);
}

TEST(ImageRender, NonexistentFile) {
	const char *path = "/tmp/nonexistent_test_img.png";
	remove(path);
	image_render_terminal(NULL, path);
	SUCCEED();  /* fallback to printing path is acceptable */
}

class ImgGenToolTest : public ::testing::Test {
protected:
  FakeImageQaState qa;
  struct tool_registry reg;
  void SetUp() override
  {
	  tool_registry_init(NULL, &reg);
  }
	void TearDown() override {
		tool_registry_cleanup(&reg);
	}
};

TEST_F(ImgGenToolTest, RegisterTool) {
	int rc = img_gen_init(&reg, NULL, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(reg.count, 1);
	struct tool_entry *e = tool_borrow(&reg, "img_gen");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_gen");
	EXPECT_NE(strstr(e->desc.description, "auto"), nullptr);
	EXPECT_NE(strstr(e->desc.input_schema, "auto"), nullptr);
	EXPECT_NE(strstr(e->desc.input_schema, "\"default\":\"2k\""),
		  nullptr);
	EXPECT_NE(strstr(e->desc.input_schema, "\"reference_images\""),
		  nullptr);
	EXPECT_NE(strstr(e->desc.input_schema, "\"maxItems\":10"), nullptr);
}

TEST_F(ImgGenToolTest, ExecMissingPrompt) {
	img_gen_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "error") != NULL ||
		    strstr(result.text.ptr, "missing") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ToolNotFound)
{
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	tool_result_init(&result);
	int rc = tool_exec(&reg, "nonexistent", "{}", &result);
	EXPECT_NE(rc, 0);
}

TEST_F(ImgGenToolTest, ExecInvalidSize) {
	img_gen_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen",
		"{\"prompt\":\"a cat\",\"size\":\"1920x1080\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(strstr(result.text.ptr, "invalid size"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ExecRejectsAutoSize) {
	img_gen_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen",
		"{\"prompt\":\"a cat\",\"size\":\"auto\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(strstr(result.text.ptr, "invalid size"), nullptr);
	EXPECT_EQ(strstr(result.text.ptr, "auto"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ExecAcceptsCustomSizeFromConfiguredModelProfile) {
	struct model image_model = {};
	snprintf(image_model.provider, sizeof(image_model.provider), "openai");
	snprintf(image_model.model_id, sizeof(image_model.model_id),
		 "gpt-image-2");
	img_gen_init(&reg, &image_model, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen",
		"{\"prompt\":\"a cat\",\"size\":\"1024x1024\"}",
		&result);
	EXPECT_EQ(rc, MORPH_ERR_NOT_CONFIGURED);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_EQ(strstr(result.text.ptr, "invalid size"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ExecAcceptsDallePortraitLandscapeAndAutomaticSizes)
{
	struct model image_model = {};
	snprintf(image_model.provider, sizeof(image_model.provider), "openai");
	snprintf(image_model.model_id, sizeof(image_model.model_id), "dall-e-3");
	ASSERT_EQ(img_gen_init(&reg, &image_model, nullptr), 0);
	for (const char *size : {"1024x1024", "1024x1792", "1792x1024", "auto"}) {
		SCOPED_TRACE(size);
		std::string args = std::string("{\"prompt\":\"a cat\",\"size\":\"") + size + "\"}";
		struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
		EXPECT_EQ(tool_exec(&reg, "img_gen", args.c_str(), &result),
		MORPH_ERR_NOT_CONFIGURED);
		ASSERT_NE(result.text.ptr, nullptr);
		EXPECT_EQ(strstr(result.text.ptr, "invalid size"), nullptr);
	}
}

TEST_F(ImgGenToolTest, ExecRejectsSingularAndPluralReferencesTogether) {
	img_gen_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen",
		"{\"prompt\":\"combine them\","
		"\"reference_image\":\"one.png\","
		"\"reference_images\":[\"two.png\"]}", &result);
	EXPECT_EQ(rc, NB_ERR_INVAL);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(strstr(result.text.ptr, "not both"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ExecRejectsMoreThanTenReferences) {
	img_gen_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen",
		"{\"prompt\":\"combine them\",\"reference_images\":["
		"\"1.png\",\"2.png\",\"3.png\",\"4.png\",\"5.png\","
		"\"6.png\",\"7.png\",\"8.png\",\"9.png\",\"10.png\","
		"\"11.png\"]}", &result);
	EXPECT_EQ(rc, NB_ERR_INVAL);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(strstr(result.text.ptr, "at most 10"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ExecRejectsInvalidReferenceArrayItem) {
	img_gen_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_gen",
		"{\"prompt\":\"combine them\","
		"\"reference_images\":[\"one.png\",42]}", &result);
	EXPECT_EQ(rc, NB_ERR_INVAL);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_NE(strstr(result.text.ptr, "non-empty file path"), nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, InpaintRegister) {
	int rc = img_inpaint_init(&reg, NULL, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_borrow(&reg, "img_inpaint");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_inpaint");
}

TEST_F(ImgGenToolTest, InpaintExecBadJson) {
	img_inpaint_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_inpaint", "not json", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "error") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ComposeRegister) {
	int rc = img_compose_init(&reg, NULL, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_borrow(&reg, "img_compose");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_compose");
}

TEST_F(ImgGenToolTest, ComposeExecBadJson) {
	img_compose_init(&reg, NULL, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_compose", "not json", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "error") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, InfoRegister) {
	int rc = img_info_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_borrow(&reg, "img_info");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_info");
}

TEST_F(ImgGenToolTest, InfoMissingPath) {
	img_info_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_info", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, InfoInvalidFile) {
	img_info_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_info", "{\"file_path\":\"/nonexistent.png\"}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, InfoDeniesSymlinkEscape) {
	const char *work = "/tmp/morph_img_work";
	const char *outside = "/tmp/morph_img_outside.png";
	const char *link_path = "/tmp/morph_img_work/link.png";
	nb_file_mkdir(work, true, NULL);
	ASSERT_EQ(create_test_png(outside), 0);
	std::remove(link_path);
	ASSERT_EQ(symlink(outside, link_path), 0);
	struct tool_context *tctx = tool_context_create(NULL, work, "/tmp/morph_img_out");
	ASSERT_NE(tctx, nullptr);
	img_info_init(&reg, tctx);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_info",
		"{\"file_path\":\"link.png\"}", &result);
	EXPECT_EQ(rc, NB_ERR_NOT_PERMITTED);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "permission denied") != NULL);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(link_path);
	std::remove(outside);
	rmdir(work);
}

TEST_F(ImgGenToolTest, QaRegister) {
	struct model fake = {};
	fake.api_key[0] = 'x';
	fake.chat_with_image = fake_chat_with_image;
	fake.handle = &qa;
	int rc = img_qa_init(&reg, &fake, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_borrow(&reg, "img_qa");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_qa");
}

TEST_F(ImgGenToolTest, QaMissingPath) {
	struct model fake = {};
	fake.api_key[0] = 'x';
	fake.chat_with_image = fake_chat_with_image;
	fake.handle = &qa;
	img_qa_init(&reg, &fake, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_qa", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "file_path") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, QaRequiresConfiguredLlm) {
	struct model fake = {};
	fake.chat_with_image = fake_chat_with_image;
	fake.handle = &qa;
	img_qa_init(&reg, &fake, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_qa",
			   "{\"file_path\":\"/tmp/nonexistent.png\"}",
			   &result);
	EXPECT_EQ(rc, MORPH_ERR_NOT_CONFIGURED);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "multimodal") != NULL);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, QaCallsMultimodalLlm) {
	const char *path = "/tmp/morph_img_qa.png";
	ASSERT_EQ(create_test_png(path), 0);
	struct model fake = {};
	fake.api_key[0] = 'x';
	fake.chat_with_image = fake_chat_with_image;
	fake.handle = &qa;
	qa.prompt[0] = '\0';
	qa.path[0] = '\0';
	qa.max_tokens = 0;
	qa.timeout_seconds = 0;
	qa.max_dim = 0;
	img_qa_init(&reg, &fake, NULL);
	struct morph_event_recorder rec;
	ASSERT_EQ(morph_event_recorder_init(&rec), 0);
	struct tool_runtime_context rt = {};
	rt.event_cb = morph_event_recorder_cb;
	rt.event_user_data = &rec;
	rt.turn_id = "turn_img_qa";
	struct tool_result result;
	tool_result_init(&result);
	result.runtime = &rt;
	int rc = tool_exec(&reg, "img_qa",
			   "{\"file_path\":\"/tmp/morph_img_qa.png\","
			   "\"prompt\":\"OCR this\"}",
			   &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	ASSERT_NE(result.data, nullptr);
	{
		cJSON *text = cJSON_GetObjectItem(result.data, "text");
		ASSERT_TRUE(cJSON_IsString(text));
		EXPECT_STREQ(text->valuestring, "fake image answer");
	}
	EXPECT_STREQ(qa.prompt, "OCR this");
	EXPECT_STREQ(qa.path, path);
	EXPECT_EQ(qa.max_tokens, 4096);
	EXPECT_EQ(qa.timeout_seconds, 120);
	EXPECT_EQ(qa.max_dim, 2048);
	EXPECT_GE(morph_event_recorder_count(&rec), 3u);
	bool saw_stream = false;
	for (size_t i = 0; i < morph_event_recorder_count(&rec); i++) {
		const char *json = morph_event_recorder_borrow(&rec, i);
		if (json && strstr(json, "\"name\":\"tool.stream.delta\"") &&
		    strstr(json, "\"turn_id\":\"turn_img_qa\""))
			saw_stream = true;
	}
	EXPECT_TRUE(saw_stream);
	tool_result_cleanup(&result);
	morph_event_recorder_cleanup(&rec);
	std::remove(path);
}

TEST_F(ImgGenToolTest, QaPassesExplicitOptions) {
	const char *path = "/tmp/morph_img_qa_opts.png";
	ASSERT_EQ(create_test_png(path), 0);
	struct model fake = {};
	fake.api_key[0] = 'x';
	fake.chat_with_image = fake_chat_with_image;
	fake.handle = &qa;
	qa.max_tokens = 0;
	qa.timeout_seconds = 0;
	qa.max_dim = 0;
	img_qa_init(&reg, &fake, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_qa",
			   "{\"file_path\":\"/tmp/morph_img_qa_opts.png\","
			   "\"max_tokens\":77,\"timeout_seconds\":12,"
			   "\"max_dim\":512}",
			   &result);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(qa.max_tokens, 77);
	EXPECT_EQ(qa.timeout_seconds, 12);
	EXPECT_EQ(qa.max_dim, 512);
	tool_result_cleanup(&result);
	std::remove(path);
}

TEST_F(ImgGenToolTest, QaReturnsProviderFailureDetail) {
	const char *path = "/tmp/morph_img_qa_fail.png";
	ASSERT_EQ(create_test_png(path), 0);
	struct model fake = {};
	fake.api_key[0] = 'x';
	fake.chat_with_image = fake_chat_with_image_fail;
	img_qa_init(&reg, &fake, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_qa",
			   "{\"file_path\":\"/tmp/morph_img_qa_fail.png\","
			   "\"prompt\":\"What is this?\"}",
			   &result);
	EXPECT_EQ(rc, MORPH_ERR_API);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "image QA LLM call failed") != NULL);
	EXPECT_TRUE(strstr(result.text.ptr, "model does not support image input") != NULL);
	EXPECT_TRUE(strstr(result.text.ptr, "[model.vision]") != NULL);
	tool_result_cleanup(&result);
	std::remove(path);
}

TEST_F(ImgGenToolTest, QaUsesConfiguredOutputBudgetAndSupportsOriginalDetail)
{
	struct model fake = {};
	fake.api_key[0] = 'x';
	fake.max_tokens = 16384;
	fake.chat_with_image = fake_chat_with_image;
	fake.handle = &qa;
	ASSERT_EQ(img_qa_init(&reg, &fake, nullptr), 0);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(tool_exec(&reg, "img_qa",
		"{\"file_path\":\"/tmp/original.png\",\"detail\":\"original\"}", &result), 0);
	EXPECT_EQ(qa.max_tokens, 16384);
	EXPECT_EQ(qa.max_dim, MORPH_IMAGE_MAX_DIMENSION);
	tool_result_cleanup(&result);
	ASSERT_EQ(tool_exec(&reg, "img_qa",
		"{\"file_path\":\"/tmp/original.png\",\"max_dim\":6000,\"max_tokens\":8192}",
		&result), 0);
	EXPECT_EQ(qa.max_tokens, 8192);
	EXPECT_EQ(qa.max_dim, 6000);
	for (const char *invalid : {
		"{\"file_path\":\"a.png\",\"max_tokens\":16385}",
		"{\"file_path\":\"a.png\",\"max_tokens\":1.5}",
		"{\"file_path\":\"a.png\",\"max_dim\":32769}",
		"{\"file_path\":\"a.png\",\"detail\":\"low\"}"}) {
		SCOPED_TRACE(invalid);
		tool_result_cleanup(&result);
		EXPECT_EQ(tool_exec(&reg, "img_qa", invalid, &result), NB_ERR_INVAL);
	}
}

TEST_F(ImgGenToolTest, ResizeRegister) {
	int rc = img_resize_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_borrow(&reg, "img_resize");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_resize");
}

TEST_F(ImgGenToolTest, ResizeMissingArgs) {
	img_resize_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_resize", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ResizeInvalidFile) {
	img_resize_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_resize",
		"{\"file_path\":\"/nonexistent.png\",\"width\":10,\"height\":10}",
		&result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ResizeDeniesOutsideInput) {
	const char *work = "/tmp/morph_img_work";
	const char *outside = "/tmp/morph_img_outside.png";
	nb_file_mkdir(work, true, NULL);
	ASSERT_EQ(create_test_png(outside), 0);
	struct tool_context *tctx = tool_context_create(NULL, work, "/tmp/morph_img_out");
	ASSERT_NE(tctx, nullptr);
	img_resize_init(&reg, tctx);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_resize",
		"{\"file_path\":\"/tmp/morph_img_outside.png\","
		"\"width\":4,\"height\":4}",
		&result);
	EXPECT_EQ(rc, NB_ERR_NOT_PERMITTED);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "permission denied") != NULL);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(outside);
	rmdir(work);
}

TEST_F(ImgGenToolTest, ConvertRegister) {
	int rc = img_convert_init(&reg, NULL);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_borrow(&reg, "img_convert");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "img_convert");
}

TEST_F(ImgGenToolTest, ConvertMissingArgs) {
	img_convert_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_convert", "{}", &result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ConvertUnsupportedFormat) {
	img_convert_init(&reg, NULL);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_convert",
		"{\"file_path\":\"/tmp/x.png\",\"format\":\"avif\"}",
		&result);
	EXPECT_NE(rc, 0);
	ASSERT_NE(result.text.ptr, nullptr);
	tool_result_cleanup(&result);
}

TEST_F(ImgGenToolTest, ConvertDeniesOutsideInputBeforeOutput) {
	const char *work = "/tmp/morph_img_work";
	const char *outside = "/tmp/morph_img_outside.png";
	nb_file_mkdir(work, true, NULL);
	ASSERT_EQ(create_test_png(outside), 0);
	struct tool_context *tctx = tool_context_create(NULL, work, "/tmp/morph_img_out");
	ASSERT_NE(tctx, nullptr);
	img_convert_init(&reg, tctx);
	struct tool_result result;
	tool_result_init(&result);
	int rc = tool_exec(&reg, "img_convert",
		"{\"file_path\":\"/tmp/morph_img_outside.png\","
		"\"format\":\"jpg\"}",
		&result);
	EXPECT_EQ(rc, NB_ERR_NOT_PERMITTED);
	ASSERT_NE(result.text.ptr, nullptr);
	EXPECT_TRUE(strstr(result.text.ptr, "permission denied") != NULL);
	tool_result_cleanup(&result);
	tool_context_destroy(tctx);
	std::remove(outside);
	rmdir(work);
}

TEST(ImageDetectFmt, Png) {
	unsigned char png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00};
	EXPECT_EQ(image_detect_fmt(png, sizeof(png)), 100);
}

TEST(ImageDetectFmt, Jpeg) {
	unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00};
	EXPECT_EQ(image_detect_fmt(jpg, sizeof(jpg)), 101);
}

TEST(ImageDetectFmt, Unknown) {
	unsigned char raw[] = {0x00, 0x01, 0x02, 0x03};
	EXPECT_EQ(image_detect_fmt(raw, sizeof(raw)), 0);
}

TEST(ImageDetectFmt, ShortBuffer) {
	unsigned char buf[] = {0x89};
	EXPECT_EQ(image_detect_fmt(buf, 1), 0);
}

static int create_test_jpeg(const char *path)
{
	const unsigned char pixel[] = {240, 120, 40};
	return stbi_write_jpg(path, 1, 1, 3, pixel, 90) ? 0 : NB_ERR_IO;
}

TEST(ImageRender, KittyJpegProtocol) {
	const char *path = "/tmp/test_kitty_jpeg.jpg";
	ASSERT_EQ(create_test_jpeg(path), 0);
	setenv("KITTY_WINDOW_ID", "12345", 1);

	char captured[4096] = {0};
	fflush(stdout);
	int pipefd[2];
	ASSERT_EQ(pipe(pipefd), 0);
	int old_stdout = dup(STDOUT_FILENO);
	ASSERT_NE(old_stdout, -1);
	ASSERT_EQ(dup2(pipefd[1], STDOUT_FILENO), STDOUT_FILENO);
	close(pipefd[1]);

	int rc = image_render_terminal(NULL, path);

	fflush(stdout);
	dup2(old_stdout, STDOUT_FILENO);
	close(old_stdout);
	ssize_t n = read(pipefd[0], captured, sizeof(captured) - 1);
	close(pipefd[0]);
	if (n > 0) captured[n] = '\0';

	EXPECT_EQ(rc, 0);
	/*
	 * Kitty's graphics protocol only supports f=100 (PNG), f=24 (RGB),
	 * and f=32 (RGBA). JPEG input must be transcoded to PNG before
	 * transmission, so we expect f=100 in the output stream.
	 */
	EXPECT_NE(strstr(captured, "f=100"), nullptr)
		<< "JPEG input must be transcoded to PNG (f=100) for kitty, got: " << captured;
	EXPECT_EQ(strstr(captured, "f=101"), nullptr)
		<< "f=101 is not a valid kitty graphics format, got: " << captured;
	EXPECT_NE(strstr(captured, "U=1"), nullptr);
	EXPECT_NE(strstr(captured, "\xF4\x8E\xBB\xAE"), nullptr);
	unsetenv("KITTY_WINDOW_ID");
	remove(path);
}

TEST(ImageDetectFmt, WebpMagicBytes) {
	unsigned char webp[] = {0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50};
	EXPECT_EQ(image_detect_fmt(webp, sizeof(webp)), 0)
		<< "WebP not natively detected by image_detect_fmt";
}

TEST(ImageDetectFmt, GifMagicBytes) {
	unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x00};
	EXPECT_EQ(image_detect_fmt(gif, sizeof(gif)), 0)
		<< "GIF not natively detected by image_detect_fmt";
}

TEST(ImageDetectFmt, NullInput) {
	EXPECT_EQ(image_detect_fmt(NULL, 0), 0);
}

TEST(ImageRender, KittyPngProtocol) {
	const char *path = "/tmp/test_kitty_png.png";
	ASSERT_EQ(create_test_png(path), 0);
	setenv("KITTY_WINDOW_ID", "12345", 1);

	char captured[4096] = {0};
	fflush(stdout);
	int pipefd[2];
	ASSERT_EQ(pipe(pipefd), 0);
	int old_stdout = dup(STDOUT_FILENO);
	ASSERT_NE(old_stdout, -1);
	ASSERT_EQ(dup2(pipefd[1], STDOUT_FILENO), STDOUT_FILENO);
	close(pipefd[1]);

	int rc = image_render_terminal(NULL, path);

	fflush(stdout);
	dup2(old_stdout, STDOUT_FILENO);
	close(old_stdout);
	ssize_t n = read(pipefd[0], captured, sizeof(captured) - 1);
	close(pipefd[0]);
	if (n > 0) captured[n] = '\0';

	EXPECT_EQ(rc, 0);
	EXPECT_NE(strstr(captured, "f=100"), nullptr)
		<< "PNG data should use f=100 in kitty protocol, got: " << captured;
	EXPECT_NE(strstr(captured, "U=1"), nullptr);
	EXPECT_NE(strstr(captured, "\xF4\x8E\xBB\xAE"), nullptr);
	unsetenv("KITTY_WINDOW_ID");
	remove(path);
}

TEST(ImageGenExtContentType, Png) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "png");
}

TEST(ImageGenExtContentType, Jpeg) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "jpg");
}

TEST(ImageGenExtContentType, Webp) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/webp\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "webp");
}

TEST(ImageGenExtContentType, Gif) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "gif");
}

TEST(ImageGenExtContentType, Bmp) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "bmp");
}

TEST(ImageGenExtContentType, CaseInsensitive) {
	const char *headers = "HTTP/1.1 200 OK\r\nCONTENT-TYPE: IMAGE/JPEG\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "jpg");
}

TEST(ImageGenExtContentType, WithCharset) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: image/png; charset=utf-8\r\n\r\n";
	EXPECT_STREQ(image_gen_ext_from_content_type(headers), "png");
}

TEST(ImageGenExtContentType, UnknownType) {
	const char *headers = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n";
	EXPECT_EQ(image_gen_ext_from_content_type(headers), nullptr);
}

TEST(ImageGenExtContentType, NullHeaders) {
	EXPECT_EQ(image_gen_ext_from_content_type(NULL), nullptr);
}

TEST(ImageGenExtContentType, EmptyHeaders) {
	EXPECT_EQ(image_gen_ext_from_content_type(""), nullptr);
}

TEST(ImageGenExtMagic, Png) {
	unsigned char png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
	EXPECT_STREQ(image_gen_ext_from_magic(png, sizeof(png)), "png");
}

TEST(ImageGenExtMagic, Jpeg) {
	unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0};
	EXPECT_STREQ(image_gen_ext_from_magic(jpg, sizeof(jpg)), "jpg");
}

TEST(ImageGenExtMagic, Webp) {
	unsigned char webp[] = {0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50};
	EXPECT_STREQ(image_gen_ext_from_magic(webp, sizeof(webp)), "webp");
}

TEST(ImageGenExtMagic, Gif87a) {
	unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x37, 0x61};
	EXPECT_STREQ(image_gen_ext_from_magic(gif, sizeof(gif)), "gif");
}

TEST(ImageGenExtMagic, Gif89a) {
	unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61};
	EXPECT_STREQ(image_gen_ext_from_magic(gif, sizeof(gif)), "gif");
}

TEST(ImageGenExtMagic, Bmp) {
	unsigned char bmp[] = {0x42, 0x4D, 0x00, 0x00};
	EXPECT_STREQ(image_gen_ext_from_magic(bmp, sizeof(bmp)), "bmp");
}

TEST(ImageGenExtMagic, UnknownDefaultsToPng) {
	unsigned char unknown[] = {0x00, 0x01, 0x02, 0x03};
	EXPECT_STREQ(image_gen_ext_from_magic(unknown, sizeof(unknown)), "png");
}

TEST(ImageGenExtMagic, NullData) {
	EXPECT_STREQ(image_gen_ext_from_magic(NULL, 0), "png");
}

TEST(ImageGenExtMagic, ShortBuffer) {
	unsigned char buf[] = {0x89};
	EXPECT_STREQ(image_gen_ext_from_magic(buf, 1), "png");
}

TEST(ImageEncode, RejectsExcessivePixelsAndDimensionsBeforeAllocation)
{
	size_t bytes = 0;
	EXPECT_EQ(image_pixel_bytes(4096, 4096, 4, &bytes), 0);
	EXPECT_EQ(bytes, 64u * 1024u * 1024u);
	EXPECT_EQ(image_pixel_bytes(32768, 32768, 4, &bytes), NB_ERR_TOO_BIG);
	EXPECT_EQ(image_pixel_bytes(INT_MAX, INT_MAX, 4, &bytes), NB_ERR_TOO_BIG);
	EXPECT_EQ(image_pixel_bytes(0, 100, 4, &bytes), NB_ERR_INVAL);
	EXPECT_EQ(image_pixel_bytes(100, 100, 5, &bytes), NB_ERR_INVAL);
	const char *path = "/tmp/morph_resize_limit.png";
	ASSERT_EQ(create_solid_png(path, 16, 16), 0);
	EXPECT_EQ(image_resize_file_exact(path, 32768, 32768), NB_ERR_TOO_BIG);
	int w = 0, h = 0;
	ASSERT_EQ(image_probe_size(path, &w, &h), 0);
	EXPECT_EQ(w, 16);
	EXPECT_EQ(h, 16);
	remove(path);
}

TEST(MediaOutputTest, ConcurrentArtifactsKeepIndependentBytes)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char directory[] = "/tmp/morph-media-output-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	constexpr int count = 20;
	std::vector<std::string> paths(count);
	std::vector<int> results(count);
	std::vector<std::thread> workers;

	for (int i = 0; i < count; i++) {
		workers.emplace_back([&, i] {
			std::string content = "artifact " + std::to_string(i);
			char path[PATH_MAX];
			results[i] = media_output_write(directory, "img", "png", content.data(),
							content.size(), path, sizeof(path));
			paths[i] = path;
		});
	}
	for (auto &worker : workers)
		worker.join();
	for (int i = 0; i < count; i++) {
		ASSERT_EQ(results[i], 0);
		struct stat metadata{};
		ASSERT_EQ(stat(paths[i].c_str(), &metadata), 0);
		EXPECT_EQ(metadata.st_mode & 0777, 0600u);
		std::string expected = "artifact " + std::to_string(i);
		char *content =
			(nb_file_read_all(paths[i].c_str(), &file_value_1, 0, NULL) == NB_OK &&
					 (file_copy_1 = (char *)malloc(nb_buf_len(&file_value_1) +
								       1)) != NULL
				 ? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
					   nb_buf_len(&file_value_1) + 1),
				    nb_charp_steal(&file_copy_1))
				 : NULL);
		ASSERT_NE(content, nullptr);
		EXPECT_EQ(std::string(content), expected);
		free(content);
		for (int j = 0; j < i; j++)
			EXPECT_NE(paths[i], paths[j]);
		EXPECT_EQ(unlink(paths[i].c_str()), 0);
	}
	EXPECT_TRUE(std::filesystem::is_empty(directory));
	EXPECT_EQ(rmdir(directory), 0);
}

TEST(MediaOutputTest, FailureLeavesNoPublishedOrTemporaryFile)
{
	char directory[] = "/tmp/morph-media-failure-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	char output[2] = {'x', '\0'};

	EXPECT_EQ(media_output_write(directory, "vid", "mp4", "data", 4, output, sizeof(output)),
		  NB_ERR_NAME_TOO_LONG);
	EXPECT_EQ(output[0], '\0');
	EXPECT_TRUE(std::filesystem::is_empty(directory));
	EXPECT_EQ(rmdir(directory), 0);
}

TEST(AnnotationTest, ParsedCopySurvivesSourceReleaseForAllEstablishedShapes)
{
	const char *body = "{\"images\":[{\"path\":\"input.png\",\"width\":200,\"height\":100}],"
			   "\"bboxes\":[{\"image_index\":0,\"x\":20,\"y\":10,\"w\":40,\"h\":30,"
			   "\"label\":\"cat\"}]}";
	for (int shape = 0; shape < 3; shape++) {
		cJSON *input = cJSON_Parse(body);
		if (shape > 0) {
			cJSON *wrapper = cJSON_CreateObject();
			if (shape == 1)
				cJSON_AddItemToObject(wrapper, "annotation", input);
			else {
				cJSON_AddStringToObject(wrapper, "annotation", body);
				cJSON_Delete(input);
			}
			input = wrapper;
		}
		annotation parsed{};
		ASSERT_EQ(annotation_parse_copy(input, &parsed), 0);
		cJSON_Delete(input);
		input = nullptr;
		ASSERT_EQ(parsed.images.len, 1u);
		ASSERT_EQ(parsed.bboxes.len, 1u);
		const auto *image = annotation_image_borrow(&parsed, 0);
		ASSERT_NE(image, nullptr);
		EXPECT_STREQ(image->path, "input.png");
		const auto *bbox =
			static_cast<const annotation_bbox *>(nb_vec_at(&parsed.bboxes, 0));
		annotation_rect normalized{};
		ASSERT_EQ(annotation_rect_normalize(&bbox->rect, image->width, image->height,
						    &normalized),
			  0);
		EXPECT_DOUBLE_EQ(normalized.x, 0.1);
		EXPECT_DOUBLE_EQ(normalized.y, 0.1);
		EXPECT_DOUBLE_EQ(normalized.width, 0.2);
		EXPECT_DOUBLE_EQ(normalized.height, 0.3);
		EXPECT_STREQ(bbox->label, "cat");
		annotation_cleanup(&parsed);
		cJSON_Delete(input);
	}
}

TEST(AnnotationTest, OccupiedOutputPreservesExistingOwner)
{
	cJSON *input = cJSON_Parse("{\"images\":[{\"path\":\"source.png\"}]}");
	ASSERT_NE(input, nullptr);
	annotation parsed{};
	ASSERT_EQ(annotation_parse_copy(input, &parsed), 0);
	cJSON *owner = parsed.owned;
	EXPECT_EQ(annotation_parse_copy(input, &parsed), NB_ERR_STATE);
	EXPECT_EQ(parsed.owned, owner);
	cJSON_Delete(input);
	EXPECT_STREQ(annotation_image_borrow(&parsed, 0)->path, "source.png");
	annotation_cleanup(&parsed);
}

TEST(AnnotationTest, RejectsInvalidCoordinatesAndImageIndices)
{
	for (const char *bbox : {"{\"image_index\":0,\"x\":-0.2,\"y\":0,\"w\":0.3,\"h\":0.3}",
				 "{\"image_index\":0,\"x\":0,\"y\":0,\"w\":0,\"h\":0.3}",
				 "{\"image_index\":1,\"x\":0,\"y\":0,\"w\":0.3,\"h\":0.3}",
				 "{\"image_index\":0.5,\"x\":0,\"y\":0,\"w\":0.3,\"h\":0.3}",
				 "{\"image_index\":0,\"x\":\"bad\",\"y\":0,\"w\":0.3,\"h\":0.3}"}) {
		std::string json = "{\"images\":[{\"path\":\"input.png\"}],\"bboxes\":[";
		json += bbox;
		json += "]}";
		cJSON *input = cJSON_Parse(json.c_str());
		annotation parsed{};
		EXPECT_EQ(annotation_parse_copy(input, &parsed), NB_ERR_INVAL);
		annotation_cleanup(&parsed);
		cJSON_Delete(input);
	}
	annotation_rect invalid{-0.1, 0, 0.5, 0.5};
	annotation_rect out{};
	EXPECT_EQ(annotation_rect_normalize(&invalid, 100, 100, &out), NB_ERR_INVAL);
	annotation_rect normalized{0.1, 0.2, 0.3, 0.4};
	ASSERT_EQ(annotation_rect_normalize(&normalized, 0, 0, &out), 0);
	EXPECT_DOUBLE_EQ(out.width, 0.3);
}

TEST(AnnotationTest, MainImageUsesArrowCountThenSmallestIndex)
{
	cJSON *input = cJSON_Parse("{\"images\":[{\"path\":\"a\"},{\"path\":\"b\"}],\"arrows\":["
				   "{\"from\":{\"image_index\":0,\"x\":0.2,\"y\":0.3},"
				   "\"to\":{\"image_index\":1,\"x\":20,\"y\":30}},"
				   "{\"from\":{\"image_index\":1,\"x\":0.2,\"y\":0.3},"
				   "\"to\":{\"image_index\":0,\"x\":0.4,\"y\":0.5}}]}");
	annotation parsed{};
	ASSERT_EQ(annotation_parse_copy(input, &parsed), 0);
	EXPECT_EQ(annotation_main_image(&parsed), 0);
	const auto *arrow = static_cast<const annotation_arrow *>(nb_vec_at(&parsed.arrows, 0));
	double x, y;
	ASSERT_EQ(annotation_point_pixels(&arrow->from, 100, 100, &x, &y), 0);
	EXPECT_DOUBLE_EQ(x, 20);
	EXPECT_DOUBLE_EQ(y, 30);
	ASSERT_EQ(annotation_point_pixels(&arrow->to, 100, 100, &x, &y), 0);
	EXPECT_DOUBLE_EQ(x, 20);
	EXPECT_DOUBLE_EQ(y, 30);
	annotation_cleanup(&parsed);
	cJSON_Delete(input);
}

TEST_F(ImgGenToolTest, QaRequestControlsDoNotMutateTheConfiguredModel)
{
	char directory[] = "/tmp/morph-image-control-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	const std::string path = std::string(directory) + "/input.png";
	ASSERT_EQ(create_test_png(path.c_str()), 0);
	struct nb_cancel original{};
	struct nb_cancel first{};
	struct nb_cancel second{};
	struct model fake{};
	fake.api_key[0] = 'x';
	fake.chat_with_image = fake_chat_with_image;
	fake.http.cancel_token = &original;
	fake.handle = &qa;
	ASSERT_EQ(img_qa_init(&reg, &fake, nullptr), 0);
	const std::string arguments = "{\"file_path\":\"" + path + "\"}";
	struct tool_runtime_context runtime{};
	struct tool_result result;
	tool_result_init(&result);
	result.runtime = &runtime;
	runtime.http.cancel_token = &first;
	ASSERT_EQ(tool_exec(&reg, "img_qa", arguments.c_str(), &result), 0);
	EXPECT_EQ(qa.cancel_token, &first);
	EXPECT_EQ(fake.http.cancel_token, &original);
	runtime.http.cancel_token = &second;
	ASSERT_EQ(tool_exec(&reg, "img_qa", arguments.c_str(), &result), 0);
	EXPECT_EQ(qa.cancel_token, &second);
	EXPECT_EQ(fake.http.cancel_token, &original);
	tool_result_cleanup(&result);
	std::filesystem::remove_all(directory);
}

TEST_F(ImgGenToolTest, ResizeAndConvertShareAuthorizedDecodeFailures)
{
	char directory[] = "/tmp/morph-image-input-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	const std::string broken = std::string(directory) + "/broken.png";
	const std::string missing = std::string(directory) + "/missing.png";
	const std::string output = std::string(directory) + "/output.png";
	ASSERT_EQ(nb_file_write_all(broken.c_str(), "not an image", 12, NULL), 0);
	struct tool_context *context = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(context, nullptr);
	ASSERT_EQ(img_resize_init(&reg, context), 0);
	ASSERT_EQ(img_convert_init(&reg, context), 0);
	for (const auto &path : {missing, broken}) {
		const int expected = path == missing ? NB_ERR_NOT_FOUND : NB_ERR_FORMAT;
		for (const char *name : {"img_resize", "img_convert"}) {
			SCOPED_TRACE(name);
			const std::string arguments =
				"{\"file_path\":\"" + path +
				"\","
				"\"width\":4,\"format\":\"png\",\"output_path\":\"" +
				output + "\"}";
			struct tool_result result;
			tool_result_init(&result);
			EXPECT_EQ(tool_exec(&reg, name, arguments.c_str(), &result), expected);
			ASSERT_NE(result.text.ptr, nullptr);
			EXPECT_NE(strstr(result.text.ptr, "failed to load image"), nullptr);
			EXPECT_FALSE(nb_file_exists(output.c_str()));
			tool_result_cleanup(&result);
		}
	}
	tool_context_destroy(context);
	std::filesystem::remove_all(directory);
}

TEST_F(ImgGenToolTest, ResizeThenConvertPreservesAuthorizedImageContent)
{
	char directory[] = "/tmp/morph-image-transform-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	const std::string original = std::string(directory) + "/original.png";
	const std::string resized = std::string(directory) + "/resized.png";
	const std::string converted = std::string(directory) + "/converted.jpg";
	ASSERT_EQ(create_solid_png(original.c_str(), 8, 4), 0);
	struct tool_context *context = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(context, nullptr);
	ASSERT_EQ(img_resize_init(&reg, context), 0);
	ASSERT_EQ(img_convert_init(&reg, context), 0);
	const std::string resize_args = "{\"file_path\":\"" + original +
					"\",\"width\":4,\"output_path\":\"" + resized + "\"}";
	const std::string convert_args = "{\"file_path\":\"" + resized +
					 "\",\"format\":\"jpg\",\"output_path\":\"" + converted +
					 "\"}";
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "img_resize", resize_args.c_str(), &result), 0);
	ASSERT_EQ(tool_exec(&reg, "img_convert", convert_args.c_str(), &result), 0);
	tool_result_cleanup(&result);
	int width = 0, height = 0, channels = 0;
	unsigned char *pixels = stbi_load(converted.c_str(), &width, &height, &channels, 3);
	ASSERT_NE(pixels, nullptr);
	EXPECT_EQ(width, 4);
	EXPECT_EQ(height, 2);
	EXPECT_NEAR(pixels[0], 0x40, 4);
	EXPECT_NEAR(pixels[1], 0x80, 4);
	EXPECT_NEAR(pixels[2], 0xC0, 4);
	stbi_image_free(pixels);
	tool_context_destroy(context);
	std::filesystem::remove_all(directory);
}

TEST_F(ImgGenToolTest, DefaultTransformsPublishDistinctCompleteImages)
{
	char directory[] = "/tmp/morph-image-unique-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	const std::string input = std::string(directory) + "/input.png";
	ASSERT_EQ(create_solid_png(input.c_str(), 8, 4), 0);
	struct tool_context *context = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(context, nullptr);
	ASSERT_EQ(img_resize_init(&reg, context), 0);
	ASSERT_EQ(img_convert_init(&reg, context), 0);
	std::set<std::string> paths;
	for (const char *name : {"img_resize", "img_convert"}) {
		for (int attempt = 0; attempt < 3; attempt++) {
			struct tool_result result;
			tool_result_init(&result);
			const std::string args =
				"{\"file_path\":\"" + input + "\",\"width\":4,\"format\":\"png\"}";
			ASSERT_EQ(tool_exec(&reg, name, args.c_str(), &result), 0);
			ASSERT_EQ(result.artifacts.count, 1);
			const auto &artifact = result.artifacts.items[0];
			EXPECT_TRUE(paths.insert(artifact.path).second);
			EXPECT_EQ(std::filesystem::path(artifact.path).parent_path(),
				  std::filesystem::canonical(directory));
			int width = 0, height = 0, channels = 0;
			unsigned char *pixels =
				stbi_load(artifact.path, &width, &height, &channels, 4);
			ASSERT_NE(pixels, nullptr);
			EXPECT_EQ(width, strcmp(name, "img_resize") == 0 ? 4 : 8);
			EXPECT_EQ(height, strcmp(name, "img_resize") == 0 ? 2 : 4);
			EXPECT_EQ(pixels[0], 0x40);
			EXPECT_EQ(pixels[1], 0x80);
			EXPECT_EQ(pixels[2], 0xC0);
			stbi_image_free(pixels);
			tool_result_cleanup(&result);
		}
	}
	EXPECT_EQ(paths.size(), 6u);
	for (const auto &entry : std::filesystem::directory_iterator(directory))
		EXPECT_EQ(entry.path().filename().string().find(".morph"), std::string::npos);
	tool_context_destroy(context);
	std::filesystem::remove_all(directory);
}

TEST_F(ImgGenToolTest, ExplicitTransformsKeepTheirOwnFormatSelection)
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_2 NB_CLEANUP(nb_charp_cleanup) = NULL;
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_3 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char directory[] = "/tmp/morph-image-explicit-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	const std::string input = std::string(directory) + "/input.png";
	ASSERT_EQ(create_solid_png(input.c_str(), 8, 4), 0);
	struct tool_context *context = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(context, nullptr);
	ASSERT_EQ(img_resize_init(&reg, context), 0);
	ASSERT_EQ(img_convert_init(&reg, context), 0);
	struct {
		const char *tool;
		const char *filename;
		const char *format;
		const char *magic;
		size_t magic_length;
	} cases[] = {
		{"img_resize", "resized.jpg", "png", "\xff\xd8", 2},
		{"img_resize", "resized.bmp", "png", "BM", 2},
		{"img_resize", "resized.tga", "png", "", 0},
		{"img_convert", "converted.png", "bmp", "BM", 2},
		{"img_convert", "converted.tga", "tga", "", 0},
	};
	for (const auto &item : cases) {
		SCOPED_TRACE(item.filename);
		const std::string output = std::string(directory) + "/" + item.filename;
		const std::string args = "{\"file_path\":\"" + input +
					 "\",\"width\":4,\"format\":\"" + item.format +
					 "\",\"output_path\":\"" + output + "\"}";
		struct tool_result result;
		tool_result_init(&result);
		ASSERT_EQ(tool_exec(&reg, item.tool, args.c_str(), &result), 0);
		ASSERT_EQ(result.artifacts.count, 1);
		EXPECT_TRUE(std::filesystem::equivalent(result.artifacts.items[0].path, output));
		size_t bytes = 0;
		char *encoded =
			(nb_file_read_all(output.c_str(), &file_value_2, 0, NULL) == NB_OK &&
					 (file_copy_2 = (char *)malloc(nb_buf_len(&file_value_2) +
								       1)) != NULL
				 ? (memcpy(file_copy_2, nb_buf_cstr(&file_value_2),
					   nb_buf_len(&file_value_2) + 1),
				    *(&bytes) = nb_buf_len(&file_value_2),
				    nb_charp_steal(&file_copy_2))
				 : NULL);
		ASSERT_NE(encoded, nullptr);
		ASSERT_GE(bytes, item.magic_length);
		EXPECT_EQ(memcmp(encoded, item.magic, item.magic_length), 0);
		free(encoded);
		int width = 0, height = 0, channels = 0;
		EXPECT_NE(stbi_info(output.c_str(), &width, &height, &channels), 0);
		EXPECT_EQ(width, strcmp(item.tool, "img_resize") == 0 ? 4 : 8);
		EXPECT_EQ(height, strcmp(item.tool, "img_resize") == 0 ? 2 : 4);
		tool_result_cleanup(&result);
	}
	const std::string target = std::string(directory) + "/canonical.png";
	const std::string alias = std::string(directory) + "/alias.jpg";
	ASSERT_EQ(nb_file_write_all(target.c_str(), "old", 3, NULL), 0);
	ASSERT_EQ(symlink("canonical.png", alias.c_str()), 0);
	const std::string args =
		"{\"file_path\":\"" + input + "\",\"width\":4,\"output_path\":\"" + alias + "\"}";
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&reg, "img_resize", args.c_str(), &result), 0);
	EXPECT_TRUE(std::filesystem::is_symlink(alias));
	char *encoded =
		(nb_file_read_all(target.c_str(), &file_value_3, 0, NULL) == NB_OK &&
				 (file_copy_3 = (char *)malloc(nb_buf_len(&file_value_3) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_3, nb_buf_cstr(&file_value_3),
				   nb_buf_len(&file_value_3) + 1),
			    nb_charp_steal(&file_copy_3))
			 : NULL);
	ASSERT_NE(encoded, nullptr);
	EXPECT_EQ(memcmp(encoded, "\x89PNG", 4), 0);
	free(encoded);
	tool_result_cleanup(&result);
	tool_context_destroy(context);
	std::filesystem::remove_all(directory);
}

TEST_F(ImgGenToolTest, TransformOutputFailuresLeaveNoArtifactOrTemporaryFiles)
{
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_4 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char directory[] = "/tmp/morph-image-output-failure-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	const std::string input = std::string(directory) + "/input.png";
	const std::string blocked = std::string(directory) + "/blocked";
	const std::string marker = blocked + "/marker.txt";
	ASSERT_EQ(create_solid_png(input.c_str(), 8, 4), 0);
	ASSERT_EQ(nb_file_mkdir(blocked.c_str(), true, NULL), 0);
	ASSERT_EQ(nb_file_write_all(marker.c_str(), "unchanged", 9, NULL), 0);
	struct tool_context *context = tool_context_create(nullptr, directory, directory);
	ASSERT_NE(context, nullptr);
	ASSERT_EQ(img_resize_init(&reg, context), 0);
	ASSERT_EQ(img_convert_init(&reg, context), 0);
	for (const char *name : {"img_resize", "img_convert"}) {
		const std::string args = "{\"file_path\":\"" + input +
					 "\",\"width\":4,\"format\":\"png\",\"output_path\":\"" +
					 blocked + "\"}";
		struct tool_result result;
		tool_result_init(&result);
		EXPECT_LT(tool_exec(&reg, name, args.c_str(), &result), 0);
		EXPECT_EQ(result.artifacts.count, 0);
		ASSERT_NE(result.text.ptr, nullptr);
		EXPECT_NE(strstr(result.text.ptr, "failed to write output"), nullptr);
		tool_result_cleanup(&result);
	}
	char *contents =
		(nb_file_read_all(marker.c_str(), &file_value_4, 0, NULL) == NB_OK &&
				 (file_copy_4 = (char *)malloc(nb_buf_len(&file_value_4) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_4, nb_buf_cstr(&file_value_4),
				   nb_buf_len(&file_value_4) + 1),
			    nb_charp_steal(&file_copy_4))
			 : NULL);
	ASSERT_NE(contents, nullptr);
	EXPECT_STREQ(contents, "unchanged");
	free(contents);
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
				std::filesystem::directory_iterator()),
		  2);
	EXPECT_EQ(std::distance(std::filesystem::directory_iterator(blocked),
				std::filesystem::directory_iterator()),
		  1);
	tool_context_destroy(context);
	std::filesystem::remove_all(directory);
}

TEST_F(ImgGenToolTest, ConvertRejectsUnboundedQualityBeforeWriting)
{
	nb_buf file_value_5 NB_CLEANUP(nb_buf_destroy) = {};
	char *file_copy_5 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char directory[] = "/tmp/morph-image-quality-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	const std::string input = std::string(directory) + "/input.png";
	const std::string output = std::string(directory) + "/existing.jpg";
	ASSERT_EQ(create_solid_png(input.c_str(), 8, 4), 0);
	ASSERT_EQ(nb_file_write_all(output.c_str(), "unchanged", 9, NULL), 0);
	ASSERT_EQ(img_convert_init(&reg, nullptr), 0);
	for (const char *quality : {"1e400", "1e100", "-1", "101"}) {
		SCOPED_TRACE(quality);
		const std::string args = "{\"file_path\":\"" + input +
					 "\",\"format\":\"jpg\",\"output_path\":\"" + output +
					 "\",\"quality\":" + quality + "}";
		struct tool_result result;
		tool_result_init(&result);
		EXPECT_EQ(tool_exec(&reg, "img_convert", args.c_str(), &result), NB_ERR_INVAL);
		EXPECT_EQ(result.artifacts.count, 0);
		tool_result_cleanup(&result);
	}
	char *contents =
		(nb_file_read_all(output.c_str(), &file_value_5, 0, NULL) == NB_OK &&
				 (file_copy_5 = (char *)malloc(nb_buf_len(&file_value_5) + 1)) !=
					 NULL
			 ? (memcpy(file_copy_5, nb_buf_cstr(&file_value_5),
				   nb_buf_len(&file_value_5) + 1),
			    nb_charp_steal(&file_copy_5))
			 : NULL);
	ASSERT_NE(contents, nullptr);
	EXPECT_STREQ(contents, "unchanged");
	free(contents);
	std::filesystem::remove_all(directory);
}

TEST(ImageEncode, CopiesSurviveSourceRemovalAndPreserveOccupiedOwners)
{
	for (int max_dim : {64, 4}) {
		char path[] = "/tmp/morph-image-owner-XXXXXX";
		int fd = mkstemp(path);
		ASSERT_GE(fd, 0);
		ASSERT_EQ(close(fd), 0);
		ASSERT_EQ(create_solid_png(path, 8, 4), 0);
		struct image_encoded encoded NB_CLEANUP(image_encoded_cleanup) = {};
		char *uri NB_CLEANUP(nb_charp_cleanup) = nullptr;
		ASSERT_EQ(image_encode_base64_copy(path, max_dim, &encoded), 0);
		ASSERT_EQ(image_encode_data_uri_copy(path, max_dim, &uri), 0);
		char *base64_owner = encoded.base64;
		const char *mime_owner = encoded.mime_type;
		char *uri_owner = uri;
		ASSERT_EQ(unlink(path), 0);
		EXPECT_EQ(image_encode_base64_copy(path, max_dim, &encoded), NB_ERR_STATE);
		EXPECT_EQ(encoded.base64, base64_owner);
		EXPECT_EQ(encoded.mime_type, mime_owner);
		EXPECT_EQ(image_encode_data_uri_copy(path, max_dim, &uri), NB_ERR_STATE);
		EXPECT_EQ(uri, uri_owner);
		EXPECT_STREQ(encoded.mime_type, "image/png");
		EXPECT_EQ(std::string(uri), std::string("data:image/png;base64,") + encoded.base64);
		size_t length = 0;
		unsigned char *decoded = test_decode_bytes(encoded.base64, &length);
		ASSERT_NE(decoded, nullptr);
		int width = 0;
		int height = 0;
		int channels = 0;
		EXPECT_TRUE(stbi_info_from_memory(decoded, static_cast<int>(length), &width,
						  &height, &channels));
		EXPECT_EQ(width, max_dim == 64 ? 8 : 4);
		EXPECT_EQ(height, max_dim == 64 ? 4 : 2);
		free(decoded);
	}
}

TEST(ImageEncode, InvalidSourceLeavesBothOutputOwnersEmpty)
{
	char path[] = "/tmp/morph-image-invalid-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, "invalid", 7), 7);
	ASSERT_EQ(close(fd), 0);
	struct image_encoded encoded NB_CLEANUP(image_encoded_cleanup) = {};
	char *uri NB_CLEANUP(nb_charp_cleanup) = nullptr;
	EXPECT_EQ(image_encode_base64_copy(path, 64, &encoded), NB_ERR_FORMAT);
	EXPECT_EQ(encoded.base64, nullptr);
	EXPECT_EQ(encoded.mime_type, nullptr);
	EXPECT_EQ(image_encode_data_uri_copy(path, 64, &uri), NB_ERR_FORMAT);
	EXPECT_EQ(uri, nullptr);
	ASSERT_EQ(unlink(path), 0);
}

TEST(ImageEncode, SharedInputOwnerRejectsReplacementAndClearsAfterRelease)
{
	char path[] = "/tmp/morph-image-input-owner-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	ASSERT_EQ(create_solid_png(path, 8, 4), 0);
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	struct tool_image_input image NB_CLEANUP(tool_image_input_cleanup) = {};
	ASSERT_EQ(tool_image_load_input(nullptr, path, &result, &image), 0);
	ASSERT_NE(image.pixels, nullptr);
	auto *owner = image.pixels;
	ASSERT_EQ(unlink(path), 0);
	EXPECT_EQ(tool_image_load_input(nullptr, path, &result, &image), NB_ERR_STATE);
	EXPECT_EQ(image.pixels, owner);
	EXPECT_EQ(image.width, 8);
	EXPECT_EQ(image.height, 4);
	tool_image_input_cleanup(&image);
	EXPECT_EQ(image.pixels, nullptr);
	EXPECT_EQ(image.width, 0);
	EXPECT_LT(tool_image_load_input(nullptr, path, &result, &image), 0);
	EXPECT_EQ(image.pixels, nullptr);
	EXPECT_EQ(image.width, 0);
}
