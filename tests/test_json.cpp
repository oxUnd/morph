#include <gtest/gtest.h>

extern "C" {
#include "util/json.h"
#include "util/arena.h"
}

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
struct JsonDeleter {
	void operator()(json *value) const
	{
		json_free(value);
	}
};
using JsonPtr = std::unique_ptr<json, JsonDeleter>;

std::string snapshot(const json *value)
{
	char *text = json_stringify(value);
	EXPECT_NE(text, nullptr);
	std::string result = text ? text : "";
	json_text_free(text);
	return result;
}

std::string nested_document(size_t depth)
{
	return std::string(depth - 1, '[') + "0" + std::string(depth - 1, ']');
}

std::string deep_path(size_t depth)
{
	std::string path;

	for (size_t i = 1; i < depth; i++) {
		if (!path.empty())
			path += '.';
		path += 'a';
	}
	return path;
}

void expect_round_trip(const json *value)
{
	std::string text = snapshot(value);
	struct json_error error = {};
	JsonPtr copy(json_parse_n(text.data(), text.size(), &error));

	ASSERT_NE(copy, nullptr) << text << ": " << error.code << " at " << error.offset;
	EXPECT_TRUE(json_equal(value, copy.get(), true)) << text;
}
} // namespace

#ifndef JSON_FAULT_TESTING
extern "C" int json_c_api_probe(void);
extern "C" int json_cpp_api_probe(void);

TEST(JsonApi, C11AndCpp17GenericCallsEvaluateEachArgumentOnce)
{
	EXPECT_EQ(json_c_api_probe(), 0);
	EXPECT_EQ(json_cpp_api_probe(), 0);
}

TEST(JsonParse, StrictParsingRejectsTrailingInputAndReportsTheOffset)
{
	const char text[] = "{\"n\":1} tail";
	struct json_error error = {};
	JsonPtr invalid(json_parse_n(text, sizeof(text) - 1, &error));

	EXPECT_EQ(invalid, nullptr);
	EXPECT_EQ(error.code, -EINVAL);
	EXPECT_EQ(error.offset, 8u);
	ASSERT_NE(error.message, nullptr);
	EXPECT_NE(error.message[0], '\0');
	EXPECT_EQ(json_parse("true false"), nullptr);
}

TEST(JsonParse, PrefixParsingExplicitlyReturnsTheFirstUnconsumedByte)
{
	const char text[] = " \ttrue,false";
	const char *end = nullptr;
	struct json_error error = {};
	JsonPtr value(json_parse_prefix(text, &end, &error));

	ASSERT_NE(value, nullptr);
	EXPECT_TRUE(json_is_true(value.get()));
	EXPECT_EQ(end, text + 6);
	EXPECT_EQ(error.code, 0);
	JsonPtr same(json_parse_ex(text, &end, false, &error));
	ASSERT_NE(same, nullptr);
	EXPECT_EQ(end, text + 6);
	JsonPtr rejected(json_parse_ex(text, &end, true, &error));
	EXPECT_EQ(rejected, nullptr);
	EXPECT_EQ(error.code, -EINVAL);
}

TEST(JsonParse, LengthBoundedInputDoesNotRequireANulTerminator)
{
	const char text[] = {'[', '1', ']', 'x'};
	struct json_error error = {};
	JsonPtr value(json_parse_n(text, 3, &error));

	ASSERT_NE(value, nullptr);
	EXPECT_EQ(json_int(value.get(), "[0]"), 1);
	JsonPtr invalid(json_parse_n(text, sizeof(text), &error));
	EXPECT_EQ(invalid, nullptr);
	EXPECT_EQ(error.code, -EINVAL);
	EXPECT_EQ(error.offset, 3u);
}

TEST(JsonParse, ErrorsArePerCallAndSuccessClearsPreviousDiagnostics)
{
	struct json_error first = {};
	struct json_error second = {};
	JsonPtr invalid(json_parse_n("{\"a\":?}", 7, &first));

	EXPECT_EQ(invalid, nullptr);
	EXPECT_EQ(first.code, -EINVAL);
	EXPECT_EQ(first.offset, 5u);
	std::string message = first.message ? first.message : "";
	JsonPtr another(json_parse_n("[", 1, &second));
	EXPECT_EQ(another, nullptr);
	EXPECT_EQ(first.code, -EINVAL);
	EXPECT_EQ(first.offset, 5u);
	EXPECT_EQ(message, first.message);
	const char valid_text[] = " \r\n{\"a\":1}\t ";
	JsonPtr valid(json_parse_n(valid_text, sizeof(valid_text) - 1, &second));
	ASSERT_NE(valid, nullptr);
	EXPECT_EQ(second.code, 0);
	JsonPtr null_input(json_parse_n(nullptr, 0, &second));
	EXPECT_EQ(null_input, nullptr);
	EXPECT_EQ(second.code, -EINVAL);
}

TEST(JsonParse, RejectsMalformedJsonRatherThanAcceptingBackendExtensions)
{
	const char *inputs[] = {
		"",	     " ",	 "{}x",	    "01",	   "-01",
		"+1",	     ".1",	 "1.",	    "1e",	   "--1",
		"NaN",	     "Infinity", "[1,]",    "[,1]",	   "{\"a\":1,}",
		"{\"a\" 1}", "'text'",	 "\"\\x\"", "\"\\uD800\"", "\"\\uDC00\"",
		"\"a\nb\"",
	};

	for (const char *input : inputs) {
		SCOPED_TRACE(input);
		struct json_error error = {};
		JsonPtr value(json_parse_n(input, std::strlen(input), &error));
		EXPECT_EQ(value, nullptr);
		EXPECT_LT(error.code, 0);
		EXPECT_LE(error.offset, std::strlen(input));
	}
}

TEST(JsonParse, DecodesUnicodeAndEveryJsonStringEscape)
{
	JsonPtr value(json_parse("{\"text\":\"\\u96ea\\uD83D\\uDE00\\\"\\\\\\/\\b\\f\\n\\r\\t\"}"));

	ASSERT_NE(value, nullptr);
	EXPECT_STREQ(json_str(value.get(), "text"), "雪😀\"\\/\b\f\n\r\t");
	expect_round_trip(value.get());
}

