#include <gtest/gtest.h>
#include "util/json.h"
#include "util/arena.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#include <string>

namespace
{

using JsonOwner = std::unique_ptr<json, decltype(&json_free)>;
using JsonText = std::unique_ptr<char, decltype(&json_text_free)>;

JsonOwner own(json *value)
{
	return JsonOwner(value, json_free);
}

std::string encode(const json *value)
{
	JsonText text(json_stringify(value), json_text_free);

	return text ? text.get() : "<serialization failed>";
}

} /* namespace */

TEST(JsonPathEdges, EscapedAndQuotedMembersRemainLiteral)
{
	auto root = own(json_object());
	ASSERT_NE(root, nullptr);
	const char *exact_path = R"(a\.b\[0\]\\z[""][0]["\u4e2d\u6587"])";

	ASSERT_EQ(json_set(root.get(), exact_path, "kept"), 0);
	json *literal = json_member(root.get(), "a.b[0]\\z");
	ASSERT_NE(literal, nullptr);
	json *empty_member = json_member(literal, "");
	ASSERT_TRUE(json_is_array_value(empty_member));
	json *first = json_at(empty_member, 0);
	ASSERT_NE(first, nullptr);
	EXPECT_STREQ(json_string_value(json_member(first, "中文")), "kept");
	EXPECT_STREQ(json_str(root.get(), R"(["a.b[0]\\z"][""][00]["中文"])"), "kept");
	EXPECT_FALSE(json_exists(root.get(), "a.b[0]"));
	ASSERT_EQ(json_set(root.get(), R"(["\uD83D\uDE00"])", 7), 0);
	EXPECT_EQ(json_int(root.get(), R"(["😀"])"), 7);
	ASSERT_EQ(json_erase(root.get(), exact_path), 0);
	EXPECT_EQ(json_size(first), 0u);
}

TEST(JsonPathEdges, SyntaxFailuresLeaveTreeAndOwnedValueUntouched)
{
	auto root = own(json_parse(R"({"keep":{"x":1},"items":[2]})"));
	auto value = own(json_parse(R"({"payload":["kept"]})"));
	ASSERT_NE(root, nullptr);
	ASSERT_NE(value, nullptr);
	const auto original = encode(root.get());
	const auto original_value = encode(value.get());
	const char *invalid[] = {
		".x", "x.",  "x..y", "x.[0]",	"x[]",	    "x[+1]",	  "x[-1]", "x[0]tail",
		"x]", "x\\", "x\\q", "[\"open", "[\"key\"", "[ \"key\"]", "x[ 0]",
	};

	for (const char *path : invalid) {
		SCOPED_TRACE(path);
		json *raw = value.get();

		EXPECT_EQ(json_set_take(root.get(), path, &raw), -EINVAL);
		EXPECT_EQ(raw, value.get());
		EXPECT_EQ(json_append(root.get(), path, 1), -EINVAL);
		EXPECT_EQ(json_erase(root.get(), path), -EINVAL);
		EXPECT_EQ(json_get(root.get(), path), nullptr);
		EXPECT_EQ(encode(root.get()), original);
		EXPECT_EQ(encode(value.get()), original_value);
	}
}

TEST(JsonPathEdges, InvalidUtf8IsReportedBeforeAnyMutation)
{
	auto root = own(json_parse(R"({"keep":1})"));
	auto value = own(json_string("kept"));
	ASSERT_NE(root, nullptr);
	ASSERT_NE(value, nullptr);
	const auto original = encode(root.get());
	const char *invalid[] = {
		"\xc0\xaf", "\xe4\xb8", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80",
	};

	for (const char *bytes : invalid) {
		SCOPED_TRACE(::testing::PrintToString(bytes));
		json *raw = value.get();
		std::string quoted = std::string("[\"") + bytes + "\"]";

		EXPECT_EQ(json_set(root.get(), bytes, 1), -EILSEQ);
		EXPECT_EQ(json_set(root.get(), quoted.c_str(), 1), -EILSEQ);
		EXPECT_EQ(json_set_take(root.get(), bytes, &raw), -EILSEQ);
		EXPECT_EQ(raw, value.get());
		EXPECT_EQ(json_append(root.get(), bytes, 1), -EILSEQ);
		EXPECT_EQ(json_erase(root.get(), bytes), -EILSEQ);
		EXPECT_EQ(json_set(root.get(), "text", bytes), -EILSEQ);
		EXPECT_EQ(json_append(root.get(), "items", bytes), -EILSEQ);
		EXPECT_EQ(encode(root.get()), original);
	}
}

