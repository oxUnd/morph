#include <gtest/gtest.h>
#include "agent/tokenizer.h"
#include "agent/context.h"
#include "util/bpe.h"
#include "util/file.h"
#include "util/base64.h"
#include "util/utf8.h"
#include "util/cleanup.h"
#include <chrono>
#include <climits>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <vector>
#include <string.h>
#include <unistd.h>

class TokenizerTest : public ::testing::Test {
protected:
	struct tokenizer *tok;
	void SetUp() override {
		tok = tokenizer_create(NULL, "gpt-4o", 128000);
	}
	void TearDown() override {
		tokenizer_destroy(tok);
	}
};

TEST_F(TokenizerTest, CreateDestroy) {
	EXPECT_NE(tok, nullptr);
	EXPECT_STREQ(tok->model_name, "gpt-4o");
	EXPECT_EQ(tok->context_limit, 128000);
}

TEST_F(TokenizerTest, CountEmpty) {
	EXPECT_EQ(tokenizer_count(tok, ""), 0);
}

TEST_F(TokenizerTest, CountNull) {
	EXPECT_EQ(tokenizer_count(tok, nullptr), 0);
}

TEST_F(TokenizerTest, CountSimpleEnglish) {
	/* "hello" = ~2 tokens in GPT BPE (he + llo or similar) */
	int count = tokenizer_count(tok, "hello");
	EXPECT_GE(count, 1);
	EXPECT_LE(count, 5);
}

TEST_F(TokenizerTest, CountEnglishSentence) {
	/* "hello world" ≈ 2-3 tokens */
	int count = tokenizer_count(tok, "hello world");
	EXPECT_GE(count, 2);
	EXPECT_LE(count, 5);
}

TEST_F(TokenizerTest, CountLongEnglishText) {
	/* ~260 chars, should be roughly 50-70 tokens */
	int count = tokenizer_count(tok,
		"The quick brown fox jumps over the lazy dog. "
		"This is a longer sentence with more words to test "
		"the token counting algorithm. We want to make sure "
		"it handles various English text reasonably well.");
	EXPECT_GE(count, 30);
	EXPECT_LE(count, 100);
}

TEST_F(TokenizerTest, CountChineseCharacters) {
	/* o200k merges common CJK words; fallback estimates characters separately. */
	int count = tokenizer_count(tok, "你好世界");
	if (tok->encoder)
		EXPECT_EQ(count, 2);
	else
		EXPECT_GE(count, 4);
	EXPECT_LE(count, 10);
}

TEST_F(TokenizerTest, CountChineseMixed) {
	/* "你好hello世界" = Chinese + English mixed */
	int cn_only = tokenizer_count(tok, "你好世界");
	int mixed = tokenizer_count(tok, "你好hello世界");
	EXPECT_GT(mixed, cn_only);
}

TEST_F(TokenizerTest, CountChineseWithEnglish) {
	/* "我是一个AI助手" = 6 CJK + 2 ASCII chars */
	int count = tokenizer_count(tok, "我是一个AI助手");
	if (tok->encoder)
		EXPECT_EQ(count, 4);
	else
		EXPECT_GE(count, 6);
	EXPECT_LE(count, 20);
}

TEST_F(TokenizerTest, CountChineseSentence) {
	/* Full Chinese sentence */
	int count = tokenizer_count(tok, "赛博朋克短视频脚本，霓虹灯在雨幕中闪烁");
	EXPECT_GE(count, 10);
	EXPECT_LE(count, 40);
}

TEST_F(TokenizerTest, CountMixedSentence) {
	/* Mix of Chinese and English */
	int cn = tokenizer_count(tok, "使用GPT-4o模型");
	int en = tokenizer_count(tok, "using GPT-4o model");
	/* mixed should be somewhere in between */
	int mixed = tokenizer_count(tok, "使用GPT-4o model进行对话");
	EXPECT_GE(mixed, 3);
}

TEST_F(TokenizerTest, CountPunctuation) {
	/* Punctuation tokens are usually 1 each */
	int count = tokenizer_count(tok, "hello, world! how are you?");
	EXPECT_GE(count, 5);
	EXPECT_LE(count, 12);
}

TEST_F(TokenizerTest, CountNewlines) {
	int count = tokenizer_count(tok, "line1\nline2\nline3");
	EXPECT_GT(count, 0);
}

