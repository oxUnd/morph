#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/widgets/rich_document.h"
#include "util/base64.h"
#include "util/utf8.h"
#include "morph_kitty_protocol.h"
#include <morph/tui/application.h>
#include <morph/tui/view_controller.h>
#include <morph/tui/window.h>
#include <morph/tui/widgets/stack.h>
}

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

static int retained_media_placeholder_write(const char *bytes, size_t length, void *data)
{
	static_cast<std::string *>(data)->append(bytes, length);
	return 0;
}

static std::string retained_media_placeholders(unsigned id, unsigned row, unsigned columns)
{
	std::string bytes;
	int rc = morph_kitty_write_placeholder_row(retained_media_placeholder_write,
		&bytes, id, row, columns);
	EXPECT_EQ(rc, 0);
	return bytes;
}

static std::string retained_media_transfer(unsigned id, unsigned format, int columns, int rows,
	const unsigned char *bytes, size_t length, int width = 0, int height = 0)
{
	char *encoded = base64_encode_copy(bytes, length);
	if (!encoded)
		return "";
	std::string result = "\033_Ga=T,f=" + std::to_string(format) + ",i=" +
		std::to_string(id) + ",U=1,q=2,c=" + std::to_string(columns) +
		",r=" + std::to_string(rows);
	if (format == 32)
		result += ",s=" + std::to_string(width) + ",v=" + std::to_string(height);
	result += ";";
	result += encoded;
	result += "\033\\";
	free(encoded);
	return result;
}

class CliRetainedMediaTest : public ::testing::Test
{
      protected:
	struct Placement {
		int x, y, width, height, source_x, source_y, source_width, source_height;
		uint64_t key;
		uintptr_t identity;
	};
	int master = -1;
	FILE *input = nullptr, *output = nullptr;
	struct morph_tui_application *app = nullptr;
	struct morph_tui_view *document = nullptr;
	std::string source, frame, plain;
	std::vector<Placement> placements;
	struct morph_tui_renderer renderer{};
	const struct morph_tui_renderer_ops renderer_ops = {
		+[](struct morph_tui_renderer *backend, struct morph_tui_surface *surface, FILE *) {
			auto *self = static_cast<CliRetainedMediaTest *>(backend->data);
			morph_buf_t rows{};
			int rc = morph_tui_surface_rows(surface, &rows);
			if (!rc) {
				self->frame = morph_buf_cstr_borrow(&rows);
				char *plain = utf8_strip_ansi_dup(rows.data, rows.len, nullptr);
				if (plain)
					self->plain = plain;
				free(plain);
			}
			morph_buf_cleanup(&rows);
			self->placements.clear();
			for (size_t i = 0; i < morph_tui_surface_image_count(surface); i++) {
				const auto *image = morph_tui_surface_image_borrow(surface, i);
				self->placements.push_back({image->x, image->y,
					image->width, image->height, image->source_x, image->source_y,
					image->source_width, image->source_height,
					morph_tui_media_asset_key(image->asset),
					reinterpret_cast<uintptr_t>(image->asset)});
			}
			return rc;
		}, nullptr};
	const struct morph_tui_view_controller_ops ops = {
		+[](struct morph_tui_view_controller *controller, struct morph_tui_builder *builder) {
			auto *self = static_cast<CliRetainedMediaTest *>(controller->data);
			auto *root = morph_tui_widget_stack_add_borrow(builder, nullptr,
				MORPH_TUI_WIDGET_STACK_VERTICAL, 0, nullptr);
			self->document = cli_rich_document_add_borrow(builder, root);
			morph_tui_builder_fail(builder, cli_rich_document_set_rendered_copy(
				self->document, self->source.c_str(), 1));
		}, nullptr, nullptr, nullptr, nullptr, nullptr};

	void SetUp() override
	{
		master = posix_openpt(O_RDWR | O_NOCTTY);
		ASSERT_GE(master, 0);
		ASSERT_EQ(grantpt(master), 0);
		ASSERT_EQ(unlockpt(master), 0);
		ASSERT_NE(input = fopen(ptsname(master), "r+"), nullptr);
		ASSERT_NE(output = fdopen(dup(fileno(input)), "w"), nullptr);
		struct winsize size{};
		size.ws_col = 80;
		size.ws_row = 12;
		ASSERT_EQ(ioctl(fileno(input), TIOCSWINSZ, &size), 0);
		struct morph_tui_application_options options{};
		options.input = input;
		options.output = output;
		options.external_signals = 1;
		ASSERT_EQ(morph_tui_application_create(&options, &app), 0);
		renderer.ops = &renderer_ops;
		renderer.data = this;
		ASSERT_EQ(morph_tui_window_set_renderer_borrow(
			morph_tui_application_window_borrow(app), &renderer), 0);
	}

	void TearDown() override
	{
		morph_tui_application_destroy(app);
		if (input)
			fclose(input);
		if (output)
			fclose(output);
		if (master >= 0)
			close(master);
	}

	void Present()
	{
		struct morph_tui_view_controller_spec spec{};
		spec.ops = &ops;
		spec.data = this;
		ASSERT_EQ(morph_tui_view_controller_present_borrow(app, &spec), 0);
		ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	}

