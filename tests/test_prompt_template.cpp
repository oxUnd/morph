#include <gtest/gtest.h>
#include "prompt/template.h"
#include "agent/system_prompt.h"
#include "util/error.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using Variable = morph_prompt_template_variable;

class PromptTemplateTest : public ::testing::Test {
protected:
	std::filesystem::path root;
	morph_prompt_template_error error{};
	std::string rendered;
	void SetUp() override {
		char pattern[] = "/tmp/morph-prompt-template-XXXXXX";
		ASSERT_NE(mkdtemp(pattern), nullptr);
		root = pattern;
	}
	void TearDown() override {
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}
	void write(const std::string &path, const std::string &text) {
		auto target = root / path;
		std::filesystem::create_directories(target.parent_path());
		std::ofstream file(target, std::ios::binary);
		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		ASSERT_TRUE(file.good());
	}
	int render(const std::string &text, const std::vector<Variable> &variables = {}) {
		char *output = nullptr;
		int rc = morph_prompt_template_render(text.c_str(), variables.data(),
			variables.size(), &output, &error);
		return collect(rc, output);
	}
	int render_file(const std::string &entry, const std::vector<Variable> &variables = {}) {
		char *output = nullptr;
		int rc = morph_prompt_template_render_file(root.c_str(), entry.c_str(),
			variables.data(), variables.size(), &output, &error);
		return collect(rc, output);
	}
	int collect(int rc, char *output) {
		rendered.clear();
		if (rc == 0) {
			EXPECT_NE(output, nullptr);
			if (output) rendered = output;
		} else {
			EXPECT_EQ(output, nullptr);
			EXPECT_NE(error.message[0], '\0');
			EXPECT_GE(error.line, 1u);
			EXPECT_GE(error.column, 1u);
		}
		free(output);
		return rc;
	}
};

TEST_F(PromptTemplateTest, PreservesEmptyPlainUtf8AndWhitespace) {
	for (const auto &text : {std::string(), std::string("  hello\r\n\n"),
		std::string("你好，世界 🌏\n}}\n")}) {
		ASSERT_EQ(render(text), 0);
		EXPECT_EQ(rendered, text);
	}
	write("empty.txt", "");
	ASSERT_EQ(render_file("empty.txt"), 0);
	EXPECT_EQ(rendered, "");
}

TEST_F(PromptTemplateTest, SubstitutesVerbatimWithoutHtmlEscapingOrRecursiveEvaluation) {
	ASSERT_EQ(render("{{ role.name }}|{{empty}}|{{value}}", {
		{"role.name", MORPH_PROMPT_TEMPLATE_STRING, "摄影师", 0},
		{"empty", MORPH_PROMPT_TEMPLATE_STRING, "", 0},
		{"value", MORPH_PROMPT_TEMPLATE_STRING, "<x>&\"{{unknown}}\\", 0}}), 0);
	EXPECT_EQ(rendered, "摄影师||<x>&\"{{unknown}}\\");
}

TEST_F(PromptTemplateTest, EscapesLiteralTagsAndBackslashesAndRemovesComments) {
	ASSERT_EQ(render(R"(\{{unknown}}|\\{{name}}|C:\work|{{! comment
ignored }}tail)", {{"name", MORPH_PROMPT_TEMPLATE_STRING, "ok", 0}}), 0);
	EXPECT_EQ(rendered, "{{unknown}}|\\ok|C:\\work|tail");
	ASSERT_EQ(render("\\"), 0);
	EXPECT_EQ(rendered, "\\");
}

TEST_F(PromptTemplateTest, SupportsNestedAndInvertedBooleanSections) {
	const char *text = "{{#on}}A{{^off}}B{{#on}}{{name}}{{/on}}{{/off}}{{/on}}"
		"{{^on}}hidden{{/on}}{{#off}}hidden{{/off}}{{^off}}D{{/off}}";
	ASSERT_EQ(render(text, {{"on", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1},
		{"off", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0},
		{"name", MORPH_PROMPT_TEMPLATE_STRING, "C", 0}}), 0);
	EXPECT_EQ(rendered, "ABCD");
}

