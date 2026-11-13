#include <gtest/gtest.h>

extern "C" {
#include <morph/tui/media.h>
#include <morph/tui/canvas.h>
#include "tui/render/native_media.h"
#include "tui/render/frame_internal.h"
#include "util/base64.h"
}

#include <vector>
#include <cstdlib>
#include <string>
#include <utility>

TEST(TuiMediaTest, PngRoundTripAndCropPreservePixelsAndStableContentKey)
{
	std::vector<unsigned char> pixels(4u * 4u * 4u, 255u);
	for (size_t y = 0; y < 4; y++) {
		for (size_t x = 0; x < 4; x++) {
			pixels[(y * 4 + x) * 4] = x < 2 ? 255u : 0u;
			pixels[(y * 4 + x) * 4 + 1] = x < 2 ? 0u : 255u;
			pixels[(y * 4 + x) * 4 + 2] = 0;
		}
	}
	struct morph_tui_media_asset *asset = nullptr, *decoded = nullptr, *crop = nullptr;
	ASSERT_EQ(morph_tui_media_asset_create_rgba_copy(&asset, pixels.data(), 4, 4), 0);
	morph_buf_t png{}, cropped{};
	ASSERT_EQ(morph_tui_media_asset_png_append(asset, 0, 0, 4, 4, &png), 0);
	ASSERT_EQ(morph_tui_media_asset_create_png_copy(&decoded,
		reinterpret_cast<const unsigned char *>(png.data), png.len), 0);
	EXPECT_EQ(morph_tui_media_asset_key(asset), morph_tui_media_asset_key(decoded));
	EXPECT_EQ(morph_tui_media_asset_width(decoded), 4);
	EXPECT_EQ(morph_tui_media_asset_height(decoded), 4);
	ASSERT_EQ(morph_tui_media_asset_png_append(asset, 1, 1, 2, 2, &cropped), 0);
	ASSERT_EQ(morph_tui_media_asset_create_png_copy(&crop,
		reinterpret_cast<const unsigned char *>(cropped.data), cropped.len), 0);
	EXPECT_EQ(morph_tui_media_asset_width(crop), 2);
	EXPECT_EQ(morph_tui_media_asset_height(crop), 2);
	const unsigned char *result = morph_tui_media_asset_pixels_borrow(crop);
	EXPECT_EQ(result[0], 255);
	EXPECT_EQ(result[1], 0);
	EXPECT_EQ(result[4], 0);
	EXPECT_EQ(result[5], 255);
	EXPECT_EQ(result[8], 255);
	EXPECT_EQ(result[9], 0);
	morph_tui_media_asset_destroy(crop);
	morph_tui_media_asset_destroy(decoded);
	morph_tui_media_asset_destroy(asset);
	morph_buf_cleanup(&png);
	morph_buf_cleanup(&cropped);
}

TEST(TuiMediaTest, InvalidAssetsAndCropsPreserveOwners)
{
	struct morph_tui_media_asset *asset = nullptr;
	const unsigned char pixel[] = {1, 2, 3, 255};
	EXPECT_LT(morph_tui_media_asset_create_rgba_copy(&asset, pixel, 0, 1), 0);
	EXPECT_EQ(asset, nullptr);
	EXPECT_LT(morph_tui_media_asset_create_rgba_copy(&asset, pixel, 100000, 100000), 0);
	EXPECT_EQ(asset, nullptr);
	EXPECT_LT(morph_tui_media_asset_create_png_copy(&asset, pixel, sizeof(pixel)), 0);
	EXPECT_EQ(asset, nullptr);
	ASSERT_EQ(morph_tui_media_asset_create_rgba_copy(&asset, pixel, 1, 1), 0);
	auto *original = asset;
	EXPECT_LT(morph_tui_media_asset_create_rgba_copy(&asset, pixel, 1, 1), 0);
	EXPECT_EQ(asset, original);
	morph_buf_t png{};
	EXPECT_LT(morph_tui_media_asset_png_append(asset, 1, 0, 1, 1, &png), 0);
	EXPECT_EQ(png.len, 0u);
	EXPECT_LT(morph_tui_media_asset_png_append(asset, -1, 0, 1, 1, &png), 0);
	EXPECT_EQ(png.len, 0u);
	morph_tui_media_asset_destroy(asset);
	morph_buf_cleanup(&png);
}