	std::string PngTransfer(unsigned id, int columns = 3, int rows = 1)
	{
		std::vector<unsigned char> pixels(4u * 2u * 4u, 255u);
		struct morph_tui_media_asset *asset = nullptr;
		int rc = morph_tui_media_asset_create_rgba_copy(&asset, pixels.data(), 4, 2);
		EXPECT_EQ(rc, 0);
		morph_buf_t png{};
		rc = morph_tui_media_asset_png_append(asset, 0, 0, 4, 2, &png);
		EXPECT_EQ(rc, 0);
		std::string result = retained_media_transfer(id, 100, columns, rows,
			reinterpret_cast<const unsigned char *>(png.data), png.len);
		morph_buf_cleanup(&png);
		morph_tui_media_asset_destroy(asset);
		return result;
	}
};

TEST_F(CliRetainedMediaTest, SdkPngPlaceholdersKeepInlinePositionAndCacheAcrossNewTransferIds)
{
	const unsigned first = 0x53123456u, second = 0x53654321u;
	source = "before " + PngTransfer(first) + retained_media_placeholders(first, 0, 3) +
		" after\n";
	Present();
	ASSERT_EQ(placements.size(), 1u);
	EXPECT_EQ(placements[0].x, 7);
	EXPECT_EQ(placements[0].y, 0);
	EXPECT_EQ(placements[0].width, 3);
	EXPECT_EQ(placements[0].height, 1);
	EXPECT_EQ(placements[0].source_width, 4);
	EXPECT_EQ(placements[0].source_height, 2);
	EXPECT_EQ(plain.find("before     after"), 0u);
	EXPECT_EQ(frame.find("\033_G"), std::string::npos);
	EXPECT_EQ(frame.find("\xF4\x8E\xBB\xAE"), std::string::npos);
	const auto original = placements[0];
	source = "before " + PngTransfer(second) + retained_media_placeholders(second, 0, 3) +
		" after\n";
	ASSERT_EQ(cli_rich_document_set_rendered_copy(document, source.c_str(), 1), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	ASSERT_EQ(placements.size(), 1u);
	EXPECT_EQ(placements[0].key, original.key);
	EXPECT_EQ(placements[0].identity, original.identity);
}

TEST_F(CliRetainedMediaTest, RgbaMathInTableKeepsCellPositionAndRejectsTruncatedPixels)
{
	const unsigned id = 0x53445566u;
	const unsigned char pixels[] = {255, 0, 0, 255, 0, 255, 0, 255};
	source = "┌──────┬──────┐\n│ math │ " +
		retained_media_transfer(id, 32, 2, 1, pixels, sizeof(pixels), 2, 1) +
		retained_media_placeholders(id, 0, 2) + "   │\n└──────┴──────┘\n";
	Present();
	ASSERT_EQ(placements.size(), 1u);
	EXPECT_EQ(placements[0].x, 9);
	EXPECT_EQ(placements[0].y, 1);
	EXPECT_EQ(placements[0].width, 2);
	EXPECT_EQ(placements[0].height, 1);
	EXPECT_NE(plain.find("│ math │      │"), std::string::npos);
	const std::string previous = frame;
	std::string invalid = retained_media_transfer(id, 32, 2, 1,
		pixels, sizeof(pixels), 2, 2) + retained_media_placeholders(id, 0, 2);
	EXPECT_LT(cli_rich_document_set_rendered_copy(document, invalid.c_str(), 1), 0);
	EXPECT_EQ(frame, previous);
	EXPECT_EQ(placements.size(), 1u);
}

TEST_F(CliRetainedMediaTest, MediaClipsAtDocumentBoundaryAndUnsafeControlsNeverEnterSurfaceRows)
{
	const unsigned id = 0x53556677u;
	source = std::string(78, 'x') + PngTransfer(id, 4, 1) +
		retained_media_placeholders(id, 0, 4) + "\n";
	Present();
	ASSERT_EQ(placements.size(), 1u);
	EXPECT_EQ(placements[0].x, 78);
	EXPECT_EQ(placements[0].width, 2);
	EXPECT_EQ(placements[0].source_width, 2);
	ASSERT_EQ(cli_rich_document_set_rendered_copy(document,
		"safe\033]52;c;private\a\033[2J\033[Htext", 1), 0);
	ASSERT_EQ(morph_tui_application_step(app, 0), 0);
	EXPECT_EQ(placements.size(), 0u);
	EXPECT_EQ(plain.find("safetext"), 0u);
	EXPECT_EQ(frame.find("private"), std::string::npos);
	EXPECT_EQ(frame.find("\033]52"), std::string::npos);
	EXPECT_EQ(frame.find("\033[2J"), std::string::npos);
	EXPECT_EQ(frame.find("\033[H"), std::string::npos);
}

TEST_F(CliRetainedMediaTest, PlainStyledDocumentKeepsTextAndNoNativePlacements)
{
	source = "plain\n\n\033[1mbold\033[0m and 中文🙂\n";
	Present();
	EXPECT_EQ(placements.size(), 0u);
	EXPECT_EQ(plain.find("plain"), 0u);
	EXPECT_NE(plain.find("bold and 中文🙂"), std::string::npos);
	EXPECT_NE(frame.find("\033[1m"), std::string::npos);
}