TEST(JsonParse, RejectsInvalidUtf8AndEmbeddedNulInsteadOfTruncating)
{
	const std::vector<std::string> invalid_utf8 = {
		std::string("\"\xc0\xaf\"", 4),
		std::string("\"\xed\xa0\x80\"", 5),
		std::string("{\"\xff\":1}", 7),
	};

	for (const std::string &input : invalid_utf8) {
		struct json_error error = {};
		JsonPtr value(json_parse_n(input.data(), input.size(), &error));
		EXPECT_EQ(value, nullptr);
		EXPECT_EQ(error.code, -EILSEQ);
	}
	for (const char *input : {"\"a\\u0000b\"", "{\"\\u0000\":1}"}) {
		struct json_error error = {};
		JsonPtr value(json_parse_n(input, std::strlen(input), &error));
		EXPECT_EQ(value, nullptr);
		EXPECT_EQ(error.code, -ENOTSUP);
	}
	const char raw[] = {'a', '\0', 'b'};
	EXPECT_EQ(json_string_n(raw, sizeof(raw)), nullptr);
	const char document[] = {'"', 'a', '\0', 'b', '"'};
	struct json_error error = {};
	JsonPtr value(json_parse_n(document, sizeof(document), &error));
	EXPECT_EQ(value, nullptr);
	EXPECT_LT(error.code, 0);
}

TEST(JsonParse, RejectsNonFiniteNumbersAndExcessiveDepth)
{
	struct json_error error = {};
	JsonPtr infinity(json_parse_n("1e9999", 6, &error));
	EXPECT_EQ(infinity, nullptr);
	EXPECT_EQ(error.code, -ERANGE);
	std::string allowed = nested_document(JSON_MAX_DEPTH);
	JsonPtr value(json_parse_n(allowed.data(), allowed.size(), &error));
	ASSERT_NE(value, nullptr) << error.code << " at " << error.offset;
	std::string excessive = nested_document(JSON_MAX_DEPTH + 1);
	JsonPtr rejected(json_parse_n(excessive.data(), excessive.size(), &error));
	EXPECT_EQ(rejected, nullptr);
	EXPECT_EQ(error.code, -E2BIG);
}

TEST(JsonConstruct, CopiesStringsAndSupportsBoundedStringsAndOwnedText)
{
	char text[] = "original";
	JsonPtr value(json_string(text));
	ASSERT_NE(value, nullptr);
	text[0] = 'X';
	EXPECT_STREQ(json_string_value(value.get()), "original");
	JsonPtr bounded(json_string_n("prefix-suffix", 6));
	ASSERT_NE(bounded, nullptr);
	EXPECT_STREQ(json_string_value(bounded.get()), "prefix");
	char *owned = static_cast<char *>(std::malloc(6));
	ASSERT_NE(owned, nullptr);
	std::memcpy(owned, "owned", 6);
	JsonPtr taken(json_take_string(owned));
	ASSERT_NE(taken, nullptr);
	EXPECT_STREQ(json_string_value(taken.get()), "owned");
	EXPECT_EQ(json_string(nullptr), nullptr);
	EXPECT_EQ(json_number(std::numeric_limits<double>::infinity()), nullptr);
	EXPECT_EQ(json_number(std::numeric_limits<double>::quiet_NaN()), nullptr);
}

TEST(JsonConstruct, ByteFragmentsRequireExplicitConstructionAndCannotBeSerialized)
{
	const char fragment[] = {static_cast<char>(0xf0), static_cast<char>(0x9f)};
	EXPECT_EQ(json_string_n(fragment, sizeof(fragment)), nullptr);
	JsonPtr bytes(json_string_bytes_n(fragment, sizeof(fragment)));
	ASSERT_NE(bytes, nullptr);
	errno = 0;
	EXPECT_EQ(json_stringify(bytes.get()), nullptr);
	EXPECT_EQ(errno, EILSEQ);
	JsonPtr root(json_object());
	ASSERT_EQ(json_object_add(root.get(), "fragment", bytes.get()), 0);
	bytes.release();
	EXPECT_EQ(json_stringify_pretty(root.get()), nullptr);
	EXPECT_EQ(errno, EILSEQ);
	const char with_nul[] = {'a', '\0', 'b'};
	EXPECT_EQ(json_string_bytes_n(with_nul, sizeof(with_nul)), nullptr);
}

TEST(JsonConstruct, DeepAndShallowClonesHaveIndependentOwnership)
{
	JsonPtr original(json_parse("{\"list\":[{\"name\":\"first\"}]}"));
	ASSERT_NE(original, nullptr);
	JsonPtr deep(json_clone(original.get(), true));
	JsonPtr shallow(json_clone(original.get(), false));
	ASSERT_NE(deep, nullptr);
	ASSERT_NE(shallow, nullptr);
	EXPECT_TRUE(json_equal(original.get(), deep.get(), true));
	EXPECT_EQ(json_size(shallow.get()), 0u);
	ASSERT_EQ(json_set(original.get(), "list[0].name", "changed"), 0);
	EXPECT_STREQ(json_str(deep.get(), "list[0].name"), "first");
	JsonPtr scalar(json_string("scalar"));
	JsonPtr scalar_copy(json_clone(scalar.get(), false));
	ASSERT_NE(scalar_copy, nullptr);
	EXPECT_TRUE(json_equal(scalar.get(), scalar_copy.get(), true));
}

TEST(JsonConstruct, CompactPrettyAndArenaSerializationRemainParseable)
{
	JsonPtr original(json_parse("{\"name\":\"雪\",\"items\":[true,null,0.125]}"));
	ASSERT_NE(original, nullptr);
	char *pretty = json_stringify_pretty(original.get());
	ASSERT_NE(pretty, nullptr);
	EXPECT_NE(std::strchr(pretty, '\n'), nullptr);
	JsonPtr parsed(json_parse(pretty));
	json_text_free(pretty);
	ASSERT_NE(parsed, nullptr);
	EXPECT_TRUE(json_equal(original.get(), parsed.get(), true));
	struct arena *arena = arena_create(128);
	ASSERT_NE(arena, nullptr);
	char *text = json_stringify_arena(arena, original.get());
	ASSERT_NE(text, nullptr);
	EXPECT_EQ(snapshot(original.get()), text);
	arena_destroy(arena);
	expect_round_trip(original.get());
	EXPECT_EQ(json_stringify(nullptr), nullptr);
	json_free(nullptr);
	json_text_free(nullptr);
}

TEST(JsonNumbers, FiniteDoublesAndSafeIntegerRegressionRoundTripExactly)
{
	const double values[] = {
		0.0,
		-0.0,
		1.0,
		-1.0,
		0.1,
		-0.125,
		std::numeric_limits<double>::min(),
		std::numeric_limits<double>::denorm_min(),
		std::numeric_limits<double>::max(),
		std::numeric_limits<double>::lowest(),
		static_cast<double>(JSON_SAFE_INTEGER),
		-static_cast<double>(JSON_SAFE_INTEGER),
	};

	for (double number : values) {
		SCOPED_TRACE(number);
		JsonPtr value(json_number(number));
		ASSERT_NE(value, nullptr);
		std::string text = snapshot(value.get());
		JsonPtr parsed(json_parse(text.c_str()));
		ASSERT_NE(parsed, nullptr) << text;
		EXPECT_EQ(json_number_value(parsed.get()), number) << text;
	}
	JsonPtr root(json_object());
	ASSERT_EQ(json_set(root.get(), "safe", JSON_SAFE_INTEGER), 0);
	JsonPtr parsed(json_parse(snapshot(root.get()).c_str()));
	ASSERT_NE(parsed, nullptr);
	EXPECT_EQ(json_int(parsed.get(), "safe"), JSON_SAFE_INTEGER);
	std::string unchanged = snapshot(root.get());
	EXPECT_EQ(json_set(root.get(), "unsafe", JSON_SAFE_INTEGER + 1), -ERANGE);
	EXPECT_EQ(json_set(root.get(), "unsafe", std::numeric_limits<int64_t>::min()), -ERANGE);
	EXPECT_EQ(json_set(root.get(), "unsafe", std::numeric_limits<uint64_t>::max()), -ERANGE);
	EXPECT_EQ(snapshot(root.get()), unchanged);
}