TEST_F(TokenizerTest, CountRepetitiveEnglish) {
	/* GPT BPE merges common substrings.
	 * "aaa" should be fewer tokens than "abc" */
	int count_aaa = tokenizer_count(tok, "aaa aaa aaa");
	int count_abc = tokenizer_count(tok, "abc def ghi");
	/* Both should be reasonable */
	EXPECT_GT(count_aaa, 0);
	EXPECT_GT(count_abc, 0);
}

TEST_F(TokenizerTest, CountLongChinese) {
	/* 200 Chinese chars ≈ 400 bytes UTF-8 */
	char text[601];
	memset(text, 0, sizeof(text));
	/* Fill with Chinese chars (U+4E2D = 中) */
	int pos = 0;
	for (int i = 0; i < 200 && pos < 598; i++) {
		text[pos++] = (char)0xE4; /* 中 = U+4E2D = E4 B8 AD */
		text[pos++] = (char)0xB8;
		text[pos++] = (char)0xAD;
	}
	text[pos] = '\0';
	int count = tokenizer_estimate_tokens(text);
	EXPECT_GE(count, 100); /* 200 chars * ~1.5 tokens minimum */
	EXPECT_LE(count, 600); /* 200 chars * ~3 tokens max */
}

TEST_F(TokenizerTest, EstimateTokens) {
	int count = tokenizer_estimate_tokens("hello world test");
	EXPECT_GT(count, 0);
}

TEST_F(TokenizerTest, EstimateNull) {
	EXPECT_EQ(tokenizer_estimate_tokens(nullptr), 0);
}

TEST_F(TokenizerTest, DefaultContextLimit) {
	struct tokenizer *t = tokenizer_create(NULL, "test", 0);
	EXPECT_EQ(t->context_limit, 128000);
	tokenizer_destroy(t);
}

TEST_F(TokenizerTest, CreateNullModel) {
	struct tokenizer *t = tokenizer_create(NULL, nullptr, 128000);
	EXPECT_NE(t, nullptr);
	EXPECT_STREQ(t->model_name, "gpt-4o");
	tokenizer_destroy(t);
}

TEST_F(TokenizerTest, ModelNameCl100k) {
	struct tokenizer *t = tokenizer_create(NULL, "cl100k_base", 8192);
	EXPECT_STREQ(t->model_name, "cl100k_base");
	EXPECT_EQ(t->context_limit, 8192);
	tokenizer_destroy(t);
}

TEST_F(TokenizerTest, CountJsonLike) {
	/* JSON-like content (tool args) */
	int count = tokenizer_count(tok, "{\"prompt\": \"hello world\", \"style\": \"realistic\"}");
	EXPECT_GT(count, 5);
	EXPECT_LT(count, 30);
}

TEST_F(TokenizerTest, CountCodeBlock) {
	/* Code snippets have more tokens */
	int count = tokenizer_count(tok, "```python\nprint('hello')\n```");
	EXPECT_GT(count, 5);
}

TEST_F(TokenizerTest, CountWhitespace) {
	EXPECT_EQ(tokenizer_count(tok, " "), 1);
	EXPECT_EQ(tokenizer_count(tok, "   "), 1);
}

TEST_F(TokenizerTest, CountSingleChar) {
	EXPECT_EQ(tokenizer_count(tok, "a"), 1);
	EXPECT_EQ(tokenizer_count(tok, "I"), 1);
}

TEST_F(TokenizerTest, CountEmoji) {
	/* A common emoji is one o200k token, independent of its UTF-8 byte length. */
	int count = tokenizer_count(tok, "😀");
	EXPECT_GE(count, 1);
	EXPECT_LE(count, 4);
}

TEST_F(TokenizerTest, CountNumbers) {
	/* Numbers are typically 1 token if small */
	int count = tokenizer_count(tok, "42");
	EXPECT_GE(count, 1);
	EXPECT_LE(count, 2);
}

TEST_F(TokenizerTest, ReActPromptEstimation) {
	/* Real-world ReAct prompt - should be reasonable */
	const char *prompt = "Thought: The user wants a cyberpunk short video script. "
		"I should generate a script first.\n"
		"Action: test_tool({\"prompt\": \"赛博朋克短视频脚本\", \"style\": \"creative\"})\n"
		"Observation: Generated script about neon lights in rain...\n"
		"Final: Here is your cyberpunk short video script.";
	int count = tokenizer_count(tok, prompt);
	EXPECT_GE(count, 20);
	EXPECT_LE(count, 200);
}