TEST(JsonPathEdges, MissingSuffixDoesNotCommitBeforeGapOrDepthValidation)
{
	auto root = own(json_parse(R"({"keep":{"x":1},"items":[2],"nil":null})"));
	auto value = own(json_parse(R"({"payload":"kept"})"));
	ASSERT_NE(root, nullptr);
	ASSERT_NE(value, nullptr);
	json *keep = json_member(root.get(), "keep");
	const auto original = encode(root.get());
	const char *gaps[] = {"new[1].child", "new.child[2]", "items[2].child"};

	for (const char *path : gaps) {
		SCOPED_TRACE(path);
		json *raw = value.get();

		EXPECT_EQ(json_set_take(root.get(), path, &raw), -ERANGE);
		EXPECT_EQ(raw, value.get());
		EXPECT_EQ(json_append(root.get(), path, 1), -ERANGE);
		EXPECT_EQ(encode(root.get()), original);
		EXPECT_EQ(json_member(root.get(), "keep"), keep);
	}
	for (const char *path : {"nil.child", "keep.x.child"}) {
		EXPECT_EQ(json_set(root.get(), path, 1), -EINVAL);
		EXPECT_EQ(json_append(root.get(), path, 1), -EINVAL);
		EXPECT_EQ(encode(root.get()), original);
	}
	std::string path = "deep";
	for (int i = 1; i < JSON_MAX_DEPTH; i++)
		path += ".x";
	json *raw = value.get();
	EXPECT_EQ(json_set_take(root.get(), path.c_str(), &raw), -E2BIG);
	EXPECT_EQ(raw, value.get());
	EXPECT_EQ(encode(root.get()), original);
	EXPECT_EQ(json_set(root.get(), (path + ".x").c_str(), 1), -E2BIG);
	EXPECT_EQ(encode(root.get()), original);
}

TEST(JsonPathEdges, SuccessfulTakeBuildsMixedBranchesAndClearsOwnership)
{
	auto root = own(json_object());
	ASSERT_NE(root, nullptr);
	json *value = json_parse(R"({"x":1})");
	ASSERT_NE(value, nullptr);
	ASSERT_EQ(json_append_take(root.get(), "items[0].values", &value), 0);
	EXPECT_EQ(value, nullptr);
	EXPECT_EQ(json_int(root.get(), "items[0].values[0].x"), 1);
	EXPECT_EQ(json_size(json_get(root.get(), "items[0].values")), 1u);
	ASSERT_EQ(json_set(root.get(), "items[1].values[0]", "second"), 0);
	EXPECT_STREQ(json_str(root.get(), "items[1].values[0]"), "second");
	ASSERT_EQ(json_erase(root.get(), "items[0]"), 0);
	EXPECT_EQ(json_size(json_member(root.get(), "items")), 1u);
	EXPECT_STREQ(json_str(root.get(), "items[0].values[0]"), "second");
}

TEST(JsonPathEdges, RootOperationsDoNotConvertExplicitNullToArray)
{
	auto root = own(json_null());
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_get(root.get(), nullptr), root.get());
	EXPECT_EQ(json_get(root.get(), ""), root.get());
	EXPECT_TRUE(json_exists(root.get(), ""));
	EXPECT_EQ(json_append(root.get(), nullptr, 1), -EINVAL);
	EXPECT_EQ(json_erase(root.get(), nullptr), -EINVAL);
	EXPECT_EQ(json_erase(root.get(), ""), -EINVAL);
	EXPECT_TRUE(json_is_null(root.get(), ""));
	auto array = own(json_array());
	ASSERT_NE(array, nullptr);
	ASSERT_EQ(json_set(root.get(), "", array.get()), 0);
	ASSERT_EQ(json_append(root.get(), nullptr, false), 0);
	EXPECT_TRUE(json_is_bool(root.get(), "[0]"));
	EXPECT_FALSE(json_bool_or(root.get(), "[0]", true));
	EXPECT_EQ(json_erase(root.get(), "[1]"), -ENOENT);
	EXPECT_EQ(json_erase(root.get(), "[0].x"), -EINVAL);
	EXPECT_EQ(json_set(root.get(), "[184467440737095516160]", 1), -ERANGE);
	EXPECT_EQ(json_size(root.get()), 1u);
}

TEST(JsonParserEdges, UnicodeValidationIncludesQuotedKeysAndEscapedSurrogates)
{
	struct json_error error{};
	const char input[] = R"({"中文":"日本語", "emoji":"\ud83d\ude00"})";
	auto root = own(json_parse_n(input, sizeof(input) - 1, &error));
	ASSERT_NE(root, nullptr) << error.code << " at " << error.offset;
	EXPECT_STREQ(json_str(root.get(), R"(["\u4e2d\u6587"])"), "日本語");
	EXPECT_STREQ(json_str(root.get(), "emoji"), "😀");
	const char *invalid[] = {
		"\"\xc0\xaf\"",
		"\"\xe4\xb8\"",
		"\"\xed\xa0\x80\"",
		"\"\xf4\x90\x80\x80\"",
	};
	for (const char *text : invalid) {
		auto bad = own(json_parse_n(text, std::strlen(text), &error));

		EXPECT_EQ(bad, nullptr);
		EXPECT_EQ(error.code, -EILSEQ);
	}
	for (const char *text : {R"("\ud800")", R"("\udc00")", R"("\ud800\u0041")"}) {
		auto bad = own(json_parse_n(text, std::strlen(text), &error));

		EXPECT_EQ(bad, nullptr);
		EXPECT_EQ(error.code, -EINVAL);
	}
}

