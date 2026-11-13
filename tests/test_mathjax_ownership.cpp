#include <gtest/gtest.h>

extern "C" {
#include "mathjax.h"
#include "parser.h"
}

#include <memory>
#include <string>

namespace
{
struct MathOptions {
	mjx_parse_opts value{};
	~MathOptions()
	{
		mjx_parse_opts_cleanup(&value);
	}
};

using MathParser = std::unique_ptr<mjx_parser, decltype(&mjx_parser_destroy)>;
using MathContext = std::unique_ptr<mjx_ctx, decltype(&mjx_free)>;

mjx_node *pass_environment(mjx_parser *, const char *, mjx_node *content)
{
	return content;
}

bool contains_type(const mjx_node *node, mjx_node_type type)
{
	if (!node)
		return false;
	if (node->type == type)
		return true;
	for (size_t i = 0; i < node->child_count; i++)
		if (contains_type(node->children[i], type))
			return true;
	return false;
}
} /* namespace */

TEST(MathjaxOwnershipTest, OwnedOptionsTransferOnlyAfterAcceptedCreation)
{
	MathOptions options;
	options.value.owns_options = 1;
	options.value.factory = mjx_factory_create();
	options.value.macros = mjx_default_macros(&options.value.macro_count);
	options.value.operators = mjx_default_operators(&options.value.operator_count);
	ASSERT_NE(options.value.factory, nullptr);
	ASSERT_NE(options.value.macros, nullptr);
	ASSERT_NE(options.value.operators, nullptr);
	MathParser parser(mjx_parser_create(&options.value), mjx_parser_destroy);
	ASSERT_NE(parser, nullptr);
	EXPECT_EQ(options.value.factory, nullptr);
	EXPECT_EQ(options.value.macros, nullptr);
	EXPECT_EQ(options.value.operators, nullptr);
	EXPECT_EQ(options.value.macro_count, 0u);
	EXPECT_EQ(options.value.owns_options, 0);
	ASSERT_EQ(mjx_parser_init_environments(parser.get()), 1);
	ASSERT_EQ(mjx_parser_register_macro(parser.get(), "ownedmacro", 0, "z"), 1);
	ASSERT_EQ(mjx_parser_register_env(parser.get(), "ownedenv", pass_environment), 1);
	auto *tree = mjx_parser_parse_latex(parser.get(), "\\ownedmacro + \\frac{1}{2}", 1);
	ASSERT_NE(tree, nullptr);
	EXPECT_TRUE(contains_type(tree, MJX_NODE_MFRAC));
}

TEST(MathjaxOwnershipTest, BorrowedStackOptionsSurviveParserAndLocalRegistrations)
{
	mjx_factory factory{};
	mjx_macro macro{};
	macro.name = const_cast<char *>("borrowedmacro");
	macro.replacement = const_cast<char *>("q");
	mjx_macro *macros[] = {&macro};
	mjx_operator operators[1]{};
	operators[0].symbol = "+";
	const char *names[] = {"borrowedenv"};
	mjx_env_handler handlers[] = {pass_environment};
	mjx_parse_opts options{};
	options.factory = &factory;
	options.macros = macros;
	options.macro_count = 1;
	options.operators = operators;
	options.operator_count = 1;
	options.env_names = names;
	options.env_handlers = handlers;
	options.env_count = 1;
	MathParser parser(mjx_parser_create(&options), mjx_parser_destroy);
	ASSERT_NE(parser, nullptr);
	ASSERT_EQ(mjx_parser_register_macro(parser.get(), "localmacro", 0, "r"), 1);
	ASSERT_EQ(mjx_parser_register_env(parser.get(), "localenv", pass_environment), 1);
	ASSERT_NE(mjx_parser_parse_latex(parser.get(), "\\borrowedmacro + \\localmacro", 0),
		  nullptr);
	EXPECT_EQ(options.factory, &factory);
	EXPECT_EQ(options.macros, macros);
	EXPECT_EQ(options.macro_count, 1u);
	EXPECT_EQ(options.env_names, names);
	EXPECT_EQ(options.env_handlers, handlers);
	EXPECT_EQ(options.env_count, 1u);
	parser.reset();
	EXPECT_EQ(macros[0], &macro);
	EXPECT_STREQ(macro.name, "borrowedmacro");
	EXPECT_STREQ(macro.replacement, "q");
	EXPECT_STREQ(operators[0].symbol, "+");
	EXPECT_STREQ(names[0], "borrowedenv");
	EXPECT_EQ(handlers[0], pass_environment);
	mjx_parse_opts_cleanup(&options);
	EXPECT_STREQ(macro.name, "borrowedmacro");
}

TEST(MathjaxOwnershipTest, RejectedCreationPreservesPartiallyInitializedOwner)
{
	MathOptions options;
	options.value.owns_options = 1;
	options.value.factory = mjx_factory_create();
	options.value.operator_count = 1;
	ASSERT_NE(options.value.factory, nullptr);
	auto *factory = options.value.factory;
	MathParser parser(mjx_parser_create(&options.value), mjx_parser_destroy);
	EXPECT_EQ(parser, nullptr);
	EXPECT_EQ(options.value.factory, factory);
	EXPECT_EQ(options.value.owns_options, 1);
	EXPECT_EQ(options.value.operator_count, 1u);
}

TEST(MathjaxOwnershipTest, RepeatedContextsReleaseDefaultsAndPreviousParseTrees)
{
	for (int i = 0; i < 40; i++) {
		MathContext context(mjx_init(nullptr), mjx_free);
		ASSERT_NE(context, nullptr);
		auto *fraction =
			mjx_parse_latex(context.get(), "\\tfrac{1}{2}+x^2", MJX_STYLE_DISPLAY);
		ASSERT_NE(fraction, nullptr);
		EXPECT_TRUE(contains_type(fraction, MJX_NODE_MFRAC));
		ASSERT_NE(mjx_parse_mathml(context.get(),
					   "<math><mi>x</mi><mo>+</mo><mn>2</mn></math>",
					   MJX_STYLE_INLINE),
			  nullptr);
	}
}
