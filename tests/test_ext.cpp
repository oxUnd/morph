#include <gtest/gtest.h>
extern "C" {
#include "ext/ext.h"
#include "ext/install.h"
#include "ext/manifest.h"
#include "ipc/jsonrpc.h"
#include "sandbox/loader.h"
#include "util/file.h"
#include "util/cleanup.h"
#include "util/error.h"
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
	struct ext_manifest m MORPH_CLEANUP(ext_manifest_cleanup) = {};
	ASSERT_EQ(manifest_parse(NULL, data, &m), 0);
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
	struct ext_manifest m MORPH_CLEANUP(ext_manifest_cleanup) = {};
	ASSERT_EQ(manifest_parse(NULL, data, &m), 0);
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
	struct ext_manifest m MORPH_CLEANUP(ext_manifest_cleanup) = {};

	ASSERT_EQ(manifest_parse(NULL, data, &m), 0);
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
	struct ext_manifest m MORPH_CLEANUP(ext_manifest_cleanup) = {};

	EXPECT_EQ(manifest_parse(NULL, data, &m), -EINVAL);
}

TEST(ExtManifestTest, NamedFilesystemRequiresAllowedPaths)
{
	const char *data =
		"name = \"bad-filesystem\"\n"
		"entry = \"bad.sh\"\n"
		"sandbox_capabilities = [\"filesystem\"]\n";
	struct ext_manifest m MORPH_CLEANUP(ext_manifest_cleanup) = {};

	EXPECT_EQ(manifest_parse(NULL, data, &m), -EINVAL);
}