TEST(JsonNumbers, SeededFiniteBitPatternsRoundTripWithoutRoundingChanges)
{
	uint64_t state = UINT64_C(0x6a09e667f3bcc909);

	for (size_t i = 0; i < 192; i++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		double number;
		std::memcpy(&number, &state, sizeof(number));
		if (!std::isfinite(number))
			continue;
		JsonPtr value(json_number(number));
		ASSERT_NE(value, nullptr);
		std::string text = snapshot(value.get());
		JsonPtr copy(json_parse(text.c_str()));
		ASSERT_NE(copy, nullptr) << "iteration " << i << ": " << text;
		EXPECT_EQ(json_number_value(copy.get()), number)
			<< "iteration " << i << ": " << text;
	}
}

TEST(JsonRead, DistinguishesMissingNullAndEveryScalarType)
{
	JsonPtr root(json_parse(
		"{\"null\":null,\"false\":false,\"zero\":0,\"empty\":\"\",\"a\":[],\"o\":{}}"));
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_typeof(nullptr), JSON_MISSING);
	EXPECT_EQ(json_typeof(json_get(root.get(), "null")), JSON_NULL_TYPE);
	EXPECT_TRUE(json_is_null(root.get(), "null"));
	EXPECT_FALSE(json_is_null(root.get(), "missing"));
	EXPECT_TRUE(json_is_bool(root.get(), "false"));
	EXPECT_TRUE(json_is_number(root.get(), "zero"));
	EXPECT_TRUE(json_is_string(root.get(), "empty"));
	EXPECT_TRUE(json_is_array(root.get(), "a"));
	EXPECT_TRUE(json_is_object(root.get(), "o"));
	EXPECT_TRUE(json_exists(root.get(), "null"));
	EXPECT_FALSE(json_exists(root.get(), "missing"));
	EXPECT_STREQ(json_str_or(root.get(), "empty", "fallback"), "");
	EXPECT_STREQ(json_str_or(root.get(), "zero", "fallback"), "fallback");
	EXPECT_EQ(json_int_or(root.get(), "zero", 42), 0);
	EXPECT_EQ(json_int_or(root.get(), "false", 42), 42);
	EXPECT_EQ(json_num_or(root.get(), "missing", 0.5), 0.5);
	EXPECT_FALSE(json_bool_or(root.get(), "false", true));
	EXPECT_TRUE(json_bool_or(root.get(), "zero", true));
}

TEST(JsonRead, TypedReadsNeverCoerceAndKeepOutputsUntouchedOnFailure)
{
	JsonPtr text(json_string("26"));
	JsonPtr number(json_number(26));
	JsonPtr fraction(json_number(0.5));
	JsonPtr negative(json_number(-1));
	int64_t integer = 77;
	uint64_t unsigned_integer = 88;
	double real = 0.75;
	bool boolean = true;
	const char *string = "unchanged";

	EXPECT_EQ(json_read_int(nullptr, &integer), -ENOENT);
	EXPECT_EQ(json_read_int(text.get(), &integer), -EINVAL);
	EXPECT_EQ(json_read_int(fraction.get(), &integer), -ERANGE);
	EXPECT_EQ(integer, 77);
	EXPECT_EQ(json_read_uint(negative.get(), &unsigned_integer), -ERANGE);
	EXPECT_EQ(unsigned_integer, 88u);
	EXPECT_EQ(json_read_number(text.get(), &real), -EINVAL);
	EXPECT_EQ(real, 0.75);
	EXPECT_EQ(json_read_bool(number.get(), &boolean), -EINVAL);
	EXPECT_TRUE(boolean);
	EXPECT_EQ(json_read_string(number.get(), &string), -EINVAL);
	EXPECT_STREQ(string, "unchanged");
	EXPECT_EQ(json_read_number(number.get(), nullptr), -EINVAL);
	EXPECT_EQ(json_read_int(number.get(), &integer), 0);
	EXPECT_EQ(integer, 26);
	EXPECT_EQ(json_read_uint(number.get(), &unsigned_integer), 0);
	EXPECT_EQ(unsigned_integer, 26u);
}

TEST(JsonRead, IntegerReadsCheckTheActualSignedAndUnsignedBoundaries)
{
	JsonPtr signed_min(json_number(-std::ldexp(1.0, 63)));
	JsonPtr signed_overflow(json_number(std::ldexp(1.0, 63)));
	JsonPtr unsigned_overflow(json_number(std::ldexp(1.0, 64)));
	JsonPtr unsigned_last(json_number(std::nextafter(std::ldexp(1.0, 64), 0.0)));
	int64_t integer = 0;
	uint64_t unsigned_integer = 0;

	ASSERT_EQ(json_read_int(signed_min.get(), &integer), 0);
	EXPECT_EQ(integer, std::numeric_limits<int64_t>::min());
	EXPECT_EQ(json_read_int(signed_overflow.get(), &integer), -ERANGE);
	EXPECT_EQ(json_read_uint(unsigned_overflow.get(), &unsigned_integer), -ERANGE);
	ASSERT_EQ(json_read_uint(unsigned_last.get(), &unsigned_integer), 0);
	EXPECT_EQ(unsigned_integer, static_cast<uint64_t>(json_number_value(unsigned_last.get())));
}

TEST(JsonLiteral, DuplicateKeysKeepOrderAndLookupReplaceRemoveUseTheFirstMatch)
{
	JsonPtr root(json_parse("{\"Key\":1,\"Key\":2,\"key\":3}"));
	ASSERT_NE(root, nullptr);
	EXPECT_EQ(json_size(root.get()), 3u);
	EXPECT_EQ(json_number_value(json_member(root.get(), "Key")), 1);
	EXPECT_EQ(json_number_value(json_member(root.get(), "key")), 3);
	EXPECT_EQ(json_number_value(json_member_ci(root.get(), "KEY")), 1);
	JsonPtr replacement(json_number(4));
	ASSERT_EQ(json_object_replace_ci(root.get(), "kEy", replacement.get()), 0);
	replacement.release();
	EXPECT_EQ(json_number_value(json_member_ci(root.get(), "KEY")), 4);
	ASSERT_EQ(json_object_remove_ci(root.get(), "KEY"), 0);
	EXPECT_EQ(json_number_value(json_member(root.get(), "Key")), 2);
	JsonPtr missing(json_number(5));
	EXPECT_EQ(json_object_replace(root.get(), "absent", missing.get()), -ENOENT);
	EXPECT_EQ(json_number_value(missing.get()), 5);
	expect_round_trip(root.get());
}