TEST_F(TokenizerTest, BaseModelUsesSameVocabularyAsDatedVariant)
{
	auto *dated = tokenizer_create(NULL, "gpt-4o-2024-08-06", 128000);
	ASSERT_NE(dated, nullptr);
	for (const char *text : {"你好世界", "我是一个AI助手", "hello world", "😀"})
		EXPECT_EQ(tokenizer_count(tok, text), tokenizer_count(dated, text));
	tokenizer_destroy(dated);
}

TEST(TokenizerEstimateTest, LeadingWhitespaceConsumesBudget)
{
	EXPECT_GT(tokenizer_estimate_tokens(std::string(8192, ' ').c_str()), 1000);
	EXPECT_GT(tokenizer_estimate_tokens("\t\t\t\tmessage"), 0);
	EXPECT_GT(tokenizer_estimate_tokens("\xf0\x9f"), 0);
}

TEST(BpeCountTest, CountsAllPiecesBeyondBatchBoundary)
{
	for (auto encoding : {BPE_CL100K_BASE, BPE_O200K_BASE}) {
		auto *encoder = bpe_encoder_create(NULL, encoding, nullptr);
		if (!encoder)
			GTEST_SKIP() << "BPE vocabulary unavailable";
		for (const char *unit : {" word", "123", "\nhello"}) {
			std::string text;
			for (int i = 0; i < 20000; i++)
				text += unit;
			EXPECT_EQ(bpe_count_tokens(encoder, text.c_str()),
				20000 * bpe_count_tokens(encoder, unit));
		}
		bpe_encoder_destroy(encoder);
	}
}

TEST(BpeCountTest, MalformedSuffixDoesNotDisappearFromBudget)
{
	auto *encoder = bpe_encoder_create(NULL, BPE_CL100K_BASE, nullptr);
	if (!encoder)
		GTEST_SKIP() << "BPE vocabulary unavailable";
	std::string text = "hello ";
	int before = bpe_count_tokens(encoder, text.c_str());
	text += std::string(100, static_cast<char>(0xff));
	EXPECT_GE(bpe_count_tokens(encoder, text.c_str()), before + 100);
	bpe_encoder_destroy(encoder);
}

static int reference_bpe_piece_count(const std::string &piece,
				     const std::map<std::string, int> &vocabulary)
{
	std::vector<size_t> boundaries(piece.size() + 1);
	std::iota(boundaries.begin(), boundaries.end(), 0u);
	while (boundaries.size() > 2) {
		int best = INT_MAX;
		size_t selected = 0;
		for (size_t i = 0; i + 2 < boundaries.size(); i++) {
			auto found = vocabulary.find(piece.substr(boundaries[i],
				boundaries[i + 2] - boundaries[i]));
			if (found != vocabulary.end() && found->second < best) {
				best = found->second;
				selected = i;
			}
		}
		if (best == INT_MAX)
			break;
		boundaries.erase(boundaries.begin() + static_cast<std::ptrdiff_t>(selected + 1));
	}
	return static_cast<int>(boundaries.size() - 1);
}

TEST(BpeCountTest, IndexedMergeHeapMatchesTheLeftmostRankReference)
{
	char temp[] = "/tmp/morph-bpe-merges-XXXXXX";
	ASSERT_NE(mkdtemp(temp), nullptr);
	struct VocabularyDirectory {
		std::string path;
		~VocabularyDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	} directory = {temp};
	const std::map<std::string, int> vocabulary = {
		{"aa", 2}, {"ab", 2}, {"ba", 2}, {"bb", 2},
		{"aaa", 1}, {"aab", 3}, {"aba", 1}, {"abb", 3},
		{"baa", 3}, {"bab", 1}, {"bba", 3}, {"bbb", 1},
		{"aaaa", 4}, {"abab", 0}, {"baba", 0}, {"bbbb", 4},
		{"aaab", 7}, {"bbba", 7}, {"ababa", INT_MAX},
	};
	std::string serialized;
	for (const auto &entry : vocabulary) {
		char *encoded MORPH_CLEANUP(morph_charp_cleanup) = base64_encode_copy(
			reinterpret_cast<const unsigned char *>(entry.first.data()),
			entry.first.size());
		ASSERT_NE(encoded, nullptr);
		serialized += std::string(encoded) + " " + std::to_string(entry.second) + "\n";
	}
	for (auto encoding : {BPE_CL100K_BASE, BPE_O200K_BASE}) {
		std::string path = directory.path + (encoding == BPE_CL100K_BASE ?
			"/cl100k_base.tiktoken" : "/o200k_base.tiktoken");
		ASSERT_EQ(file_write_all(path.c_str(), serialized.data(), serialized.size()), 0);
		std::unique_ptr<struct bpe_encoder, decltype(&bpe_encoder_destroy)> encoder(
			bpe_encoder_create(nullptr, encoding, directory.path.c_str()),
			bpe_encoder_destroy);
		ASSERT_NE(encoder, nullptr);
		for (size_t length : {511u, 512u, 513u, 769u}) {
			for (int pattern = 0; pattern < 4; pattern++) {
				std::string text(length, 'a');
				uint32_t state = 17;
				for (size_t i = 0; i < text.size(); i++) {
					state = state * 1664525u + 1013904223u;
					if (pattern == 1 || (pattern == 2 && i % 2) ||
					    (pattern == 3 && (state >> 31)))
						text[i] = 'b';
				}
				EXPECT_EQ(bpe_count_tokens(encoder.get(), text.c_str()),
					reference_bpe_piece_count(text, vocabulary))
					<< "length=" << length << " pattern=" << pattern;
			}
		}
	}
}

