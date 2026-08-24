#include <gtest/gtest.h>
extern "C" {
#include "ext/ext.h"
#include "ext/install.h"
#include "ext/manifest.h"
#include "util/file.h"
}
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

static int test_cmd(const std::string &cmd)
{
	int status = system(cmd.c_str());
	if (status == -1)
		return -1;
	return status;
}

static void write_text(const std::string &path, const std::string &text)
{
	ASSERT_EQ(file_write_all(path.c_str(), text.c_str(), text.size()), 0);
}

static void mkdir_p(const std::string &path)
{
	ASSERT_EQ(file_ensure_dir(path.c_str()), 0);
}

TEST(ExtSourceParseTest, ParsesGithubSourceWithRefAndSubdir)
{
	struct ext_source src;
	ASSERT_EQ(ext_source_parse(
		"github:morph-ai/exts@v1.2.0//tools/translate", &src), 0);
	EXPECT_STREQ(src.owner, "morph-ai");
	EXPECT_STREQ(src.repo, "exts");
	EXPECT_STREQ(src.ref, "v1.2.0");
	EXPECT_STREQ(src.subdir, "tools/translate");
}

TEST(ExtSourceParseTest, DefaultsRefToHead)
{
	struct ext_source src;
	ASSERT_EQ(ext_source_parse("github:morph-ai/exts//translate", &src), 0);
	EXPECT_STREQ(src.ref, "HEAD");
	EXPECT_STREQ(src.subdir, "translate");
}

TEST(ExtSourceParseTest, ParsesGithubTreeUrl)
{
	struct ext_source src;
	ASSERT_EQ(ext_source_parse(
		"https://github.com/oxUnd/morph/tree/main/exts/locate",
		&src), 0);
	EXPECT_STREQ(src.owner, "oxUnd");
	EXPECT_STREQ(src.repo, "morph");
	EXPECT_STREQ(src.ref, "main");
	EXPECT_STREQ(src.subdir, "exts/locate");
}

TEST(ExtSourceParseTest, RejectsUnsafeSubdir)
{
	struct ext_source src;
	EXPECT_EQ(ext_source_parse("github:morph-ai/exts//../bad", &src),
		  -EINVAL);
	EXPECT_EQ(ext_source_parse("github:morph-ai/exts///abs", &src),
		  -EINVAL);
}

TEST(ExtManifestTest, ParsesFrontsCategoriesAndBuildCommand)
{
	const char *data =
		"name = \"demo\"\n"
		"version = \"0.1.0\"\n"
		"description = \"Demo ext\"\n"
		"entry = \"bin/demo\"\n"
		"fronts = [\"cli\", \"fastcgi\"]\n"
		"categories = [\"text\", \"workflow\"]\n"
		"[build]\n"
		"command = \"make build\"\n";
	struct ext_manifest m;
	ASSERT_EQ(manifest_parse(data, &m), 0);
	EXPECT_STREQ(m.type, "exec");
	EXPECT_STREQ(m.entry, "bin/demo");
	ASSERT_EQ(m.fronts_count, 2);
	EXPECT_STREQ(m.fronts[0], "cli");
	EXPECT_STREQ(m.fronts[1], "fastcgi");
	ASSERT_EQ(m.categories_count, 2);
	EXPECT_STREQ(m.categories[0], "text");
	EXPECT_STREQ(m.categories[1], "workflow");
	EXPECT_STREQ(m.build_command, "make build");
	EXPECT_TRUE(ext_manifest_supports_front(&m, "cli"));
	EXPECT_FALSE(ext_manifest_supports_front(&m, "ios"));
	ext_manifest_cleanup(&m);
}

TEST(ExtManifestTest, MissingFrontsMeansAllFronts)
{
	const char *data =
		"name = \"legacy\"\n"
		"entry = \"legacy.sh\"\n";
	struct ext_manifest m;
	ASSERT_EQ(manifest_parse(data, &m), 0);
	EXPECT_TRUE(ext_manifest_supports_front(&m, "cli"));
	EXPECT_TRUE(ext_manifest_supports_front(&m, "fastcgi"));
	ext_manifest_cleanup(&m);
}

TEST(ExtManifestTest, ParsesNamedSandboxCapabilities)
{
	const char *data =
		"name = \"capable\"\n"
		"entry = \"capable.sh\"\n"
		"sandbox_capabilities = [\"exec\", \"pty\", "
		"\"process_info\", \"ipc\", \"temporary_directory\"]\n";
	struct ext_manifest m;

	ASSERT_EQ(manifest_parse(data, &m), 0);
	EXPECT_NE(m.permissions & EXT_PERM_EXEC, 0U);
	EXPECT_NE(m.permissions & EXT_PERM_PTY, 0U);
	EXPECT_NE(m.permissions & EXT_PERM_PROCESS_INFO, 0U);
	EXPECT_NE(m.permissions & EXT_PERM_IPC, 0U);
	EXPECT_NE(m.permissions & EXT_PERM_TEMP, 0U);
	ext_manifest_cleanup(&m);
}