TEST(JsonLiteral, LiteralSpecialAndEmptyKeysDoNotBecomePaths)
{
	JsonPtr root(json_object());
	ASSERT_NE(json_add_string(root.get(), "a.b[0]", "literal"), nullptr);
	ASSERT_NE(json_add_number(root.get(), "", 7), nullptr);
	EXPECT_STREQ(json_string_value(json_member(root.get(), "a.b[0]")), "literal");
	EXPECT_EQ(json_number_value(json_member(root.get(), "")), 7);
	EXPECT_EQ(json_get(root.get(), "a.b[0]"), nullptr);
	expect_round_trip(root.get());
}

TEST(JsonLiteral, ScalarUpdatesValidateBeforeChangingTheExistingValue)
{
	JsonPtr text(json_string("original"));
	ASSERT_NE(json_string_update(text.get(), "updated"), nullptr);
	EXPECT_STREQ(json_string_value(text.get()), "updated");
	const char invalid[] = {static_cast<char>(0xff), '\0'};
	EXPECT_EQ(json_string_update(text.get(), invalid), nullptr);
	EXPECT_STREQ(json_string_value(text.get()), "updated");
	JsonPtr number(json_number(1.25));
	ASSERT_EQ(json_number_update(number.get(), -0.5), 0);
	EXPECT_EQ(json_number_value(number.get()), -0.5);
	EXPECT_EQ(json_number_update(number.get(), std::numeric_limits<double>::infinity()),
		  -ERANGE);
	EXPECT_EQ(json_number_value(number.get()), -0.5);
	EXPECT_EQ(json_number_update(text.get(), 7), -EINVAL);
	expect_round_trip(text.get());
	expect_round_trip(number.get());
}

TEST(JsonOwnership, AttachTransfersIdentityAndRejectsAlreadyAttachedNodes)
{
	JsonPtr first(json_object());
	JsonPtr second(json_object());
	json *child = json_string("child");
	ASSERT_NE(child, nullptr);
	ASSERT_EQ(json_object_add(first.get(), "child", child), 0);
	EXPECT_EQ(json_member(first.get(), "child"), child);
	std::string unchanged = snapshot(first.get());
	EXPECT_EQ(json_object_add(second.get(), "alias", child), -EINVAL);
	EXPECT_EQ(snapshot(first.get()), unchanged);
	EXPECT_EQ(json_size(second.get()), 0u);
	EXPECT_EQ(json_detach(second.get(), child), nullptr);
	EXPECT_EQ(json_object_detach(first.get(), "child"), child);
	EXPECT_EQ(json_size(first.get()), 0u);
	EXPECT_EQ(json_next(child), nullptr);
	EXPECT_EQ(json_prev(child), nullptr);
	ASSERT_EQ(json_object_add(second.get(), "moved", child), 0);
	expect_round_trip(second.get());
}

TEST(JsonOwnership, FreeingAttachedFirstMiddleAndLastNodesKeepsBothTreesConsistent)
{
	JsonPtr root(json_parse("[0,1,2,3,4]"));
	ASSERT_NE(root, nullptr);
	json_free(json_at(root.get(), 0));
	EXPECT_EQ(snapshot(root.get()), "[1,2,3,4]");
	json_free(json_at(root.get(), 1));
	EXPECT_EQ(snapshot(root.get()), "[1,3,4]");
	json_free(json_at(root.get(), 2));
	EXPECT_EQ(snapshot(root.get()), "[1,3]");
	EXPECT_EQ(json_size(root.get()), 2u);
	EXPECT_EQ(json_prev(json_first(root.get())), nullptr);
	EXPECT_EQ(json_next(json_at(root.get(), 1)), nullptr);
	EXPECT_EQ(json_prev(json_at(root.get(), 1)), json_first(root.get()));
	expect_round_trip(root.get());
	JsonPtr object(json_parse("{\"a\":1,\"b\":2,\"c\":3}"));
	json_free(json_member(object.get(), "b"));
	EXPECT_EQ(snapshot(object.get()), "{\"a\":1,\"c\":3}");
	expect_round_trip(object.get());
}

TEST(JsonOwnership, AssigningAnAttachedNodeKeepsItsIdentityAndParentLinks)
{
	JsonPtr root(json_parse("{\"a\":{\"old\":1},\"b\":2}"));
	ASSERT_NE(root, nullptr);
	json *target = json_member(root.get(), "a");
	json *owned = json_parse("[1,2]");
	ASSERT_NE(owned, nullptr);
	ASSERT_EQ(json_assign_take(target, &owned), 0);
	EXPECT_EQ(owned, nullptr);
	EXPECT_EQ(json_member(root.get(), "a"), target);
	EXPECT_STREQ(json_key(target), "a");
	EXPECT_EQ(snapshot(root.get()), "{\"a\":[1,2],\"b\":2}");
	EXPECT_EQ(json_number_value(json_at(target, 1)), 2);
	JsonPtr replacement(json_number(3));
	ASSERT_EQ(json_array_replace(target, 0, replacement.get()), 0);
	replacement.release();
	EXPECT_EQ(snapshot(root.get()), "{\"a\":[3,2],\"b\":2}");
	JsonPtr missing(json_null());
	EXPECT_EQ(json_array_replace(target, 2, missing.get()), -ENOENT);
	EXPECT_EQ(json_count(target), 2);
	size_t visited = 0;
	json *item;
	json_foreach (item, target) {
		EXPECT_EQ(item, json_at(target, static_cast<int>(visited)));
		visited++;
	}
	EXPECT_EQ(visited, 2u);
	expect_round_trip(root.get());
}

TEST(JsonOwnership, RejectsCyclesSelfAssignmentAndInvalidOwnershipAliases)
{
	JsonPtr root(json_object());
	json *child = json_add_object(root.get(), "child");
	ASSERT_NE(child, nullptr);
	EXPECT_EQ(json_object_add(root.get(), "self", root.get()), -EINVAL);
	EXPECT_EQ(json_object_add(child, "ancestor", root.get()), -EINVAL);
	json *alias = root.get();
	std::string unchanged = snapshot(root.get());
	EXPECT_EQ(json_set_take(root.get(), "inside", &alias), -EINVAL);
	EXPECT_EQ(alias, root.get());
	EXPECT_EQ(snapshot(root.get()), unchanged);
	alias = child;
	EXPECT_EQ(json_append_take(root.get(), "items", &alias), -EINVAL);
	EXPECT_EQ(alias, child);
	EXPECT_EQ(snapshot(root.get()), unchanged);
	alias = root.get();
	EXPECT_EQ(json_assign_take(root.get(), &alias), -EINVAL);
	EXPECT_EQ(alias, root.get());
	EXPECT_EQ(json_validate_take(nullptr, root.get()), 0);
	EXPECT_EQ(json_validate_take(nullptr, child), -EINVAL);
	EXPECT_EQ(json_validate_take(nullptr, nullptr), -EINVAL);
	EXPECT_EQ(json_assign_take(nullptr, &alias), -EINVAL);
	EXPECT_EQ(alias, root.get());
}