TEST(ExtManifestTest, ExampleExtManifestsFollowInstallSchema)
{
	const std::vector<std::string> dirs = {
		"exts/guardrail-agent-ui-tags",
		"exts/demo-translate",
		"exts/demo-upper",
		"exts/demo-guardrail-pii",
		"exts/locate",
	};

	for (const auto &dir : dirs) {
		struct ext_manifest m MORPH_CLEANUP(ext_manifest_cleanup) = {};
		std::string full_dir = std::string(MORPH_TEST_SOURCE_DIR) +
			"/" + dir;
		std::string manifest = full_dir + "/manifest.toml";
		ASSERT_EQ(manifest_parse_file(NULL, manifest.c_str(), &m), 0) << manifest;
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

TEST(ExtRuntimeTest, ExecExtensionSandboxCanReadItsOwnEntry)
{
	struct ext extension MORPH_CLEANUP(ext_unload) = {};
	struct sandbox_config config = {};
	std::string directory = std::string(MORPH_TEST_SOURCE_DIR) +
		"/tests/fixtures/exec-sandbox";
	char cwd[PATH_MAX];

	ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
	ASSERT_EQ(ext_load(NULL, &extension, directory.c_str()), 0);
	ASSERT_EQ(ext_sandbox_config_prepare(&extension, &config), 0);
	ASSERT_EQ(config.read_paths_count, 2);
	EXPECT_TRUE(path_is_within(extension.exec_path, config.read_paths[0]));
	EXPECT_TRUE(path_is_within(cwd, config.read_paths[1]));
	EXPECT_EQ(config.write_paths_count, 0);
	EXPECT_EQ(config.delete_paths_count, 0);
	EXPECT_TRUE(config.process_exec);
	ext_sandbox_config_cleanup(&config);
	ext_unload(&extension);
}

TEST(ExtManifestTest, OccupiedOwnerIsRejectedWithoutLosingItsStorage)
{
	struct ext_manifest manifest MORPH_CLEANUP(ext_manifest_cleanup) = {};
	ASSERT_EQ(manifest_parse(NULL, "name = \"original\"\nfronts = [\"cli\"]\n", &manifest), 0);
	char *schema = manifest.input_schema;
	char **fronts = manifest.fronts;
	EXPECT_EQ(manifest_parse(NULL, "name = \"replacement\"\n", &manifest), -EEXIST);
	EXPECT_EQ(manifest_parse_file(NULL, "/missing/manifest.toml", &manifest), -EEXIST);
	EXPECT_EQ(manifest.input_schema, schema);
	EXPECT_EQ(manifest.fronts, fronts);
	EXPECT_STREQ(manifest.name, "original");
	ASSERT_EQ(manifest.fronts_count, 1);
	EXPECT_STREQ(manifest.fronts[0], "cli");
}

TEST(ExtManifestTest, FailurePreservesAnEmptyOwnersMetadata)
{
	struct ext_manifest manifest MORPH_CLEANUP(ext_manifest_cleanup) = {};
	strcpy(manifest.name, "untouched");
	manifest.max_cpu_seconds = 19;
	const char *invalid = "name = \"partial\"\n"
			      "sandbox_capabilities = [\"filesystem\"]\n";
	EXPECT_EQ(manifest_parse(NULL, invalid, &manifest), -EINVAL);
	EXPECT_STREQ(manifest.name, "untouched");
	EXPECT_EQ(manifest.max_cpu_seconds, 19);
	EXPECT_EQ(manifest.input_schema, nullptr);
	EXPECT_EQ(manifest.fronts, nullptr);
	EXPECT_EQ(manifest_parse(NULL, "invalid = [", &manifest), MORPH_ERR_PARSE);
	EXPECT_STREQ(manifest.name, "untouched");
	EXPECT_EQ(manifest_parse_file(NULL, "/missing/manifest.toml", &manifest), -ENOENT);
	EXPECT_STREQ(manifest.name, "untouched");
}

TEST(ExtRuntimeTest, LoadPreservesExistingOwnerAndRebindsDescriptorViews)
{
	struct ext extension MORPH_CLEANUP(ext_unload) = {};
	std::string directory = std::string(MORPH_TEST_SOURCE_DIR) + "/tests/fixtures/exec-sandbox";
	ASSERT_EQ(ext_load(NULL, &extension, directory.c_str()), 0);
	EXPECT_EQ(extension.tool_desc.name, extension.manifest.name);
	EXPECT_EQ(extension.tool_desc.description, extension.manifest.description);
	EXPECT_EQ(extension.tool_desc.input_schema, extension.manifest.input_schema);
	char *schema = extension.manifest.input_schema;
	EXPECT_EQ(ext_load(NULL, &extension, "/missing/extension"), -EEXIST);
	EXPECT_EQ(extension.manifest.input_schema, schema);
	EXPECT_EQ(extension.tool_desc.name, extension.manifest.name);
	EXPECT_TRUE(extension.enabled);
}

TEST(ExtRuntimeTest, FailedLoadDoesNotPublishAPartialOwner)
{
	struct ext extension MORPH_CLEANUP(ext_unload) = {};
	strcpy(extension.path, "untouched");
	char temporary[] = "/tmp/morph_ext_owner_XXXXXX";
	char *directory = mkdtemp(temporary);
	ASSERT_NE(directory, nullptr);
	std::string manifest_path = std::string(directory) + "/manifest.toml";
	write_text(manifest_path, "name = \"partial\"\nentry = \"missing\"\n"
				  "fronts = [\"cli\"]\n");
	int rc = ext_load(NULL, &extension, directory);
	EXPECT_EQ(unlink(manifest_path.c_str()), 0);
	EXPECT_EQ(rmdir(directory), 0);
	EXPECT_EQ(rc, -ENOENT);
	EXPECT_STREQ(extension.path, "untouched");
	EXPECT_EQ(extension.manifest.input_schema, nullptr);
	EXPECT_EQ(extension.tool_desc.name, nullptr);
	EXPECT_FALSE(extension.enabled);
}

static int ext_test_native_run(const char *args, char **result)
{
	*result = strdup(args);
	return *result ? 0 : -ENOMEM;
}

TEST(ExtRuntimeTest, ExecutionRejectsAnOccupiedResultOwner)
{
	struct ext extension = {};
	extension.enabled = 1;
	extension.run = ext_test_native_run;
	char *result MORPH_CLEANUP(morph_charp_cleanup) = strdup("original");
	ASSERT_NE(result, nullptr);
	char *original = result;
	EXPECT_EQ(ext_run(&extension, "{}", &result), -EEXIST);
	EXPECT_EQ(result, original);
	EXPECT_STREQ(result, "original");
	morph_charp_cleanup(&result);
	ASSERT_EQ(ext_run(&extension, "{\"ok\":true}", &result), 0);
	EXPECT_STREQ(result, "{\"ok\":true}");
}

TEST(ExtJsonRpcTest, ParsedResponseOwnsStorageAfterInputIsReleased)
{
	struct jsonrpc_response response MORPH_CLEANUP(jsonrpc_response_free) = {};
	char *input MORPH_CLEANUP(morph_charp_cleanup) =
		strdup("{\"id\":7,\"result\":{\"message\":\"independent\"}}");
	ASSERT_NE(input, nullptr);
	ASSERT_EQ(jsonrpc_response_copy(input, &response), 0);
	morph_charp_cleanup(&input);
	EXPECT_EQ(response.id, 7);
	EXPECT_STREQ(response.result_json, "{\"message\":\"independent\"}");
	jsonrpc_response_free(&response);
	input = strdup("{\"error\":{\"code\":-5,\"message\":\"independent error\"}}");
	ASSERT_NE(input, nullptr);
	ASSERT_EQ(jsonrpc_response_copy(input, &response), 0);
	morph_charp_cleanup(&input);
	EXPECT_TRUE(response.has_error);
	EXPECT_EQ(response.error_code, -5);
	EXPECT_STREQ(response.error_message, "independent error");
}

TEST(ExtJsonRpcTest, RejectedAndInvalidResponsesPreserveOutput)
{
	struct jsonrpc_response response MORPH_CLEANUP(jsonrpc_response_free) = {};
	ASSERT_EQ(jsonrpc_response_copy("{\"result\":\"original\"}", &response), 0);
	char *original = response.result_json;
	EXPECT_EQ(jsonrpc_response_copy("{\"result\":\"new\"}", &response), -EEXIST);
	EXPECT_EQ(response.result_json, original);
	EXPECT_STREQ(response.result_json, "\"original\"");
	jsonrpc_response_free(&response);
	response.id = 37;
	EXPECT_EQ(jsonrpc_response_copy("invalid json", &response), -EPROTO);
	EXPECT_EQ(response.id, 37);
	EXPECT_EQ(response.result_json, nullptr);
	EXPECT_EQ(response.error_message, nullptr);
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
	ASSERT_EQ(ext_install_source(NULL, "github:owner/repo@v0.1.0//packages/demo", &opts, &res),
		  0);
	EXPECT_STREQ(res.name, "demo-install");
	EXPECT_EQ(strlen(res.resolved_ref), 40u);
	EXPECT_TRUE(file_exists((install_dir + "/demo-install/bin/demo").c_str()));
	EXPECT_FALSE(file_exists((install_dir + "/demo-install/.git").c_str()));

	ASSERT_EQ(test_cmd("rm -rf " + root_dir), 0);
}

TEST(ExtRuntimeTest, SharedLibraryBackendRejectsAnExistingEntrypoint)
{
	struct ext extension = {};
	extension.run = ext_test_native_run;
	EXPECT_EQ(ext_load_so(&extension, "/missing/library"), -EEXIST);
	EXPECT_EQ(extension.run, ext_test_native_run);
	EXPECT_EQ(extension.dl_handle, nullptr);
}


TEST(ExtManifestTest, OversizedFixedStringsAreRejectedWithoutMutatingTheOutput)
{
	const std::pair<const char *, size_t> fields[] = {
		{"name", 64}, {"version", 32}, {"description", 256}, {"author", 64},
		{"type", 16}, {"entry", 128}, {"hook", 32}, {"action_text", 512},
		{"build.command", 512},
	};
	for (const auto &field : fields) {
		SCOPED_TRACE(field.first);
		std::string input = std::string(field.first) + " = \"" +
			std::string(field.second, 'x') + "\"\n";
		struct ext_manifest manifest MORPH_CLEANUP(ext_manifest_cleanup) = {};
		strcpy(manifest.name, "unchanged");
		EXPECT_EQ(manifest_parse(nullptr, input.c_str(), &manifest), -E2BIG);
		EXPECT_STREQ(manifest.name, "unchanged");
		EXPECT_EQ(manifest.input_schema, nullptr);
	}
}

TEST(ExtManifestTest, Utf8BoundaryIsPreservedAndMalformedOrNulStringsAreRejected)
{
	std::string name;
	for (int i = 0; i < 21; i++)
		name += "中";
	struct ext_manifest manifest MORPH_CLEANUP(ext_manifest_cleanup) = {};
	std::string input = "name = \"" + name + "\"\n";
	ASSERT_EQ(manifest_parse(nullptr, input.c_str(), &manifest), 0);
	EXPECT_STREQ(manifest.name, name.c_str());
	ext_manifest_cleanup(&manifest);
	input = "name = \"" + name + "中\"\n";
	EXPECT_EQ(manifest_parse(nullptr, input.c_str(), &manifest), -E2BIG);
	input = "description = \"" + std::string(1, static_cast<char>(0xff)) + "\"\n";
	EXPECT_NE(manifest_parse(nullptr, input.c_str(), &manifest), 0);
	EXPECT_NE(manifest_parse(nullptr, "entry = \"safe\\u0000hidden\"\n", &manifest), 0);
	EXPECT_EQ(manifest.input_schema, nullptr);
}

TEST(ExtManifestTest, ResourceIntegersCannotWrapIntoDifferentLimits)
{
	for (const char *field : {"max_memory_mb", "max_cpu_seconds", "max_open_files",
				  "permissions"}) {
		SCOPED_TRACE(field);
		struct ext_manifest manifest MORPH_CLEANUP(ext_manifest_cleanup) = {};
		std::string input = std::string(field) + " = 4294967296\n";
		EXPECT_EQ(manifest_parse(nullptr, input.c_str(), &manifest), -ERANGE);
		input = std::string(field) + " = -1\n";
		EXPECT_EQ(manifest_parse(nullptr, input.c_str(), &manifest), -ERANGE);
	}
}