TEST(ExtManifestTest, RejectsUnknownSandboxCapability)
{
	const char *data =
		"name = \"bad-capability\"\n"
		"entry = \"bad.sh\"\n"
		"sandbox_capabilities = [\"unrestricted_device\"]\n";
	struct ext_manifest m;

	EXPECT_EQ(manifest_parse(data, &m), -EINVAL);
}

TEST(ExtManifestTest, NamedFilesystemRequiresAllowedPaths)
{
	const char *data =
		"name = \"bad-filesystem\"\n"
		"entry = \"bad.sh\"\n"
		"sandbox_capabilities = [\"filesystem\"]\n";
	struct ext_manifest m;

	EXPECT_EQ(manifest_parse(data, &m), -EINVAL);
}

TEST(ExtManifestTest, ExampleExtManifestsFollowInstallSchema)
{
	const std::vector<std::string> dirs = {
		"exts/guardrail-agent-ui-tags",
		"exts/demo-translate",
		"exts/demo-upper",
		"exts/demo-guardrail-pii",
		"exts/locate",
		"exts/rg",
	};

	for (const auto &dir : dirs) {
		struct ext_manifest m;
		std::string full_dir = std::string(MORPH_TEST_SOURCE_DIR) +
			"/" + dir;
		std::string manifest = full_dir + "/manifest.toml";
		ASSERT_EQ(manifest_parse_file(manifest.c_str(), &m), 0)
			<< manifest;
		EXPECT_STRNE(m.name, "") << manifest;
		EXPECT_STRNE(m.entry, "") << manifest;
		EXPECT_GT(m.fronts_count, 0) << manifest;
		EXPECT_GT(m.categories_count, 0) << manifest;

		if (m.build_command[0]) {
			EXPECT_STRNE(m.build_command, "") << manifest;
		} else {
			EXPECT_TRUE(file_exists((full_dir + "/" + m.entry).c_str()))
				<< manifest;
		}
		ext_manifest_cleanup(&m);
	}
}

TEST(ExtInstallTest, InstallsTaggedMonorepoPackageWithBuild)
{
	if (test_cmd("git --version >/dev/null 2>&1") != 0)
		GTEST_SKIP() << "git not available";

	char tmpl[] = "/tmp/morph_ext_install_XXXXXX";
	char *root = mkdtemp(tmpl);
	ASSERT_NE(root, nullptr);
	std::string root_dir(root);
	std::string src_dir = root_dir + "/src";
	std::string base_dir = root_dir + "/base";
	std::string owner_dir = base_dir + "/owner";
	std::string bare_dir = owner_dir + "/repo.git";
	std::string pkg_dir = src_dir + "/packages/demo";
	std::string install_dir = root_dir + "/installed";
	std::string base_url = "file://" + base_dir;

	mkdir_p(pkg_dir);
	mkdir_p(owner_dir);
	write_text(pkg_dir + "/morph-ext.toml",
		   "name = \"demo-install\"\n"
		   "version = \"0.1.0\"\n"
		   "entry = \"bin/demo\"\n"
		   "fronts = [\"cli\"]\n"
		   "[build]\n"
		   "command = \"mkdir -p bin && cp run.sh bin/demo && chmod +x bin/demo\"\n");
	write_text(pkg_dir + "/run.sh", "#!/bin/sh\necho demo\n");

	ASSERT_EQ(test_cmd("git init " + src_dir + " >/dev/null"), 0);
	ASSERT_EQ(test_cmd("git -C " + src_dir +
			   " config user.email test@example.com"), 0);
	ASSERT_EQ(test_cmd("git -C " + src_dir +
			   " config user.name test"), 0);
	ASSERT_EQ(test_cmd("git -C " + src_dir + " add ."), 0);
	ASSERT_EQ(test_cmd("git -C " + src_dir +
			   " commit -m init >/dev/null"), 0);
	ASSERT_EQ(test_cmd("git -C " + src_dir + " tag v0.1.0"), 0);
	ASSERT_EQ(test_cmd("git clone --bare " + src_dir + " " + bare_dir +
			   " >/dev/null 2>&1"), 0);

	struct ext_install_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.install_dir = install_dir.c_str();
	opts.github_base_url = base_url.c_str();
	opts.yes = 1;

	struct ext_install_result res;
	ASSERT_EQ(ext_install_source(
		"github:owner/repo@v0.1.0//packages/demo", &opts, &res), 0);
	EXPECT_STREQ(res.name, "demo-install");
	EXPECT_EQ(strlen(res.resolved_ref), 40u);
	EXPECT_TRUE(file_exists((install_dir + "/demo-install/bin/demo").c_str()));
	EXPECT_FALSE(file_exists((install_dir + "/demo-install/.git").c_str()));

	ASSERT_EQ(test_cmd("rm -rf " + root_dir), 0);
}