TEST(BpeCountTest, LargeContinuousPiecesRemainExactAndFinishPromptly)
{
	const size_t bytes = 300u * 1024u;
	std::string text(bytes, 'x');
	for (auto encoding : {BPE_CL100K_BASE, BPE_O200K_BASE}) {
		std::unique_ptr<struct bpe_encoder, decltype(&bpe_encoder_destroy)> encoder(
			bpe_encoder_create(nullptr, encoding, nullptr), bpe_encoder_destroy);
		if (!encoder)
			GTEST_SKIP() << "BPE vocabulary unavailable";
		auto start = std::chrono::steady_clock::now();
		int tokens = bpe_count_tokens_n(encoder.get(), text.data(), text.size());
		auto elapsed = std::chrono::steady_clock::now() - start;
		/* Both shipped vocabularies merge eight consecutive x bytes into one token. */
		EXPECT_EQ(tokens, static_cast<int>(bytes / 8));
		EXPECT_LT(elapsed, std::chrono::seconds(10));
	}
}

TEST(BpeCountTest, VocabularyLoadsLongTokensUpToItsRepresentableBoundary)
{
	char temp[] = "/tmp/morph-bpe-long-vocab-XXXXXX";
	ASSERT_NE(mkdtemp(temp), nullptr);
	struct VocabularyDirectory {
		std::string path;
		~VocabularyDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	} directory = {temp};
	std::map<std::string, int> vocabulary;
	for (size_t length = 2; length <= 257; length++)
		vocabulary.emplace(std::string(length, 'x'), static_cast<int>(length));
	/* Powers merge first, then each successive binary suffix joins the prefix. */
	for (size_t length = 2; length <= 32768; length *= 2)
		vocabulary.emplace(std::string(length, 'y'), static_cast<int>(length));
	size_t prefix = 32768;
	for (size_t suffix = 16384; suffix; suffix /= 2) {
		prefix += suffix;
		vocabulary.emplace(std::string(prefix, 'y'), 100000 + static_cast<int>(prefix));
	}
	ASSERT_EQ(prefix, 65535u);
	std::string serialized;
	for (const auto &entry : vocabulary) {
		char *encoded MORPH_CLEANUP(morph_charp_cleanup) = base64_encode_copy(
			reinterpret_cast<const unsigned char *>(entry.first.data()),
			entry.first.size());
		ASSERT_NE(encoded, nullptr);
		serialized += std::string(encoded) + " " + std::to_string(entry.second) + "\n";
	}
	for (auto encoding : {BPE_CL100K_BASE, BPE_O200K_BASE}) {
		std::string path = directory.path + (encoding == BPE_CL100K_BASE ?
			"/cl100k_base.tiktoken" : "/o200k_base.tiktoken");
		ASSERT_EQ(file_write_all(path.c_str(), serialized.data(), serialized.size()), 0);
		std::unique_ptr<struct bpe_encoder, decltype(&bpe_encoder_destroy)> encoder(
			bpe_encoder_create(nullptr, encoding, directory.path.c_str()),
			bpe_encoder_destroy);
		ASSERT_NE(encoder, nullptr);
		EXPECT_EQ(bpe_count_tokens(encoder.get(), std::string(257, 'x').c_str()), 1);
		EXPECT_EQ(bpe_count_tokens(encoder.get(), std::string(65535, 'y').c_str()), 1);
	}
}