TEST_F(PromptTemplateTest, SelectsExplicitIfElseBranchesWithExactWhitespace) {
	const char *text = "begin\n{{ # if\tmodel.text.reasoning_enabled }}reasoning\n"
		"{{ else }}standard\n{{ / if }}end\n";
	for (int enabled : {0, 1, -2}) {
		ASSERT_EQ(render(text, {
			{"model.text.reasoning_enabled", MORPH_PROMPT_TEMPLATE_BOOL,
			 nullptr, enabled}}), 0);
		EXPECT_EQ(rendered, enabled ? "begin\nreasoning\nend\n" :
			"begin\nstandard\nend\n");
	}
	for (const char *empty : {"{{#if flag}}{{/if}}", "{{#if flag}}{{else}}{{/if}}"}) {
		for (int enabled : {0, 1}) {
			ASSERT_EQ(render(empty, {{"flag", MORPH_PROMPT_TEMPLATE_BOOL,
				nullptr, enabled}}), 0);
			EXPECT_TRUE(rendered.empty());
		}
	}
	ASSERT_EQ(render("{{#if flag}}yes{{/if}}",
		{{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}}), 0);
	EXPECT_TRUE(rendered.empty());
}

TEST_F(PromptTemplateTest, NestedIfElseAndLegacySectionsRespectParentSelection) {
	const char *text = "{{#if outer}}A{{#if inner}}1{{else}}2{{/if}}"
		"{{else}}{{^outer}}B{{/outer}}{{#if inner}}3{{else}}4{{/if}}{{/if}}"
		"{{#hidden}}{{#if inner}}leak{{else}}leak{{/if}}{{/hidden}}"
		"{{#if hidden}}{{#if inner}}leak{{else}}leak{{/if}}{{/if}}";
	for (int outer : {0, 1}) {
		for (int inner : {0, 1}) {
			ASSERT_EQ(render(text, {
				{"outer", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, outer},
				{"inner", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, inner},
				{"hidden", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}}), 0);
			EXPECT_EQ(rendered, outer ? (inner ? "A1" : "A2") :
				(inner ? "B3" : "B4"));
		}
	}
}

TEST_F(PromptTemplateTest, ExistingIfAndElseVariableNamesRetainTheirMeaning) {
	ASSERT_EQ(render("{{if}}:{{else}}", {
		{"if", MORPH_PROMPT_TEMPLATE_STRING, "one", 0},
		{"else", MORPH_PROMPT_TEMPLATE_STRING, "two", 0}}), 0);
	EXPECT_EQ(rendered, "one:two");
	ASSERT_EQ(render("{{#if}}{{else}}{{/if}}{{#ifx}}x{{/ifx}}", {
		{"if", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1},
		{"ifx", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1},
		{"else", MORPH_PROMPT_TEMPLATE_STRING, "legacy", 0}}), 0);
	EXPECT_EQ(rendered, "legacyx");
	ASSERT_EQ(render("{{#if flag}}{{#nested}}{{else}}{{/nested}}{{else}}other{{/if}}", {
		{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1},
		{"nested", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1},
		{"else", MORPH_PROMPT_TEMPLATE_STRING, "value", 0}}), 0);
	EXPECT_EQ(rendered, "value");
}

TEST_F(PromptTemplateTest, RejectsMalformedConditionalsWithoutImplicitExpressions) {
	const std::vector<Variable> variables{{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}};
	for (const char *text : {"{{#if }}x{{/if}}", "{{#if missing}}x{{/if}}",
		"{{#if flag}}x{{else}}y{{else}}z{{/if}}", "{{#if flag}}x{{else}}y",
		"{{#if flag}}x{{/flag}}", "{{#if flag}}x{{else if flag}}y{{/if}}",
		"{{#if !flag}}x{{/if}}", "{{#if flag == true}}x{{/if}}",
		"{{#if model == 'x'}}x{{/if}}", "{{#if flag}}{{#flag}}{{/if}}{{/flag}}",
		"{{else}}", "{{/if}}"}) {
		SCOPED_TRACE(text);
		EXPECT_EQ(render(text, variables), MORPH_ERR_FORMAT);
	}
	EXPECT_EQ(render("{{#if flag}}x{{else}}y{{/if}}",
		{{"flag", MORPH_PROMPT_TEMPLATE_STRING, "true", 0}}), MORPH_ERR_FORMAT);
	EXPECT_EQ(render("line\n{{#if flag}}x{{else}}y{{else}}z{{/if}}", variables),
		MORPH_ERR_FORMAT);
	EXPECT_EQ(error.line, 2u);
	EXPECT_EQ(error.column, 23u);
}