TEST(TuiMediaTest, CanvasClipsImagesToAncestorContentAndRetainsTheirAssets)
{
	std::vector<unsigned char> pixels(80u * 60u * 4u, 255u);
	struct morph_tui_media_asset *asset = nullptr;
	ASSERT_EQ(morph_tui_media_asset_create_rgba_copy(&asset, pixels.data(), 80, 60), 0);
	struct morph_tui_surface *surface = nullptr;
	ASSERT_EQ(morph_tui_surface_create(&surface), 0);
	ASSERT_EQ(morph_tui_surface_resize(surface, 10, 6), 0);
	struct morph_tui_canvas root{}, viewport{};
	morph_tui_canvas_root_borrow(surface, &root);
	morph_tui_canvas_child_borrow(&root, 2, 1, 6, 4, &viewport);
	ASSERT_EQ(morph_tui_canvas_image(&viewport, asset, -2, -1, 8, 6), 0);
	ASSERT_EQ(morph_tui_surface_image_count(surface), 1u);
	const auto *placement = morph_tui_surface_image_borrow(surface, 0);
	ASSERT_NE(placement, nullptr);
	EXPECT_EQ(placement->x, 2);
	EXPECT_EQ(placement->y, 1);
	EXPECT_EQ(placement->width, 6);
	EXPECT_EQ(placement->height, 4);
	EXPECT_EQ(placement->source_x, 20);
	EXPECT_EQ(placement->source_y, 10);
	EXPECT_EQ(placement->source_width, 60);
	EXPECT_EQ(placement->source_height, 40);
	ASSERT_EQ(morph_tui_canvas_image(&viewport, asset, 50, 50, 8, 6), 0);
	EXPECT_EQ(morph_tui_surface_image_count(surface), 1u);
	EXPECT_EQ(morph_tui_surface_image_borrow(surface, 1), nullptr);
	morph_tui_media_asset_destroy(asset);
	EXPECT_EQ(morph_tui_media_asset_width(placement->asset), 80);
	morph_tui_surface_clear(surface);
	EXPECT_EQ(morph_tui_surface_image_count(surface), 0u);
	morph_tui_surface_destroy(surface);
}

class TuiNativeMediaTest : public ::testing::Test
{
      protected:
	struct SavedEnvironment {
		std::string key, value;
		bool present;
	};
	std::vector<SavedEnvironment> environment;
	struct morph_tui_media_asset *asset = nullptr;
	struct morph_tui_surface *surface = nullptr;
	struct render_native_media *media = nullptr;
	struct morph_tui_frame frame{};
	FILE *output = nullptr;

	void SetUp() override
	{
		for (const char *key : {"TERM", "KITTY_WINDOW_ID", "ITERM_PROFILE",
				       "TERM_PROGRAM"}) {
			const char *value = getenv(key);
			environment.push_back({key, value ? value : "", value != nullptr});
			ASSERT_EQ(unsetenv(key), 0);
		}
		std::vector<unsigned char> pixels(80u * 60u * 4u, 255u);
		ASSERT_EQ(morph_tui_media_asset_create_rgba_copy(&asset, pixels.data(), 80, 60), 0);
		ASSERT_EQ(morph_tui_surface_create(&surface), 0);
		ASSERT_EQ(morph_tui_surface_resize(surface, 10, 6), 0);
		struct morph_tui_canvas root{}, viewport{};
		morph_tui_canvas_root_borrow(surface, &root);
		morph_tui_canvas_child_borrow(&root, 2, 1, 6, 4, &viewport);
		ASSERT_EQ(morph_tui_canvas_image(&viewport, asset, -2, -1, 8, 6), 0);
		ASSERT_NE(output = tmpfile(), nullptr);
	}

	void TearDown() override
	{
		render_native_media_destroy(media);
		morph_tui_frame_cleanup(&frame);
		morph_tui_surface_destroy(surface);
		morph_tui_media_asset_destroy(asset);
		if (output)
			fclose(output);
		for (const auto &item : environment) {
			if (item.present)
				EXPECT_EQ(setenv(item.key.c_str(), item.value.c_str(), 1), 0);
			else
				EXPECT_EQ(unsetenv(item.key.c_str()), 0);
		}
	}

