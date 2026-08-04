#include <gtest/gtest.h>

#include "morph_kitty_protocol.h"

#include <cerrno>
#include <cstdint>
#include <string>
#include <unordered_set>

static int append_kitty_output(const char *bytes, size_t len, void *user_data)
{
	auto *output = static_cast<std::string *>(user_data);

	output->append(bytes, len);
	return 0;
}

TEST(KittyProtocol, ImageIdsAreUniqueAndPlaceholderSafe)
{
	std::unordered_set<uint32_t> ids;

	for (int i = 0; i < 4096; i++) {
		uint32_t id = morph_kitty_image_id_new();

		EXPECT_NE(id, 0u);
		EXPECT_EQ(id & 0xff000000u, 0x53000000u);
		EXPECT_NE(id & 0x00ffff00u, 0u);
		EXPECT_TRUE(ids.insert(id).second);
	}
}

TEST(KittyProtocol, PlaceholderRowEncodesEveryCell)
{
	std::string output;
	const uint32_t id = 0x7a123456u;
	const std::string placeholder = "\xF4\x8E\xBB\xAE";

	ASSERT_EQ(morph_kitty_write_placeholder_row(
			  append_kitty_output, &output, id, 7u, 3u),
		  0);
	EXPECT_NE(output.find("\033[38:2:18:52:86m"), std::string::npos);
	EXPECT_EQ(output.find("\033[39m"), output.size() - 5u);

	size_t count = 0u;
	size_t offset = 0u;
	while ((offset = output.find(placeholder, offset)) != std::string::npos) {
		count++;
		offset += placeholder.size();
	}
	EXPECT_EQ(count, 3u);
}

TEST(KittyProtocol, RejectsUnrepresentableRowsAndColumns)
{
	std::string output;

	EXPECT_EQ(morph_kitty_write_placeholder_row(
			  append_kitty_output, &output, 0x7a123456u,
			  MORPH_KITTY_PLACEHOLDER_LIMIT, 1u),
		  -EINVAL);
	EXPECT_EQ(morph_kitty_write_placeholder_row(
			  append_kitty_output, &output, 0x7a123456u,
			  0u, MORPH_KITTY_PLACEHOLDER_LIMIT + 1u),
		  -EINVAL);
}