TEST_F(PromptTemplateTest, ValidatesBothConditionalBranchesAndAllPartialReferences) {
	for (int enabled : {0, 1}) {
		const std::vector<Variable> variables{
			{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, enabled}};
		EXPECT_EQ(render("{{#if flag}}{{missing}}{{else}}ok{{/if}}", variables),
			MORPH_ERR_FORMAT);
		EXPECT_EQ(render("{{#if flag}}ok{{else}}{{missing}}{{/if}}", variables),
			MORPH_ERR_FORMAT);
		write("main.txt", "{{#if flag}}{{>missing.txt}}{{else}}ok{{/if}}");
		EXPECT_EQ(render_file("main.txt", variables), -ENOENT);
		write("main.txt", "{{#if flag}}ok{{else}}{{>missing.txt}}{{/if}}");
		EXPECT_EQ(render_file("main.txt", variables), -ENOENT);
		write("main.txt", "{{#if flag}}ok{{else}}{{>bad.txt}}{{/if}}");
		write("bad.txt", "{{#if flag}}unclosed");
		EXPECT_EQ(render_file("main.txt", variables), MORPH_ERR_FORMAT);
		EXPECT_STREQ(error.path, "bad.txt");
	}
}

TEST_F(PromptTemplateTest, ComposesRoleAndModelGuidanceFromFreshCallerBindings) {
	write("main.txt", "{{>role.txt}}\n{{#if model.text.reasoning_enabled}}"
		"{{>models/reasoning.txt}}{{else}}{{>models/standard.txt}}{{/if}}");
	write("role.txt", "Role: {{role.name}}");
	write("models/reasoning.txt", "{{#if concise}}short{{else}}expanded{{/if}}");
	write("models/standard.txt", "standard");
	for (int enabled : {0, 1}) {
		ASSERT_EQ(render_file("main.txt", {
			{"role.name", MORPH_PROMPT_TEMPLATE_STRING, "Researcher", 0},
			{"model.text.reasoning_enabled", MORPH_PROMPT_TEMPLATE_BOOL,
			 nullptr, enabled},
			{"concise", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1}}), 0);
		EXPECT_EQ(rendered, enabled ? "Role: Researcher\nshort" :
			"Role: Researcher\nstandard");
	}
	write("models/reasoning.txt", "{{else}}");
	EXPECT_EQ(render_file("main.txt", {
		{"role.name", MORPH_PROMPT_TEMPLATE_STRING, "Researcher", 0},
		{"model.text.reasoning_enabled", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1}}),
		MORPH_ERR_FORMAT);
}

TEST_F(PromptTemplateTest, ConditionalBranchesEnforceDepthAndSelectedOutputLimits) {
	std::string opening, closing;
	for (unsigned i = 0; i < MORPH_PROMPT_TEMPLATE_SECTION_DEPTH; i++) {
		opening += "{{#if flag}}{{else}}";
		closing += "{{/if}}";
	}
	const std::vector<Variable> variables{{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1}};
	ASSERT_EQ(render(opening + closing, variables), 0);
	EXPECT_TRUE(rendered.empty());
	EXPECT_EQ(render("{{#if flag}}" + opening + closing + "{{/if}}", variables), -ELOOP);
	std::string maximum(MORPH_PROMPT_TEMPLATE_OUTPUT_MAX, 'x');
	for (int enabled : {0, 1}) {
		int rc = render("{{#if flag}}ok{{else}}{{x}}x{{/if}}", {
			{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, enabled},
			{"x", MORPH_PROMPT_TEMPLATE_STRING, maximum.c_str(), 0}});
		EXPECT_EQ(rc, enabled ? 0 : -EFBIG);
		if (enabled)
			EXPECT_EQ(rendered, "ok");
	}
}

TEST_F(PromptTemplateTest, CallersCanRefreshBindingsWithoutCachedState) {
	const char *text = "{{assistant.name}}:{{#capabilities.vision}}vision{{/capabilities.vision}}"
		"{{^capabilities.vision}}text{{/capabilities.vision}}";
	ASSERT_EQ(render(text, {{"assistant.name", MORPH_PROMPT_TEMPLATE_STRING, "Morph", 0},
		{"capabilities.vision", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1}}), 0);
	EXPECT_EQ(rendered, "Morph:vision");
	ASSERT_EQ(render(text, {{"assistant.name", MORPH_PROMPT_TEMPLATE_STRING, "Helper", 0},
		{"capabilities.vision", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}}), 0);
	EXPECT_EQ(rendered, "Helper:text");
}