	void Present()
	{
		ASSERT_EQ(render_native_media_prepare(&media, surface, &frame), 0);
		ASSERT_NE(media, nullptr);
		ASSERT_EQ(render_native_media_present(media, surface, output), 0);
	}

	std::string Read()
	{
		EXPECT_EQ(fflush(output), 0);
		long position = ftell(output);
		EXPECT_GE(position, 0);
		EXPECT_EQ(fseek(output, 0, SEEK_SET), 0);
		std::string bytes(static_cast<size_t>(position), '\0');
		EXPECT_EQ(fread(bytes.data(), 1, bytes.size(), output), bytes.size());
		EXPECT_EQ(fseek(output, position, SEEK_SET), 0);
		return bytes;
	}
};

TEST_F(TuiNativeMediaTest, KittyTransmitsOnceReusesPlacementAndRemovesHiddenImages)
{
	ASSERT_EQ(setenv("TERM", "xterm-kitty", 1), 0);
	EXPECT_EQ(morph_tui_media_protocol_available(), 1);
	Present();
	std::string first = Read();
	EXPECT_NE(first.find("\033_Ga=t,f=100,"), std::string::npos);
	EXPECT_NE(first.find("c=6,r=4,C=1"), std::string::npos);
	EXPECT_NE(first.find("\033[2;3H"), std::string::npos);
	EXPECT_NE(first.find("\0337"), std::string::npos);
	EXPECT_NE(first.find("\0338"), std::string::npos);
	Present();
	std::string second = Read().substr(first.size());
	EXPECT_EQ(second.find("\033_Ga=t,"), std::string::npos);
	EXPECT_NE(second.find("\033_Ga=p,"), std::string::npos);
	frame.columns = 10;
	frame.rows = 6;
	morph_tui_surface_clear(surface);
	ASSERT_EQ(render_native_media_prepare(&media, surface, &frame), 0);
	EXPECT_EQ(frame.columns, 0);
	EXPECT_EQ(frame.rows, 0);
	ASSERT_EQ(render_native_media_present(media, surface, output), 0);
	std::string hidden = Read().substr(first.size() + second.size());
	EXPECT_NE(hidden.find("\033_Ga=d,d=i,"), std::string::npos);
	EXPECT_EQ(hidden.find("\033_Ga=p,"), std::string::npos);
}

TEST_F(TuiNativeMediaTest, ITermUsesCroppedPngAndExactCellPlacement)
{
	ASSERT_EQ(setenv("TERM_PROGRAM", "iTerm.app", 1), 0);
	EXPECT_EQ(morph_tui_media_protocol_available(), 1);
	Present();
	std::string bytes = Read();
	EXPECT_NE(bytes.find("\033[2;3H"), std::string::npos);
	EXPECT_NE(bytes.find("width=6;height=4;preserveAspectRatio=0:"), std::string::npos);
	size_t start = bytes.find("preserveAspectRatio=0:");
	ASSERT_NE(start, std::string::npos);
	start += std::string("preserveAspectRatio=0:").size();
	size_t end = bytes.find('\a', start);
	ASSERT_NE(end, std::string::npos);
	size_t decoded_length = 0;
	unsigned char *decoded = base64_decode_copy(bytes.substr(start, end - start).c_str(),
		&decoded_length);
	ASSERT_NE(decoded, nullptr);
	struct morph_tui_media_asset *cropped = nullptr;
	ASSERT_EQ(morph_tui_media_asset_create_png_copy(&cropped, decoded, decoded_length), 0);
	EXPECT_EQ(morph_tui_media_asset_width(cropped), 60);
	EXPECT_EQ(morph_tui_media_asset_height(cropped), 40);
	morph_tui_media_asset_destroy(cropped);
	free(decoded);
	EXPECT_NE(bytes.find("\0338"), std::string::npos);
}

TEST_F(TuiNativeMediaTest, SixelUsesCellSizedRasterAndRestoresCursor)
{
	ASSERT_EQ(setenv("TERM", "xterm-sixel", 1), 0);
	EXPECT_EQ(morph_tui_media_protocol_available(), 1);
	Present();
	std::string bytes = Read();
	EXPECT_NE(bytes.find("\033[2;3H"), std::string::npos);
	EXPECT_NE(bytes.find("\033P0;1;0q\"1;1;48;64"), std::string::npos);
	EXPECT_NE(bytes.find("#215;2;100;100;100"), std::string::npos);
	EXPECT_NE(bytes.find("\033\\"), std::string::npos);
	EXPECT_NE(bytes.find("\0338"), std::string::npos);
}

