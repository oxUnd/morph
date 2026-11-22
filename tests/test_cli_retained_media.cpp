#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/components/rich_document.h"
#include "sapi/cli/presentation/markdown.h"
#include "sapi/cli/application.h"
#include "stb_image_write.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/media.h>
#include "encoding_fixture.h"
#include <nb/utf8.h>
#include <nothing/tui/terminal/text.h>
#include "morph_kitty_protocol.h"
#include <nothing/tui/application.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/window.h>
#include <nothing/tui/views/stack_view.h>
}

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <optional>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>

static int retained_media_placeholder_write(const char *bytes, size_t length, void *data)
{
	static_cast<std::string *>(data)->append(bytes, length);
	return 0;
}

static std::string retained_media_placeholders(unsigned id, unsigned row, unsigned columns)
{
	std::string bytes;
	int rc = morph_kitty_write_placeholder_row(retained_media_placeholder_write, &bytes, id,
						   row, columns);
	EXPECT_EQ(rc, 0);
	return bytes;
}

static std::string retained_media_transfer(unsigned id, unsigned format, int columns, int rows,
					   const unsigned char *bytes, size_t length, int width = 0,
					   int height = 0)
{
	char *encoded = test_encode_bytes(bytes, length);
	if (!encoded)
		return "";

	std::string result = "\033_Ga=T,f=" + std::to_string(format) + ",i=" + std::to_string(id) +
			     ",U=1,q=2,c=" + std::to_string(columns) + ",r=" + std::to_string(rows);
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
	std::vector<std::pair<std::string, std::optional<std::string>>> original_environment;
	int master = -1;
	FILE *input = nullptr, *output = nullptr;
	struct nt_application *app = nullptr;
	struct nt_view *document = nullptr;
	struct nt_view *original_text = nullptr;
	int text_actions = 0;
	std::string source, frame, plain;
	std::vector<Placement> placements;
	struct nt_renderer renderer = {
		+[](struct nt_renderer *backend, struct nt_surface *surface, FILE *) {
			auto *self = static_cast<CliRetainedMediaTest *>(backend->data);
			nb_buf rows{};
			int rc = nt_surface_rows(surface, &rows);
			if (!rc) {
				self->frame = nb_buf_cstr(&rows);
				char *plain =
					nt_terminal_strip_ansi_dup(rows.ptr, rows.len, nullptr);
				if (plain)
					self->plain = plain;

				free(plain);
			}

			nb_buf_cleanup(&rows);
			self->placements.clear();
			for (size_t i = 0; i < nt_surface_image_count(surface); i++) {
				const auto *image = nt_surface_image(surface, i);
				self->placements.push_back(
					{image->x, image->y, image->width, image->height,
					 image->source_x, image->source_y, image->source_width,
					 image->source_height, nt_media_asset_key(image->asset),
					 reinterpret_cast<uintptr_t>(image->asset)});
			}

			return rc;
		},
		nullptr, this};
	static void Build(struct nt_view_controller *controller, struct nt_builder *builder)
	{
		auto *self = static_cast<CliRetainedMediaTest *>(controller->data);
		auto *root =
			nt_stack_view_add(builder, nullptr, NT_STACK_VIEW_VERTICAL, 0, nullptr);
		self->document = cli_rich_document_add_borrow(builder, root);
		self->original_text = nt_view_child(self->document, 0);
		nt_connection listener = nt_view_on(
			self->original_text, NT_VIEW_EVENT_ACTIVATE,
			+[](void *data, const struct nt_view_event *) {
				static_cast<CliRetainedMediaTest *>(data)->text_actions++;
				return 0;
			},
			self);
		nt_builder_fail(builder, listener ? NT_OK : NT_ERR_NOMEM);
		nt_builder_fail(builder, cli_rich_document_set_rendered_copy(
						 self->document, self->source.c_str(), 1));
	}

	void SetUp() override
	{
		for (const char *key : {"TERM", "KITTY_WINDOW_ID", "TERM_PROGRAM", "ITERM_PROFILE",
			"WT_SESSION"}) {
			const char *value = getenv(key);
			original_environment.emplace_back(key, value ? std::make_optional<std::string>(value)
				: std::nullopt);
			unsetenv(key);
		}

		ASSERT_EQ(setenv("TERM", "xterm-kitty", 1), 0);
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
		struct nt_application_options options{};
		options.input = input;
		options.output = output;
		options.external_signals = 1;
		ASSERT_EQ(nt_application_create(&options, &app), 0);
		ASSERT_EQ(nt_window_set_renderer(nt_application_window(app), &renderer), 0);
	}

	void TearDown() override
	{
		nt_application_destroy(app);
		for (const auto &entry : original_environment) {
			if (entry.second)
				setenv(entry.first.c_str(), entry.second->c_str(), 1);
			else
				unsetenv(entry.first.c_str());
		}

		if (input)
			fclose(input);

		if (output)
			fclose(output);

		if (master >= 0)
			close(master);
	}

	void Present()
	{
		static const nt_view_controller_class type = [] {
			nt_view_controller_class value{};
			value.superclass = &nt_view_controller_base_class;
			value.instance_size = sizeof(struct nt_view_controller);
			value.load_view = Build;
			return value;
		}();
		struct nt_view_controller *page NB_CLEANUP(cli_application_controller_cleanup) =
			nullptr;
		ASSERT_EQ(nt_view_controller_create(&type, &page), 0);
		page->data = this;
		ASSERT_EQ(nt_application_present_view_controller(app, &page), 0);
		ASSERT_EQ(nt_application_step(app, 0), 0);
	}

	std::string PngTransfer(unsigned id, int columns = 3, int rows = 1)
	{
		std::vector<unsigned char> pixels(4u * 2u * 4u, 255u);
		std::vector<unsigned char> png;
		int rc = stbi_write_png_to_func(
			+[](void *context, void *bytes, int length) {
				auto &target = *static_cast<std::vector<unsigned char> *>(context);
				auto *data = static_cast<unsigned char *>(bytes);
				target.insert(target.end(), data, data + length);
			},
			&png, 4, 2, 4, pixels.data(), 4 * 4);
		EXPECT_NE(rc, 0);
		return retained_media_transfer(id, 100, columns, rows, png.data(), png.size());
	}
};