TEST_F(PromptTemplateTest, RejectsUnknownVariablesAndTypeCoercion) {
	EXPECT_EQ(render("{{unknown}}"), MORPH_ERR_FORMAT);
	EXPECT_EQ(render("{{flag}}", {{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1}}),
		MORPH_ERR_FORMAT);
	EXPECT_EQ(render("{{#name}}x{{/name}}",
		{{"name", MORPH_PROMPT_TEMPLATE_STRING, "true", 0}}), MORPH_ERR_FORMAT);
}

TEST_F(PromptTemplateTest, ValidatesReferencesAndSyntaxInInactiveBranches) {
	const std::vector<Variable> variables{{"off", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}};
	EXPECT_EQ(render("{{#off}}{{unknown}}{{/off}}", variables), MORPH_ERR_FORMAT);
	EXPECT_EQ(render("{{#off}}{{#off}}x{{/off}}", variables), MORPH_ERR_FORMAT);
	write("main.txt", "{{#off}}{{>missing.txt}}{{/off}}");
	EXPECT_EQ(render_file("main.txt", variables), -ENOENT);
	write("main.txt", "{{#off}}{{>partial.txt}}{{/off}}");
	write("partial.txt", "{{unknown}}");
	EXPECT_EQ(render_file("main.txt", variables), MORPH_ERR_FORMAT);
	EXPECT_STREQ(error.path, "partial.txt");
}

TEST_F(PromptTemplateTest, ReportsExactSourceAndByteLocationWithoutVariableValues) {
	EXPECT_EQ(render("first\n你好 {{missing}}"), MORPH_ERR_FORMAT);
	EXPECT_STREQ(error.path, "<string>");
	EXPECT_EQ(error.line, 2u);
	EXPECT_EQ(error.column, 8u);
	write("main.txt", "{{>part.txt}}");
	write("part.txt", "x\n  {{unknown}}");
	EXPECT_EQ(render_file("main.txt"), MORPH_ERR_FORMAT);
	EXPECT_STREQ(error.path, "part.txt");
	EXPECT_EQ(error.line, 2u);
	EXPECT_EQ(error.column, 3u);
}

TEST_F(PromptTemplateTest, RejectsMalformedTagsSectionsAndUnsupportedSyntax) {
	for (const char *text : {"{{", "{{}}", "{{ }}", "{{.}}", "{{a..b}}", "{{a.}}",
		"{{1a}}", "{{a-b}}", "{{{a}}}", "{{&a}}", "{{a | upper}}", "{{=x=}}",
		"{{/a}}", "{{#a}}", "{{#a}}{{/b}}", "{{#a}}{{#b}}{{/a}}{{/b}}"}) {
		SCOPED_TRACE(text);
		EXPECT_EQ(render(text, {{"a", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1},
			{"b", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1}}), MORPH_ERR_FORMAT);
	}
}

TEST_F(PromptTemplateTest, ValidatesBindingsEvenWhenUnused) {
	EXPECT_EQ(render("", {{"x", MORPH_PROMPT_TEMPLATE_STRING, "", 0},
		{"x", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}}), -EINVAL);
	EXPECT_EQ(render("", {{nullptr, MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}}), -EINVAL);
	EXPECT_EQ(render("", {{"a..b", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}}), -EINVAL);
	EXPECT_EQ(render("", {{"x", MORPH_PROMPT_TEMPLATE_STRING, nullptr, 0}}), -EINVAL);
	EXPECT_EQ(render("", {{"x", static_cast<morph_prompt_template_type>(42), "", 0}}),
		-EINVAL);
	EXPECT_EQ(render("", {{"x", MORPH_PROMPT_TEMPLATE_STRING, "\xff", 0}}), -EILSEQ);
	std::vector<Variable> too_many(MORPH_PROMPT_TEMPLATE_VARIABLES_MAX + 1,
		{"x", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0});
	EXPECT_EQ(render("", too_many), -EINVAL);
}