TEST(JsonOwnership, AttachmentChecksCombinedDepthAndFailureRetainsOwnership)
{
	std::string text = nested_document(JSON_MAX_DEPTH);
	JsonPtr child(json_parse(text.c_str()));
	JsonPtr root(json_array());
	ASSERT_NE(child, nullptr);
	EXPECT_EQ(json_array_add(root.get(), child.get()), -E2BIG);
	EXPECT_EQ(json_size(root.get()), 0u);
	expect_round_trip(child.get());
	text = nested_document(JSON_MAX_DEPTH - 1);
	JsonPtr allowed(json_parse(text.c_str()));
	ASSERT_NE(allowed, nullptr);
	ASSERT_EQ(json_array_add(root.get(), allowed.get()), 0);
	allowed.release();
	expect_round_trip(root.get());
}

TEST(JsonPath, CreatesOnlyTheRequiredObjectAndArrayBranches)
{
	JsonPtr root(json_object());
	ASSERT_EQ(json_set(root.get(), "user.name", "Bob"), 0);
	ASSERT_EQ(json_set(root.get(), "user.age", 26), 0);
	ASSERT_EQ(json_append(root.get(), "user.tags", "first"), 0);
	ASSERT_EQ(json_append(root.get(), "user.tags", true), 0);
	ASSERT_EQ(json_set(root.get(), "user.tags[2].name", "third"), 0);
	EXPECT_STREQ(json_str(root.get(), "user.name"), "Bob");
	EXPECT_EQ(json_int(root.get(), "user.age"), 26);
	EXPECT_STREQ(json_str(root.get(), "user.tags[0]"), "first");
	EXPECT_TRUE(json_bool(root.get(), "user.tags[1]"));
	EXPECT_STREQ(json_str(root.get(), "user.tags[2].name"), "third");
	ASSERT_EQ(json_set(root.get(), "user.tags[0]", "replaced"), 0);
	EXPECT_EQ(json_size(json_get(root.get(), "user.tags")), 3u);
	expect_round_trip(root.get());
	JsonPtr array(json_array());
	ASSERT_EQ(json_set(array.get(), "[0].name", "root array"), 0);
	EXPECT_STREQ(json_str(array.get(), "[0].name"), "root array");
}

TEST(JsonPath, SupportsEscapedBareKeysQuotedKeysUnicodeAndEmptyLiteralKeys)
{
	JsonPtr root(json_object());
	ASSERT_EQ(json_set(root.get(), "literal\\.key", 1), 0);
	ASSERT_EQ(json_set(root.get(), "bracket\\[0\\]", 2), 0);
	ASSERT_EQ(json_set(root.get(), "slash\\\\key", 3), 0);
	ASSERT_EQ(json_set(root.get(), "[\"\"]", 4), 0);
	ASSERT_EQ(json_set(root.get(), "[\"quote\\\"key\"]", 5), 0);
	ASSERT_EQ(json_set(root.get(), "[\"\\u96ea\"]", 6), 0);
	EXPECT_EQ(json_number_value(json_member(root.get(), "literal.key")), 1);
	EXPECT_EQ(json_int(root.get(), "[\"literal.key\"]"), 1);
	EXPECT_EQ(json_int(root.get(), "[\"bracket[0]\"]"), 2);
	EXPECT_EQ(json_number_value(json_member(root.get(), "slash\\key")), 3);
	EXPECT_EQ(json_int(root.get(), "[\"\"]"), 4);
	EXPECT_EQ(json_number_value(json_member(root.get(), "quote\"key")), 5);
	EXPECT_EQ(json_number_value(json_member(root.get(), "雪")), 6);
	expect_round_trip(root.get());
}

TEST(JsonPath, EmptyAndNullPathsReadAndReplaceTheRootInPlace)
{
	JsonPtr root(json_object());
	json *identity = root.get();
	EXPECT_EQ(json_get(root.get(), ""), identity);
	EXPECT_EQ(json_get(root.get(), nullptr), identity);
	ASSERT_EQ(json_set(root.get(), "", 26), 0);
	EXPECT_EQ(root.get(), identity);
	EXPECT_EQ(json_int(root.get(), ""), 26);
	ASSERT_EQ(json_set(root.get(), nullptr, "root"), 0);
	EXPECT_STREQ(json_str(root.get(), nullptr), "root");
	ASSERT_EQ(json_set(root.get(), "", JSON_NULL), 0);
	EXPECT_TRUE(json_is_null(root.get(), ""));
	EXPECT_EQ(json_append(root.get(), "", "item"), -EINVAL);
	EXPECT_EQ(json_erase(root.get(), ""), -EINVAL);
	EXPECT_EQ(json_erase(root.get(), nullptr), -EINVAL);
	JsonPtr array(json_array());
	ASSERT_EQ(json_append(array.get(), nullptr, 7), 0);
	EXPECT_EQ(snapshot(array.get()), "[7]");
}

TEST(JsonPath, RejectsInvalidGrammarOverflowHolesAndScalarParentsAtomically)
{
	JsonPtr root(json_parse("{\"stable\":1,\"scalar\":0,\"items\":[1]}"));
	const char *invalid_paths[] = {
		".",
		"a.",
		"a..b",
		"a[",
		"a[]",
		"a[-1]",
		"a[+1]",
		"a[1.5]",
		"a.[0]",
		"a[0]b",
		"\\q",
		"a\\",
		"[\"unterminated]",
		"[\"bad\\x\"]",
	};
	std::string unchanged = snapshot(root.get());
	for (const char *path : invalid_paths) {
		SCOPED_TRACE(path);
		EXPECT_EQ(json_set(root.get(), path, 7), -EINVAL);
		EXPECT_EQ(snapshot(root.get()), unchanged);
		EXPECT_EQ(json_get(root.get(), path), nullptr);
	}
	EXPECT_EQ(json_set(root.get(), "a[999999999999999999999999999999]", 7), -ERANGE);
	EXPECT_EQ(json_set(root.get(), "a[2].name", "hole"), -ERANGE);
	EXPECT_EQ(json_set(root.get(), "items[2]", 7), -ERANGE);
	EXPECT_EQ(json_set(root.get(), "scalar.name", "wrong type"), -EINVAL);
	EXPECT_EQ(json_append(root.get(), "scalar", 7), -EINVAL);
	EXPECT_EQ(snapshot(root.get()), unchanged);
	EXPECT_EQ(json_set(static_cast<json *>(nullptr), "name", "value"), -EINVAL);
}