TEST(BpeCountTest, VocabularyRejectsTokensBeyondItsRepresentableBoundary)
{
	char temp[] = "/tmp/morph-bpe-oversized-vocab-XXXXXX";
	ASSERT_NE(mkdtemp(temp), nullptr);
	struct VocabularyDirectory {
		std::string path;
		~VocabularyDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	} directory = {temp};
	std::string token(65536, 'x');
	char *encoded MORPH_CLEANUP(morph_charp_cleanup) = base64_encode_copy(
		reinterpret_cast<const unsigned char *>(token.data()), token.size());
	ASSERT_NE(encoded, nullptr);
	std::string serialized = std::string(encoded) + " 0\n";
	for (auto encoding : {BPE_CL100K_BASE, BPE_O200K_BASE}) {
		std::string path = directory.path + (encoding == BPE_CL100K_BASE ?
			"/cl100k_base.tiktoken" : "/o200k_base.tiktoken");
		ASSERT_EQ(file_write_all(path.c_str(), serialized.data(), serialized.size()), 0);
		std::unique_ptr<struct bpe_encoder, decltype(&bpe_encoder_destroy)> encoder(
			bpe_encoder_create(nullptr, encoding, directory.path.c_str()),
			bpe_encoder_destroy);
		EXPECT_EQ(encoder, nullptr);
	}
}

TEST_F(TokenizerTest, DefaultAndBaseReasoningModelsUseTheResolvedVocabulary)
{
	for (const char *name : {static_cast<const char *>(nullptr), "o1", "o3", "o4"}) {
		auto *other = tokenizer_create(NULL, name, 128000);
		ASSERT_NE(other, nullptr);
		for (const char *text : {"你好世界", "我是一个AI助手", "😀"})
			EXPECT_EQ(tokenizer_count(other, text), tokenizer_count(tok, text));
		tokenizer_destroy(other);
	}
}

TEST(BpeCountTest, UnicodeCategoriesDriveCaseAndDigitBoundaries)
{
	char temp[] = "/tmp/morph-bpe-unicode-XXXXXX";
	ASSERT_NE(mkdtemp(temp), nullptr);
	struct VocabularyDirectory {
		std::string path;
		~VocabularyDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	} directory = {temp};
	std::string path = directory.path + "/o200k_base.tiktoken";
	/* Prefix tokens provide every intermediate byte merge up to a whole word. */
	std::string vocabulary;
	std::set<std::string> prefixes;
	int rank = 0;
	for (const char *word : {"БВГ", "ΑΒΓ", "১২৩৪", "৪"}) {
		for (size_t length = 2; length <= strlen(word); length++) {
			if (!prefixes.insert(std::string(word, length)).second)
				continue;
			char *encoded = base64_encode_copy(
				reinterpret_cast<const unsigned char *>(word), length);
			ASSERT_NE(encoded, nullptr);
			vocabulary += std::string(encoded) + " " + std::to_string(rank++) + "\n";
			free(encoded);
		}
	}
	ASSERT_EQ(file_write_all(path.c_str(), vocabulary.data(), vocabulary.size()), 0);
	std::unique_ptr<struct bpe_encoder, decltype(&bpe_encoder_destroy)> encoder(
		bpe_encoder_create(nullptr, BPE_O200K_BASE, directory.path.c_str()),
		bpe_encoder_destroy);
	ASSERT_NE(encoder, nullptr);
	EXPECT_EQ(bpe_count_tokens(encoder.get(), "БВГ"), 1);
	EXPECT_EQ(bpe_count_tokens(encoder.get(), "ΑΒΓ"), 1);
	EXPECT_EQ(bpe_count_tokens(encoder.get(), "১২৩৪"), 2);
}

TEST(TokenizerEstimateTest, UnicodePropertiesReplaceEmojiBlockGuesses)
{
	struct EstimateCase {
		unsigned cp;
		int tokens;
	};
	const EstimateCase cases[] = {
		{0x00a9, 3}, {0x3030, 3}, {0x1f600, 3}, {0x1f1e8, 3},
		{0x1f3fb, 3}, {0xfe0f, 3}, {0xe0100, 3}, {0x200d, 3},
		{0x0301, 2}, {0x2606, 2},
	};
	for (const auto &example : cases) {
		SCOPED_TRACE(example.cp);
		char bytes[4];
		size_t count = utf8_encode_codepoint(example.cp, bytes);
		ASSERT_GT(count, 0u);
		std::string text(bytes, count);
		EXPECT_EQ(tokenizer_estimate_tokens(text.c_str()), example.tokens);
	}
}