TEST_F(TuiNativeMediaTest, SixelResamplingPreservesSolidColorsAndTransparentPixels)
{
	ASSERT_EQ(setenv("TERM", "xterm-sixel", 1), 0);
	morph_tui_surface_clear(surface);
	morph_tui_media_asset_destroy(asset);
	asset = nullptr;
	const unsigned char red[] = {255, 0, 0, 255};
	ASSERT_EQ(morph_tui_media_asset_create_rgba_copy(&asset, red, 1, 1), 0);
	struct morph_tui_canvas canvas{};
	morph_tui_canvas_root_borrow(surface, &canvas);
	ASSERT_EQ(morph_tui_canvas_image(&canvas, asset, 0, 0, 2, 1), 0);
	Present();
	const std::string opaque = Read();
	EXPECT_NE(opaque.find("#180!16~"), std::string::npos);
	morph_tui_surface_clear(surface);
	morph_tui_media_asset_destroy(asset);
	asset = nullptr;
	const unsigned char transparent[] = {255, 0, 0, 0};
	ASSERT_EQ(morph_tui_media_asset_create_rgba_copy(&asset, transparent, 1, 1), 0);
	ASSERT_EQ(morph_tui_canvas_image(&canvas, asset, 0, 0, 2, 1), 0);
	Present();
	const std::string empty = Read().substr(opaque.size());
	EXPECT_NE(empty.find("\033P0;1;0q\"1;1;16;16"), std::string::npos);
	EXPECT_EQ(empty.find("#180!"), std::string::npos);
}

TEST_F(TuiNativeMediaTest, TextAndImagesShareOneAtomicFrameAndUnrelatedRowsSkipOverlayRefresh)
{
	ASSERT_EQ(setenv("TERM", "xterm-kitty", 1), 0);
	struct morph_tui_canvas canvas{};
	morph_tui_canvas_root_borrow(surface, &canvas);
	ASSERT_EQ(morph_tui_canvas_text(&canvas, 0, 0, "ATOMIC", 0), 0);
	ASSERT_EQ(render_native_media_prepare(&media, surface, &frame), 0);
	ASSERT_NE(media, nullptr);
	morph_buf_t rows{}, overlay{};
	ASSERT_EQ(morph_tui_surface_rows(surface, &rows), 0);
	ASSERT_EQ(render_native_media_append(media, surface, output, &overlay), 0);
	EXPECT_EQ(Read(), "");
	EXPECT_EQ(std::string(morph_buf_cstr_borrow(&overlay)).find("\033[?2026"),
		std::string::npos);
	ASSERT_EQ(render_frame_present_composed(&frame, output, 10, 6,
		morph_buf_cstr_borrow(&rows), &overlay), 0);
	std::string bytes = Read();
	size_t start = bytes.find("\033[?2026h"), end = bytes.find("\033[?2026l");
	ASSERT_NE(start, std::string::npos);
	ASSERT_NE(end, std::string::npos);
	EXPECT_EQ(start, bytes.rfind("\033[?2026h"));
	EXPECT_EQ(end, bytes.rfind("\033[?2026l"));
	EXPECT_LT(start, bytes.find("ATOMIC"));
	EXPECT_LT(bytes.find("ATOMIC"), bytes.find("\033_Ga=t,"));
	EXPECT_LT(bytes.find("\033_Ga=p,"), end);
	std::string unrelated = morph_buf_cstr_borrow(&rows);
	unrelated[unrelated.find("ATOMIC")] = 'B';
	EXPECT_EQ(render_native_media_needs_refresh(media, surface, &frame, unrelated.c_str()), 0);
	std::string overlaps = morph_buf_cstr_borrow(&rows);
	size_t row_two = overlaps.find('\n', overlaps.find('\n') + 1) + 1;
	size_t cell = overlaps.find(' ', row_two);
	ASSERT_NE(cell, std::string::npos);
	overlaps[cell] = 'E';
	EXPECT_EQ(render_native_media_needs_refresh(media, surface, &frame, overlaps.c_str()), 1);
	morph_buf_cleanup(&rows);
	morph_buf_cleanup(&overlay);
}