TEST_F(PromptTemplateTest, IncludesRootedPartialsWithSharedBindingsAndExactWhitespace) {
	write("entries/main.txt", "start\n{{> common/one.txt }}{{>common/one.txt}}end\n");
	write("common/one.txt", "{{#flag}}{{>common/two.txt}}{{/flag}}\n");
	write("common/two.txt", "{{text}}");
	ASSERT_EQ(render_file("entries/main.txt", {
		{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1},
		{"text", MORPH_PROMPT_TEMPLATE_STRING, "literal {{unknown}}", 0}}), 0);
	EXPECT_EQ(rendered, "start\nliteral {{unknown}}\nliteral {{unknown}}\nend\n");
	EXPECT_EQ(render("{{>common/one.txt}}"), MORPH_ERR_FORMAT);
}

TEST_F(PromptTemplateTest, SectionsCannotCrossFileBoundaries) {
	write("main.txt", "{{#flag}}{{>partial.txt}}{{/flag}}");
	write("partial.txt", "{{/flag}}");
	EXPECT_EQ(render_file("main.txt", {{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 1}}),
		MORPH_ERR_FORMAT);
}

TEST_F(PromptTemplateTest, RejectsUnsafeEntryAndPartialPaths) {
	for (const char *path : {"", "/etc/passwd", "../outside.txt", "./main.txt",
		"a/../main.txt", "a//main.txt", "a/", "a\\b", "a b"}) {
		SCOPED_TRACE(path);
		EXPECT_EQ(render_file(path), -EINVAL);
		write("main.txt", std::string("{{>") + path + "}}");
		EXPECT_EQ(render_file("main.txt"), MORPH_ERR_FORMAT);
	}
	std::string long_path(PATH_MAX, 'a');
	EXPECT_EQ(render_file(long_path), -EINVAL);
	write("main.txt", "{{>" + long_path + "}}");
	EXPECT_EQ(render_file("main.txt"), -ENAMETOOLONG);
}

TEST_F(PromptTemplateTest, RejectsFileAndDirectorySymlinksAndSpecialFiles) {
	write("real/file.txt", "content");
	ASSERT_EQ(symlink("real/file.txt", (root / "alias.txt").c_str()), 0);
	ASSERT_EQ(symlink("real", (root / "alias-dir").c_str()), 0);
	EXPECT_LT(render_file("alias.txt"), 0);
	EXPECT_LT(render_file("alias-dir/file.txt"), 0);
	write("main.txt", "{{>alias.txt}}");
	EXPECT_LT(render_file("main.txt"), 0);
	ASSERT_EQ(mkfifo((root / "pipe.txt").c_str(), 0600), 0);
	EXPECT_EQ(render_file("pipe.txt"), MORPH_ERR_FORMAT);
	EXPECT_EQ(render_file("real"), MORPH_ERR_FORMAT);
}

TEST_F(PromptTemplateTest, DetectsDirectIndirectAndHardLinkIncludeCycles) {
	write("main.txt", "{{>main.txt}}");
	EXPECT_EQ(render_file("main.txt"), -ELOOP);
	write("main.txt", "{{>other.txt}}");
	write("other.txt", "{{>main.txt}}");
	EXPECT_EQ(render_file("main.txt"), -ELOOP);
	write("main.txt", "{{>alias.txt}}");
	ASSERT_EQ(link((root / "main.txt").c_str(), (root / "alias.txt").c_str()), 0);
	EXPECT_EQ(render_file("main.txt"), -ELOOP);
}

TEST_F(PromptTemplateTest, EnforcesIncludeDepthAndFileLoadLimits) {
	for (unsigned i = 0; i < MORPH_PROMPT_TEMPLATE_INCLUDE_DEPTH; i++) {
		write(std::to_string(i) + ".txt", i + 1 == MORPH_PROMPT_TEMPLATE_INCLUDE_DEPTH ?
			"ok" : "{{>" + std::to_string(i + 1) + ".txt}}");
	}
	ASSERT_EQ(render_file("0.txt"), 0);
	EXPECT_EQ(rendered, "ok");
	write(std::to_string(MORPH_PROMPT_TEMPLATE_INCLUDE_DEPTH - 1) + ".txt", "{{>0.txt}}");
	EXPECT_EQ(render_file("0.txt"), -ELOOP);
	write("empty.txt", "");
	std::string many;
	for (unsigned i = 0; i < MORPH_PROMPT_TEMPLATE_FILES_MAX - 1; i++)
		many += "{{>empty.txt}}";
	write("main.txt", many);
	EXPECT_EQ(render_file("main.txt"), 0);
	write("main.txt", many + "{{>empty.txt}}");
	EXPECT_EQ(render_file("main.txt"), -ELOOP);
}