TEST(JsonPath, NodeArgumentsAreDeepClonedIncludingTheRootAsASource)
{
	JsonPtr root(json_object());
	JsonPtr source(json_parse("{\"inner\":{\"name\":\"before\"}}"));
	ASSERT_EQ(json_set(root.get(), "copy", source.get()), 0);
	EXPECT_NE(json_get(root.get(), "copy"), source.get());
	ASSERT_EQ(json_set(source.get(), "inner.name", "after"), 0);
	EXPECT_STREQ(json_str(root.get(), "copy.inner.name"), "before");
	ASSERT_EQ(json_set(root.get(), "attached", json_get(root.get(), "copy")), 0);
	ASSERT_EQ(json_set(root.get(), "snapshot", root.get()), 0);
	EXPECT_EQ(json_get(root.get(), "snapshot.snapshot"), nullptr);
	EXPECT_STREQ(json_str(root.get(), "snapshot.attached.inner.name"), "before");
	std::string unchanged = snapshot(root.get());
	ASSERT_EQ(json_set(root.get(), "", root.get()), 0);
	EXPECT_EQ(snapshot(root.get()), unchanged);
	expect_round_trip(root.get());
}

TEST(JsonPath, TakeClearsOnlySuccessfulOwnershipAndRejectsAliases)
{
	JsonPtr root(json_object());
	json *owned = json_parse("{\"name\":\"owned\"}");
	ASSERT_NE(owned, nullptr);
	json *identity = owned;
	EXPECT_EQ(json_set_take(root.get(), "invalid[2]", &owned), -ERANGE);
	EXPECT_EQ(owned, identity);
	EXPECT_EQ(json_size(root.get()), 0u);
	ASSERT_EQ(json_set_take(root.get(), "value", &owned), 0);
	EXPECT_EQ(owned, nullptr);
	EXPECT_STREQ(json_str(root.get(), "value.name"), "owned");
	owned = json_number(7);
	ASSERT_EQ(json_append_take(root.get(), "items", &owned), 0);
	EXPECT_EQ(owned, nullptr);
	EXPECT_EQ(json_int(root.get(), "items[0]"), 7);
	owned = root.get();
	std::string unchanged = snapshot(root.get());
	EXPECT_EQ(json_set_take(root.get(), "", &owned), -EINVAL);
	EXPECT_EQ(owned, root.get());
	EXPECT_EQ(snapshot(root.get()), unchanged);
	EXPECT_EQ(json_set_take(root.get(), "x", nullptr), -EINVAL);
	owned = nullptr;
	EXPECT_EQ(json_append_take(root.get(), "items", &owned), -EINVAL);
}

TEST(JsonPath, PathDepthLimitIncludesTheRootAndTheScalarLeaf)
{
	JsonPtr root(json_object());
	std::string allowed = deep_path(JSON_MAX_DEPTH);
	ASSERT_EQ(json_set(root.get(), allowed.c_str(), 1), 0);
	EXPECT_EQ(json_int(root.get(), allowed.c_str()), 1);
	expect_round_trip(root.get());
	JsonPtr empty(json_object());
	std::string excessive = deep_path(JSON_MAX_DEPTH + 1);
	EXPECT_EQ(json_set(empty.get(), excessive.c_str(), 1), -E2BIG);
	EXPECT_EQ(snapshot(empty.get()), "{}");
	std::string child_text = nested_document(JSON_MAX_DEPTH);
	JsonPtr child(json_parse(child_text.c_str()));
	std::string unchanged = snapshot(child.get());
	json *owned = child.release();
	EXPECT_EQ(json_set_take(empty.get(), "child", &owned), -E2BIG);
	ASSERT_NE(owned, nullptr);
	EXPECT_EQ(snapshot(owned), unchanged);
	json_free(owned);
	EXPECT_EQ(snapshot(empty.get()), "{}");
}

TEST(JsonPath, EraseRemovesOnlyTheSelectedMemberOrArraySlot)
{
	JsonPtr root(json_parse("{\"a\":{\"x\":1,\"y\":2},\"items\":[0,1,2]}"));
	ASSERT_EQ(json_erase(root.get(), "a.x"), 0);
	EXPECT_FALSE(json_exists(root.get(), "a.x"));
	EXPECT_EQ(json_int(root.get(), "a.y"), 2);
	ASSERT_EQ(json_erase(root.get(), "items[1]"), 0);
	EXPECT_EQ(json_int(root.get(), "items[1]"), 2);
	EXPECT_EQ(json_size(json_get(root.get(), "items")), 2u);
	std::string unchanged = snapshot(root.get());
	EXPECT_EQ(json_erase(root.get(), "a.absent"), -ENOENT);
	EXPECT_EQ(json_erase(root.get(), "items[2]"), -ENOENT);
	EXPECT_EQ(snapshot(root.get()), unchanged);
	expect_round_trip(root.get());
}

TEST(JsonMutation, SeededAddReplaceRemoveAndPathEditsAlwaysRemainRoundTrippable)
{
	JsonPtr root(json_object());
	json *array = json_add_array(root.get(), "items");
	ASSERT_NE(array, nullptr);
	uint64_t state = UINT64_C(0xbb67ae8584caa73b);

	for (size_t i = 0; i < 48; i++) {
		state = state * UINT64_C(6364136223846793005) + 1;
		int64_t number = static_cast<int64_t>(state % UINT64_C(1000000000000000));
		JsonPtr value(json_number(static_cast<double>(number)));
		ASSERT_EQ(json_array_add(array, value.get()), 0);
		value.release();
		expect_round_trip(root.get());
		std::string key = "key." + std::to_string(i);
		ASSERT_NE(json_add_number(root.get(), key.c_str(), static_cast<double>(number)),
			  nullptr);
		expect_round_trip(root.get());
		JsonPtr replacement(json_string("雪\\\"\n"));
		ASSERT_EQ(json_object_replace(root.get(), key.c_str(), replacement.get()), 0);
		replacement.release();
		expect_round_trip(root.get());
		ASSERT_EQ(json_object_remove(root.get(), key.c_str()), 0);
		expect_round_trip(root.get());
		if (json_size(array) > 3) {
			ASSERT_EQ(json_array_remove(array, 1), 0);
			expect_round_trip(root.get());
		}
		std::string unchanged = snapshot(root.get());
		JsonPtr missing(json_null());
		EXPECT_EQ(json_object_replace(root.get(), "absent", missing.get()), -ENOENT);
		EXPECT_EQ(snapshot(root.get()), unchanged);
	}
}