TEST_F(CliRetainedMediaTest, CommandOutputWrapsAtTheLastTerminalColumn)
{
	source = std::string(74, 'a') + " restore code";
	Present();
	EXPECT_NE(plain.find(std::string(74, 'a') + " resto\r\nre code"), std::string::npos) << plain;
}

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
	ASSERT_EQ(nt_application_step(app, 0), 0);
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
	std::string invalid = retained_media_transfer(id, 32, 2, 1, pixels, sizeof(pixels), 2, 2) +
			      retained_media_placeholders(id, 0, 2);
	EXPECT_LT(cli_rich_document_set_rendered_copy(document, invalid.c_str(), 1), 0);
	EXPECT_EQ(frame, previous);
	EXPECT_EQ(placements.size(), 1u);
}

TEST_F(CliRetainedMediaTest, MarkdownMathUsesNativeImagesAcrossTerminalProtocols)
{
	Present();
	const char *formulas[] = {
		"before $x^2$ after\n",
		"before \\(x^2\\) after\n",
		"$$\\frac{1}{2}$$\n",
		"$$\n\\int_{0}^{\\infty} e^{-x^2}\\,\\mathrm{d}x = "
		"\\frac{\\sqrt{\\pi}}{2}\n$$\n",
		"\\[\n\\begin{aligned}\na&=1\\\\[4pt]\nb&=2\n\\end{aligned}\n\\]\n",
		"| Formula | Result |\n|---|---|\n| $x^2$ | done |\n"
	};
	for (const char *protocol : {"kitty", "iterm", "sixel"}) {
		SCOPED_TRACE(protocol);
		setenv("TERM", !strcmp(protocol, "kitty") ? "xterm-kitty" : "xterm-256color", 1);
		unsetenv("TERM_PROGRAM");
		unsetenv("WT_SESSION");
		if (!strcmp(protocol, "iterm"))
			setenv("TERM_PROGRAM", "iTerm.app", 1);
		else if (!strcmp(protocol, "sixel"))
			setenv("WT_SESSION", "math-test", 1);

		for (const char *formula : formulas) {
			SCOPED_TRACE(formula);
			nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {};
			ASSERT_EQ(cli_markdown_render_retained(&rendered, formula, 80, 1), 0);
			ASSERT_NE(strstr(nb_buf_cstr(&rendered), "\033_Ga=T,f=32,"), nullptr);
			ASSERT_EQ(cli_rich_document_set_rendered_copy(document,
				nb_buf_cstr(&rendered), 1), 0);
			ASSERT_EQ(nt_application_step(app, 0), 0);
			ASSERT_EQ(placements.size(), 1u);
			EXPECT_GT(placements[0].source_width, 0);
			EXPECT_GT(placements[0].source_height, 0);
			EXPECT_EQ(plain.find("\\frac"), std::string::npos);
		}
	}
}