TEST_F(PromptTemplateTest, EnforcesSectionDepthIncludingInactiveSections) {
	std::string opening, closing;
	for (unsigned i = 0; i < MORPH_PROMPT_TEMPLATE_SECTION_DEPTH; i++) {
		opening += "{{#flag}}";
		closing += "{{/flag}}";
	}
	const std::vector<Variable> variables{{"flag", MORPH_PROMPT_TEMPLATE_BOOL, nullptr, 0}};
	EXPECT_EQ(render(opening + closing, variables), 0);
	EXPECT_EQ(render("{{#flag}}" + opening + closing + "{{/flag}}", variables), -ELOOP);
}

TEST_F(PromptTemplateTest, EnforcesPerFileAndAggregateSourceLimits) {
	std::string maximum(MORPH_PROMPT_TEMPLATE_FILE_MAX, 'a');
	EXPECT_EQ(render(maximum), 0);
	EXPECT_EQ(render(maximum + "a"), -EFBIG);
	write("large.txt", maximum);
	EXPECT_EQ(render_file("large.txt"), 0);
	write("main.txt", "{{>large.txt}}{{>large.txt}}{{>large.txt}}{{>large.txt}}");
	EXPECT_EQ(render_file("main.txt"), -EFBIG);
	write("large.txt", maximum + "a");
	EXPECT_EQ(render_file("large.txt"), -EFBIG);
}

TEST_F(PromptTemplateTest, EnforcesOutputAndAggregateBindingLimits) {
	std::string maximum(MORPH_PROMPT_TEMPLATE_OUTPUT_MAX, 'x');
	std::vector<Variable> variables{{"x", MORPH_PROMPT_TEMPLATE_STRING, maximum.c_str(), 0}};
	ASSERT_EQ(render("{{x}}", variables), 0);
	EXPECT_EQ(rendered.size(), maximum.size());
	EXPECT_EQ(render("{{x}}x", variables), -EFBIG);
	variables.push_back({"y", MORPH_PROMPT_TEMPLATE_STRING, "y", 0});
	EXPECT_EQ(render("", variables), -EFBIG);
}

TEST_F(PromptTemplateTest, RejectsInvalidUtf8AndEmbeddedNulFiles) {
	EXPECT_EQ(render("\xff"), -EILSEQ);
	write("main.txt", std::string("a\0b", 3));
	EXPECT_EQ(render_file("main.txt"), -EILSEQ);
	write("main.txt", "\xc0\xaf");
	EXPECT_EQ(render_file("main.txt"), -EILSEQ);
}

TEST_F(PromptTemplateTest, ErrorsClearOutputAndDiagnosticsAndAllowNullDiagnostics) {
	char *output = reinterpret_cast<char *>(1);
	EXPECT_EQ(morph_prompt_template_render(nullptr, nullptr, 0, &output, &error), -EINVAL);
	EXPECT_EQ(output, nullptr);
	EXPECT_EQ(morph_prompt_template_render("", nullptr, 1, &output, nullptr), -EINVAL);
	EXPECT_EQ(output, nullptr);
	EXPECT_EQ(morph_prompt_template_render("", nullptr, 0, nullptr, nullptr), -EINVAL);
	EXPECT_EQ(morph_prompt_template_render_file("/nonexistent-morph-prompt-root", "x.txt",
		nullptr, 0, &output, &error), -ENOENT);
	EXPECT_EQ(output, nullptr);
	EXPECT_EQ(render("valid"), 0);
	EXPECT_EQ(error.path[0], '\0');
	EXPECT_EQ(error.line, 0u);
	EXPECT_EQ(morph_prompt_template_render("ok", nullptr, 0, &output, nullptr), 0);
	free(output);
}

TEST_F(PromptTemplateTest, SnapshotLoadsAsPlainTextThroughExistingPromptLoader) {
	ASSERT_EQ(render("Role: {{role}}\n", {
		{"role", MORPH_PROMPT_TEMPLATE_STRING, "literal {{not_a_template}}", 0}}), 0);
	write("snapshot.md", rendered);
	char *loaded = nullptr;
	ASSERT_EQ(morph_prompt_load(nullptr, (root / "snapshot.md").c_str(), nullptr, &loaded), 0);
	ASSERT_NE(loaded, nullptr);
	EXPECT_STREQ(loaded, "Role: literal {{not_a_template}}");
	free(loaded);
}