#else
namespace
{
struct TrackedAllocations {
	std::array<void *, 4096> slots = {};
	size_t count = 0;

	bool insert(void *pointer)
	{
		for (void *&slot : slots) {
			if (!slot) {
				slot = pointer;
				count++;
				return true;
			}
		}
		ADD_FAILURE() << "Fault allocator tracking capacity exhausted";
		return false;
	}

	size_t erase(void *pointer)
	{
		if (!pointer)
			return 0;
		for (void *&slot : slots) {
			if (slot == pointer) {
				slot = nullptr;
				count--;
				return 1;
			}
		}
		return 0;
	}

	bool empty() const
	{
		return count == 0;
	}
	size_t size() const
	{
		return count;
	}
};

struct FaultAllocator {
	int64_t fail_at = -1;
	int64_t calls = 0;
	bool injected = false;
	TrackedAllocations live;
};

FaultAllocator *allocation_context()
{
	const char *address = std::getenv("MORPH_JSON_TEST_ALLOC_CONTEXT");
	if (!address)
		return nullptr;
	char *end = nullptr;
	unsigned long long numeric = std::strtoull(address, &end, 16);
	if (!end || *end)
		return nullptr;
	return reinterpret_cast<FaultAllocator *>(static_cast<uintptr_t>(numeric));
}

bool fail_allocation()
{
	FaultAllocator *context = allocation_context();
	if (!context)
		return false;
	FaultAllocator &allocator = *context;
	int64_t current = allocator.calls++;
	if (allocator.fail_at >= 0 && current >= allocator.fail_at) {
		allocator.injected = true;
		errno = ENOMEM;
		return true;
	}
	return false;
}

void arm_fault(size_t index)
{
	FaultAllocator &allocator = *allocation_context();
	allocator.calls = 0;
	allocator.injected = false;
	allocator.fail_at = static_cast<int64_t>(index);
}

void disarm_fault()
{
	FaultAllocator *allocator = allocation_context();
	if (allocator)
		allocator->fail_at = -1;
}

class JsonOom : public ::testing::Test
{
      protected:
	FaultAllocator allocator;
	void SetUp() override
	{
		char address[2 + sizeof(uintptr_t) * 2 + 1];
		std::snprintf(address, sizeof(address), "%p", static_cast<void *>(&allocator));
		ASSERT_EQ(setenv("MORPH_JSON_TEST_ALLOC_CONTEXT", address, 1), 0);
		disarm_fault();
		ASSERT_TRUE(allocator.live.empty());
	}
	void TearDown() override
	{
		disarm_fault();
		EXPECT_TRUE(allocator.live.empty())
			<< allocator.live.size() << " leaked allocations";
		EXPECT_EQ(unsetenv("MORPH_JSON_TEST_ALLOC_CONTEXT"), 0);
	}
};
} // namespace

extern "C" void *morph_json_test_malloc(size_t size)
{
	if (fail_allocation())
		return nullptr;
	void *result = std::malloc(size ? size : 1);
	FaultAllocator *context = allocation_context();
	if (result && context && !context->live.insert(result)) {
		std::free(result);
		return nullptr;
	}
	return result;
}

extern "C" void *morph_json_test_calloc(size_t count, size_t size)
{
	if (fail_allocation())
		return nullptr;
	void *result = std::calloc(count ? count : 1, size ? size : 1);
	FaultAllocator *context = allocation_context();
	if (result && context && !context->live.insert(result)) {
		std::free(result);
		return nullptr;
	}
	return result;
}

extern "C" void morph_json_test_free(void *pointer)
{
	FaultAllocator *context = allocation_context();
	if (context)
		context->live.erase(pointer);
	std::free(pointer);
}

extern "C" void *morph_json_test_realloc(void *pointer, size_t size)
{
	if (fail_allocation())
		return nullptr;
	FaultAllocator *context = allocation_context();
	bool tracked = context && context->live.erase(pointer) != 0;
	void *result = std::realloc(pointer, size ? size : 1);
	if (result && context) {
		if (!context->live.insert(result)) {
			std::free(result);
			return nullptr;
		}
	} else if (tracked) {
		context->live.insert(pointer);
	}
	return result;
}

TEST_F(JsonOom, ParseAndCloneReleaseEveryPartialBackendAndWrapperTree)
{
	const char *document = "{\"a\":[1,true,null,{\"name\":\"雪\"}],\"b\":{\"c\":2}}";
	bool completed = false;
	for (size_t index = 0; index < 2048; index++) {
		SCOPED_TRACE(index);
		struct json_error error = {};
		arm_fault(index);
		json *parsed = json_parse_n(document, std::strlen(document), &error);
		bool injected = allocator.injected;
		disarm_fault();
		if (parsed)
			expect_round_trip(parsed);
		else
			EXPECT_EQ(error.code, -ENOMEM);
		json_free(parsed);
		EXPECT_TRUE(allocator.live.empty());
		if (!injected) {
			ASSERT_NE(parsed, nullptr);
			completed = true;
			break;
		}
	}
	ASSERT_TRUE(completed);
	JsonPtr original(json_parse(document));
	ASSERT_NE(original, nullptr);
	std::string unchanged = snapshot(original.get());
	size_t baseline = allocator.live.size();
	completed = false;
	for (size_t index = 0; index < 2048; index++) {
		arm_fault(index);
		json *copy = json_clone(original.get(), true);
		bool injected = allocator.injected;
		disarm_fault();
		if (copy)
			EXPECT_TRUE(json_equal(copy, original.get(), true));
		json_free(copy);
		EXPECT_EQ(snapshot(original.get()), unchanged);
		EXPECT_EQ(allocator.live.size(), baseline);
		if (!injected) {
			ASSERT_NE(copy, nullptr);
			completed = true;
			break;
		}
	}
	EXPECT_TRUE(completed);
}

TEST_F(JsonOom, MissingDeepBranchesAndRootReplacementFailAtomically)
{
	for (const std::string &path : {deep_path(32), std::string()}) {
		bool completed = false;
		for (size_t index = 0; index < 2048; index++) {
			SCOPED_TRACE(path);
			SCOPED_TRACE(index);
			JsonPtr root(json_parse("{\"stable\":[1,2,3]}"));
			JsonPtr source(json_parse("{\"copy\":[true,\"value\"]}"));
			ASSERT_NE(root, nullptr);
			ASSERT_NE(source, nullptr);
			std::string unchanged = snapshot(root.get());
			std::string source_before = snapshot(source.get());
			size_t baseline = allocator.live.size();
			arm_fault(index);
			int rc = json_set(root.get(), path.c_str(), source.get());
			bool injected = allocator.injected;
			disarm_fault();
			if (rc) {
				EXPECT_EQ(rc, -ENOMEM);
				EXPECT_EQ(snapshot(root.get()), unchanged);
				EXPECT_EQ(allocator.live.size(), baseline);
			} else {
				expect_round_trip(root.get());
			}
			EXPECT_EQ(snapshot(source.get()), source_before);
			if (!injected) {
				EXPECT_EQ(rc, 0);
				completed = true;
				break;
			}
		}
		EXPECT_TRUE(completed);
	}
}