TEST(JsonParserEdges, NestingCountsScalarLeavesAndEmptyContainers)
{
	struct json_error error{};
	std::string last_scalar =
		std::string(JSON_MAX_DEPTH - 1, '[') + "0" + std::string(JSON_MAX_DEPTH - 1, ']');
	std::string last_container =
		std::string(JSON_MAX_DEPTH, '[') + std::string(JSON_MAX_DEPTH, ']');

	auto scalar = own(json_parse_n(last_scalar.c_str(), last_scalar.size(), &error));
	ASSERT_NE(scalar, nullptr) << error.code;
	auto container = own(json_parse_n(last_container.c_str(), last_container.size(), &error));
	ASSERT_NE(container, nullptr) << error.code;
	for (const auto &text : {"[" + last_scalar + "]", "[" + last_container + "]"}) {
		auto bad = own(json_parse_n(text.c_str(), text.size(), &error));

		EXPECT_EQ(bad, nullptr);
		EXPECT_EQ(error.code, -E2BIG);
	}
}

TEST(JsonParserEdges, NumericLocaleIsScopedForParsingAndAllSerializationModes)
{
	auto root = own(json_parse(R"({"n":1.25})"));
	std::unique_ptr<struct arena, decltype(&arena_destroy)> arena(arena_create(1024),
								      arena_destroy);
	ASSERT_NE(root, nullptr);
	ASSERT_NE(arena, nullptr);
	locale_t numeric = newlocale(LC_NUMERIC_MASK, "fr_FR.UTF-8", nullptr);

	if (!numeric)
		numeric = newlocale(LC_NUMERIC_MASK, "de_DE.UTF-8", nullptr);
	if (!numeric)
		GTEST_SKIP() << "No comma decimal locale is installed";
	locale_t previous = uselocale(numeric);
	if (!previous) {
		freelocale(numeric);
		FAIL() << "Cannot activate the thread numeric locale";
	}
	const char input[] = "1.25";
	struct json_error error{};
	auto parsed = own(json_parse_n(input, sizeof(input) - 1, &error));
	bool parse_restored = uselocale(nullptr) == numeric;
	JsonText compact(json_stringify(root.get()), json_text_free);
	bool compact_restored = uselocale(nullptr) == numeric;
	JsonText pretty(json_stringify_pretty(root.get()), json_text_free);
	bool pretty_restored = uselocale(nullptr) == numeric;
	const char *arena_text = json_stringify_arena(arena.get(), root.get());
	bool arena_restored = uselocale(nullptr) == numeric;

	uselocale(previous);
	freelocale(numeric);
	ASSERT_NE(parsed, nullptr) << error.code;
	EXPECT_DOUBLE_EQ(json_number_value(parsed.get()), 1.25);
	EXPECT_TRUE(parse_restored);
	EXPECT_TRUE(compact_restored);
	EXPECT_TRUE(pretty_restored);
	EXPECT_TRUE(arena_restored);
	ASSERT_NE(compact, nullptr);
	EXPECT_STREQ(compact.get(), R"({"n":1.25})");
	ASSERT_NE(pretty, nullptr);
	auto pretty_document = own(json_parse(pretty.get()));
	ASSERT_NE(pretty_document, nullptr);
	EXPECT_DOUBLE_EQ(json_num(pretty_document.get(), "n"), 1.25);
	ASSERT_NE(arena_text, nullptr);
	EXPECT_STREQ(arena_text, R"({"n":1.25})");
}

TEST(JsonCoreEdges, StringUpdatesCopyBorrowedSelfAndSlicesBeforeReleasingStorage)
{
	auto root = own(json_parse(R"({"name":"abcdef","stable":true})"));
	ASSERT_NE(root, nullptr);
	json *name = json_member(root.get(), "name");
	ASSERT_NE(name, nullptr);
	const char *slice = json_string_value(name) + 2;

	ASSERT_NE(json_string_update(name, slice), nullptr);
	EXPECT_STREQ(json_string_value(name), "cdef");
	EXPECT_STREQ(json_key(name), "name");
	ASSERT_NE(json_string_update(name, json_string_value(name)), nullptr);
	EXPECT_STREQ(json_string_value(name), "cdef");
	ASSERT_EQ(json_set(root.get(), "name", json_string_value(name) + 1), 0);
	EXPECT_STREQ(json_str(root.get(), "name"), "def");
	EXPECT_TRUE(json_bool(root.get(), "stable"));
}