TEST_F(CliRetainedMediaTest, PlainMathAndLiteralCodeDoNotProduceImages)
{
	for (const auto &sample : {
		std::pair<const char *, int>{"$$\n\\frac{1}{2}\n$$\n", 0},
		{"`\\(x^2\\)` and `$x^2$`\n", 1},
		{"```latex\n\\[x^2\\]\n$$x^2$$\n```\n", 1}}) {
		SCOPED_TRACE(sample.first);
		nb_buf rendered NB_CLEANUP(nb_buf_cleanup) = {};
		ASSERT_EQ(cli_markdown_render_retained(&rendered, sample.first, 80, sample.second), 0);
		EXPECT_EQ(strstr(nb_buf_cstr(&rendered), "\033_G"), nullptr);
		EXPECT_NE(strstr(nb_buf_cstr(&rendered), sample.second ? "x^2" : "\\frac"), nullptr);
	}
}

TEST_F(CliRetainedMediaTest, InvalidPngPreservesTheDisplayedImage)
{
	const unsigned id = 0x53345678u;
	source = PngTransfer(id) + retained_media_placeholders(id, 0, 3);
	Present();
	ASSERT_EQ(placements.size(), 1u);
	const auto identity = placements[0].identity;
	const std::string original = frame;

	const unsigned char truncated[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	const std::string replacement =
		retained_media_transfer(id, 100, 3, 1, truncated, sizeof(truncated)) +
		retained_media_placeholders(id, 0, 3);
	EXPECT_EQ(cli_rich_document_set_rendered_copy(document, replacement.c_str(), 1),
		  NB_ERR_FORMAT);

	nt_view_invalidate(document, 1);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	ASSERT_EQ(placements.size(), 1u);
	EXPECT_EQ(placements[0].identity, identity);
	EXPECT_EQ(frame, original);
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
	ASSERT_EQ(cli_rich_document_set_rendered_copy(
			  document, "safe\033]52;c;private\a\033[2J\033[Htext", 1),
		  0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
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
	EXPECT_NE(frame.find("\033[0;1mbold"), std::string::npos);
}

TEST_F(CliRetainedMediaTest, TextUpdatesKeepTheirOriginalViewBindingAndReflowWithoutChildren)
{
	source = "initial text";
	Present();
	ASSERT_NE(original_text, nullptr);
	ASSERT_EQ(nt_view_child(document, 0), original_text);
	ASSERT_EQ(nt_view_child_count(document), 1u);
	for (int update = 0; update < 100; update++) {
		source = "update " + std::to_string(update) +
			 "\n\n\033[1mbold\033[0m and 中文🙂 "
			 "\033]8;;https://example.com/history\033\\linked\033]8;;\033\\";
		ASSERT_EQ(cli_rich_document_set_rendered_copy(document, source.c_str(), 1), 0);
		ASSERT_EQ(nt_view_child(document, 0), original_text);
		ASSERT_EQ(nt_view_child_count(document), 1u);
	}

	ASSERT_EQ(nt_view_emit(nt_view_child(document, 0), NT_VIEW_EVENT_ACTIVATE, 0, "retained"),
		  0);
	EXPECT_EQ(text_actions, 1);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_EQ(placements.size(), 0u);
	EXPECT_EQ(plain.find("update 99"), 0u);
	EXPECT_NE(frame.find("\033[0;1mbold"), std::string::npos);
	EXPECT_NE(frame.find("https://example.com/history"), std::string::npos);
	struct nt_view_size wide{}, narrow{}, restored{};
	ASSERT_EQ(nt_view_measure(document, 80, &wide), 0);
	ASSERT_EQ(nt_view_measure(document, 12, &narrow), 0);
	ASSERT_EQ(nt_view_measure(document, 80, &restored), 0);
	EXPECT_GT(narrow.height, wide.height);
	EXPECT_EQ(restored.height, wide.height);
	EXPECT_EQ(nt_view_child(document, 0), original_text);
}

TEST_F(CliRetainedMediaTest, MediaTransitionsKeepTextViewAndReleaseOnlyImageChildren)
{
	source = "before media";
	Present();
	const unsigned first = 0x53445566u, second = 0x53556677u;
	source = "before " + PngTransfer(first) + retained_media_placeholders(first, 0, 3) +
		 " after\n";
	ASSERT_EQ(cli_rich_document_set_rendered_copy(document, source.c_str(), 1), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	ASSERT_EQ(placements.size(), 1u);
	const auto original = placements[0];
	auto *images = nt_view_child(document, 1);
	ASSERT_NE(images, nullptr);
	EXPECT_EQ(nt_view_child_count(images), 1u);
	EXPECT_EQ(nt_view_child(document, 0), original_text);
	source = "before " + PngTransfer(second) + retained_media_placeholders(second, 0, 3) +
		 " after\n";
	ASSERT_EQ(cli_rich_document_set_rendered_copy(document, source.c_str(), 1), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	ASSERT_EQ(placements.size(), 1u);
	EXPECT_EQ(placements[0].identity, original.identity);
	EXPECT_EQ(nt_view_child(document, 1), images);
	EXPECT_EQ(nt_view_child(document, 0), original_text);
	ASSERT_EQ(cli_rich_document_set_rendered_copy(document, "text after media", 1), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	EXPECT_EQ(placements.size(), 0u);
	EXPECT_EQ(nt_view_child_count(images), 0u);
	EXPECT_TRUE(nt_view_is_hidden(images));
	EXPECT_EQ(plain.find("text after media"), 0u);
	EXPECT_EQ(nt_view_child(document, 0), original_text);
	ASSERT_EQ(nt_view_emit(nt_view_child(document, 0), NT_VIEW_EVENT_ACTIVATE, 0, "retained"),
		  0);
	EXPECT_EQ(text_actions, 1);
	source = PngTransfer(first) + retained_media_placeholders(first, 0, 3) + " again";
	ASSERT_EQ(cli_rich_document_set_rendered_copy(document, source.c_str(), 1), 0);
	ASSERT_EQ(nt_application_step(app, 0), 0);
	ASSERT_EQ(placements.size(), 1u);
	EXPECT_EQ(placements[0].key, original.key);
	EXPECT_FALSE(nt_view_is_hidden(images));
	EXPECT_EQ(nt_view_child(document, 0), original_text);
}