TEST_F(JsonOom, AppendAndTakeRetainBothTheOriginalTreeAndOwnershipOnFailure)
{
	struct Operation {
		bool append;
		const char *path;
	};
	const Operation operations[] = {
		{false, "branch.deep.value"},
		{false, ""},
		{true, "items"},
		{true, "branch.deep.items"},
	};
	for (const Operation &operation : operations) {
		bool completed = false;
		for (size_t index = 0; index < 2048; index++) {
			SCOPED_TRACE(index);
			JsonPtr root(json_parse("{\"items\":[1,2],\"stable\":true}"));
			json *owned = json_parse("{\"nested\":[\"value\",false]}");
			ASSERT_NE(root, nullptr);
			ASSERT_NE(owned, nullptr);
			json *identity = owned;
			std::string unchanged = snapshot(root.get());
			std::string source_before = snapshot(owned);
			size_t baseline = allocator.live.size();
			arm_fault(index);
			int rc = operation.append
					 ? json_append_take(root.get(), operation.path, &owned)
					 : json_set_take(root.get(), operation.path, &owned);
			bool injected = allocator.injected;
			disarm_fault();
			if (rc) {
				EXPECT_EQ(rc, -ENOMEM);
				EXPECT_EQ(owned, identity);
				EXPECT_EQ(snapshot(root.get()), unchanged);
				EXPECT_EQ(snapshot(owned), source_before);
				EXPECT_EQ(allocator.live.size(), baseline);
			} else {
				EXPECT_EQ(owned, nullptr);
				expect_round_trip(root.get());
			}
			json_free(owned);
			if (!injected) {
				EXPECT_EQ(rc, 0);
				completed = true;
				break;
			}
		}
		EXPECT_TRUE(completed);
	}
}

TEST_F(JsonOom, LiteralAddAndReplaceRetainOwnershipUntilCommit)
{
	for (bool replace : {false, true}) {
		bool completed = false;
		for (size_t index = 0; index < 2048; index++) {
			JsonPtr root(json_parse("{\"key\":1}"));
			json *owned = json_string("replacement");
			ASSERT_NE(root, nullptr);
			ASSERT_NE(owned, nullptr);
			std::string unchanged = snapshot(root.get());
			size_t baseline = allocator.live.size();
			arm_fault(index);
			int rc = replace ? json_object_replace(root.get(), "key", owned)
					 : json_object_add(root.get(), "new", owned);
			bool injected = allocator.injected;
			disarm_fault();
			if (rc) {
				EXPECT_EQ(rc, -ENOMEM);
				EXPECT_STREQ(json_string_value(owned), "replacement");
				EXPECT_EQ(snapshot(root.get()), unchanged);
				EXPECT_EQ(allocator.live.size(), baseline);
				json_free(owned);
			} else {
				expect_round_trip(root.get());
			}
			if (!injected) {
				EXPECT_EQ(rc, 0);
				completed = true;
				break;
			}
		}
		EXPECT_TRUE(completed);
	}
}

TEST_F(JsonOom, TakingAStringConsumesItEvenWhenTheWrapperCannotBeAllocated)
{
	bool completed = false;
	for (size_t index = 0; index < 2048; index++) {
		char *owned = static_cast<char *>(morph_json_test_malloc(6));
		ASSERT_NE(owned, nullptr);
		std::memcpy(owned, "owned", 6);
		arm_fault(index);
		json *value = json_take_string(owned);
		bool injected = allocator.injected;
		disarm_fault();
		if (value)
			EXPECT_STREQ(json_string_value(value), "owned");
		json_free(value);
		EXPECT_TRUE(allocator.live.empty());
		if (!injected) {
			ASSERT_NE(value, nullptr);
			completed = true;
			break;
		}
	}
	EXPECT_TRUE(completed);
	char *invalid = static_cast<char *>(morph_json_test_malloc(2));
	ASSERT_NE(invalid, nullptr);
	invalid[0] = static_cast<char>(0xff);
	invalid[1] = '\0';
	EXPECT_EQ(json_take_string(invalid), nullptr);
	EXPECT_TRUE(allocator.live.empty());
}

TEST_F(JsonOom, SerializationLeavesTheSourceIntactAndFreesFailedBuffers)
{
	std::string document = "{\"long\":\"" + std::string(1024, 'x') + "\"}";
	JsonPtr root(json_parse(document.c_str()));
	ASSERT_NE(root, nullptr);
	std::string unchanged = snapshot(root.get());
	size_t baseline = allocator.live.size();
	for (int mode = 0; mode < 3; mode++) {
		bool completed = false;
		for (size_t index = 0; index < 2048; index++) {
			SCOPED_TRACE(mode);
			SCOPED_TRACE(index);
			struct arena *arena = mode == 2 ? arena_create(128) : nullptr;
			if (mode == 2)
				ASSERT_NE(arena, nullptr);
			arm_fault(index);
			char *text = mode == 2	 ? json_stringify_arena(arena, root.get())
				     : mode == 1 ? json_stringify_pretty(root.get())
						 : json_stringify(root.get());
			bool injected = allocator.injected;
			disarm_fault();
			if (text) {
				JsonPtr parsed(json_parse(text));
				ASSERT_NE(parsed, nullptr);
				EXPECT_TRUE(json_equal(root.get(), parsed.get(), true));
			}
			bool succeeded = text != nullptr;
			if (mode == 2)
				arena_destroy(arena);
			else
				json_text_free(text);
			EXPECT_EQ(snapshot(root.get()), unchanged);
			EXPECT_EQ(allocator.live.size(), baseline);
			if (!injected) {
				EXPECT_TRUE(succeeded);
				completed = true;
				break;
			}
		}
		EXPECT_TRUE(completed);
	}
}

TEST_F(JsonOom, StringUpdatesDoNotDiscardTheOldTextWhenAllocationFails)
{
	bool completed = false;
	for (size_t index = 0; index < 2048; index++) {
		JsonPtr value(json_string("original"));
		ASSERT_NE(value, nullptr);
		size_t baseline = allocator.live.size();
		arm_fault(index);
		char *updated = json_string_update(value.get(), "a much longer replacement string");
		bool injected = allocator.injected;
		disarm_fault();
		if (!updated) {
			EXPECT_STREQ(json_string_value(value.get()), "original");
			EXPECT_EQ(allocator.live.size(), baseline);
		} else {
			EXPECT_STREQ(updated, "a much longer replacement string");
		}
		if (!injected) {
			ASSERT_NE(updated, nullptr);
			completed = true;
			break;
		}
	}
	EXPECT_TRUE(completed);
}
#endif
