#include <gtest/gtest.h>
#include "skill/skill.h"
#include "skill/skill_parse.h"
#include "agent/tools/skill_activate.h"
#include "agent/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

class SkillParseTest : public ::testing::Test {
protected:
	struct skill_frontmatter fm;
	char *body;
	void SetUp() override {
		memset(&fm, 0, sizeof(fm));
		body = nullptr;
	}
	void TearDown() override {
		free(body);
	}
};

TEST_F(SkillParseTest, MinimalFrontmatter) {
	const char *data =
		"---\n"
		"name: test-skill\n"
		"description: A test skill for unit testing.\n"
		"---\n"
		"# Test\n"
		"Body content here.\n";
	int rc = skill_parse(data, strlen(data), &fm, &body);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(fm.name, "test-skill");
	EXPECT_STREQ(fm.description, "A test skill for unit testing.");
	ASSERT_NE(body, nullptr);
	EXPECT_NE(strstr(body, "# Test"), nullptr);
}

TEST_F(SkillParseTest, AllFields) {
	const char *data =
		"---\n"
		"name: full-skill\n"
		"description: A skill with all fields.\n"
		"license: MIT\n"
		"compatibility: Requires git\n"
		"allowed-tools: Bash(git:*) Read\n"
		"metadata:\n"
		"  author: test-org\n"
		"  version: \"1.0\"\n"
		"---\n"
		"Instructions here.\n";
	int rc = skill_parse(data, strlen(data), &fm, &body);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(fm.name, "full-skill");
	EXPECT_STREQ(fm.license, "MIT");
	EXPECT_STREQ(fm.compatibility, "Requires git");
	EXPECT_STREQ(fm.allowed_tools, "Bash(git:*) Read");
	EXPECT_EQ(fm.metadata_count, 2);
	EXPECT_STREQ(fm.metadata[0].key, "author");
	EXPECT_STREQ(fm.metadata[0].value, "test-org");
	EXPECT_STREQ(fm.metadata[1].key, "version");
	EXPECT_STREQ(fm.metadata[1].value, "1.0");
}

TEST_F(SkillParseTest, NoClosingDelimiter) {
	const char *data = "---\nname: broken\n";
	int rc = skill_parse(data, strlen(data), &fm, &body);
	EXPECT_NE(rc, 0);
}

TEST_F(SkillParseTest, EmptyBody) {
	const char *data =
		"---\n"
		"name: empty-body\n"
		"description: No body.\n"
		"---\n";
	int rc = skill_parse(data, strlen(data), &fm, &body);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(fm.name, "empty-body");
}

TEST_F(SkillParseTest, QuotedValues) {
	const char *data =
		"---\n"
		"name: quoted\n"
		"description: \"A description with: colons\"\n"
		"---\n";
	int rc = skill_parse(data, strlen(data), &fm, &body);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(fm.name, "quoted");
	EXPECT_STREQ(fm.description, "A description with: colons");
}

TEST_F(SkillParseTest, BlockScalarChinese) {
	const char *data =
		"---\n"
		"name: 中文技能\n"
		"description: |\n"
		"  这是一个多行的中文描述。\n"
		"  第二行内容。\n"
		"---\n"
		"# 正文\n";
	int rc = skill_parse(data, strlen(data), &fm, &body);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(fm.name, "中文技能");
	EXPECT_NE(strstr(fm.description, "这是一个多行的中文描述。"), nullptr);
	EXPECT_NE(strstr(fm.description, "第二行内容。"), nullptr);
}

TEST_F(SkillParseTest, EmptyValueFollowedByKey) {
	const char *data =
		"---\n"
		"name: x\n"
		"description:\n"
		"license: MIT\n"
		"---\n";
	int rc = skill_parse(data, strlen(data), &fm, &body);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(fm.name, "x");
	EXPECT_STREQ(fm.license, "MIT");
}

TEST_F(SkillParseTest, NullInput) {
	int rc = skill_parse(nullptr, 0, &fm, &body);
	EXPECT_NE(rc, 0);
	rc = skill_parse("---\nname:x\n---\n", 17, nullptr, &body);
	EXPECT_NE(rc, 0);
}

class SkillRegistryTest : public ::testing::Test {
protected:
	struct skill_registry reg;
	char tmpdir[256];
	void SetUp() override {
		skill_registry_init(&reg);
		snprintf(tmpdir, sizeof(tmpdir), "/tmp/morph_skill_test_%d", getpid());
		mkdir(tmpdir, 0755);
	}
	void TearDown() override {
		skill_registry_cleanup(&reg);
		rmdir(tmpdir);
	}
	void create_skill(const char *dir_name, const char *name,
			  const char *description, const char *body_text) {
		char skill_dir[512];
		snprintf(skill_dir, sizeof(skill_dir), "%s/%s", tmpdir, dir_name);
		mkdir(skill_dir, 0755);
		char md_path[512];
		snprintf(md_path, sizeof(md_path), "%s/SKILL.md", skill_dir);
		FILE *f = fopen(md_path, "w");
		ASSERT_NE(f, nullptr);
		fprintf(f, "---\nname: %s\ndescription: %s\n---\n%s\n",
			name, description, body_text ? body_text : "");
		fclose(f);
	}
	void remove_skill(const char *dir_name) {
		char skill_dir[512];
		snprintf(skill_dir, sizeof(skill_dir), "%s/%s", tmpdir, dir_name);
		char md_path[512];
		snprintf(md_path, sizeof(md_path), "%s/SKILL.md", skill_dir);
		unlink(md_path);
		rmdir(skill_dir);
	}
};

TEST_F(SkillRegistryTest, Init) {
	EXPECT_EQ(reg.count, 0);
}

TEST_F(SkillRegistryTest, DiscoverOne) {
	create_skill("my-skill", "my-skill", "A test skill.", "# Hello");
	int rc = skill_discover(&reg, tmpdir);
	EXPECT_EQ(rc, 1);
	EXPECT_EQ(reg.count, 1);
	EXPECT_STREQ(reg.entries[0].fm.name, "my-skill");
	EXPECT_STREQ(reg.entries[0].fm.description, "A test skill.");
	remove_skill("my-skill");
}

TEST_F(SkillRegistryTest, DiscoverMultiple) {
	create_skill("skill-a", "skill-a", "First skill.", "A");
	create_skill("skill-b", "skill-b", "Second skill.", "B");
	int rc = skill_discover(&reg, tmpdir);
	EXPECT_EQ(rc, 2);
	EXPECT_EQ(reg.count, 2);
	remove_skill("skill-a");
	remove_skill("skill-b");
}

TEST_F(SkillRegistryTest, DiscoverNoSkillMd) {
	char nodir[512];
	snprintf(nodir, sizeof(nodir), "%s/no-skill", tmpdir);
	mkdir(nodir, 0755);
	int rc = skill_discover(&reg, tmpdir);
	EXPECT_EQ(rc, 0);
	rmdir(nodir);
}

TEST_F(SkillRegistryTest, DiscoverNonexistentDir) {
	int rc = skill_discover(&reg, "/tmp/nonexistent_dir_12345");
	EXPECT_EQ(rc, 0);
}

TEST_F(SkillRegistryTest, Lookup) {
	create_skill("find-me", "find-me", "Findable.", "X");
	skill_discover(&reg, tmpdir);
	struct skill_entry *e = skill_lookup(&reg, "find-me");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->fm.name, "find-me");
	EXPECT_EQ(skill_lookup(&reg, "not-here"), nullptr);
	remove_skill("find-me");
}

TEST_F(SkillRegistryTest, ActivateAndDeactivate) {
	create_skill("act-me", "act-me", "Activatable.", "# Instructions\nDo stuff.");
	skill_discover(&reg, tmpdir);
	struct skill_entry *e = skill_lookup(&reg, "act-me");
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(e->activated, 0);
	EXPECT_EQ(e->body_loaded, 0);

	int rc = skill_activate(e);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(e->activated, 1);
	EXPECT_EQ(e->body_loaded, 1);
	ASSERT_NE(e->body, nullptr);
	EXPECT_NE(strstr(e->body, "# Instructions"), nullptr);

	skill_deactivate(e);
	EXPECT_EQ(e->activated, 0);
	EXPECT_EQ(e->body_loaded, 0);
	EXPECT_EQ(e->body, nullptr);
	remove_skill("act-me");
}

TEST_F(SkillRegistryTest, ActivateIdempotent) {
	create_skill("idem", "idem", "Idempotent.", "Body");
	skill_discover(&reg, tmpdir);
	struct skill_entry *e = skill_lookup(&reg, "idem");
	ASSERT_NE(e, nullptr);
	skill_activate(e);
	int rc = skill_activate(e);
	EXPECT_EQ(rc, 0);
	remove_skill("idem");
}

TEST_F(SkillRegistryTest, BuildActivatedInstructions) {
	create_skill("s1", "s1", "Skill one.", "# One\nDo one.");
	create_skill("s2", "s2", "Skill two.", "# Two\nDo two.");
	skill_discover(&reg, tmpdir);
	skill_activate(skill_lookup(&reg, "s1"));
	skill_activate(skill_lookup(&reg, "s2"));
	char *instr = skill_build_activated_instructions(&reg);
	ASSERT_NE(instr, nullptr);
	EXPECT_NE(strstr(instr, "<skill name=\"s1\">"), nullptr);
	EXPECT_NE(strstr(instr, "# One"), nullptr);
	EXPECT_NE(strstr(instr, "<skill name=\"s2\">"), nullptr);
	EXPECT_NE(strstr(instr, "# Two"), nullptr);
	free(instr);
	remove_skill("s1");
	remove_skill("s2");
}

TEST_F(SkillRegistryTest, BuildCatalog) {
	create_skill("cat-a", "cat-a", "Catalog A.", "X");
	create_skill("cat-b", "cat-b", "Catalog B.", "Y");
	skill_discover(&reg, tmpdir);
	char buf[4096];
	int n = skill_build_catalog(&reg, buf, sizeof(buf));
	EXPECT_EQ(n, 2);
	EXPECT_NE(strstr(buf, "cat-a: Catalog A."), nullptr);
	EXPECT_NE(strstr(buf, "cat-b: Catalog B."), nullptr);
	remove_skill("cat-a");
	remove_skill("cat-b");
}

TEST_F(SkillRegistryTest, DuplicateNameSkipped) {
	create_skill("dup-name", "dup-name", "First.", "A");
	skill_discover(&reg, tmpdir);
	EXPECT_EQ(reg.count, 1);
	char skill_dir2[512];
	snprintf(skill_dir2, sizeof(skill_dir2), "%s/dup-name-2", tmpdir);
	mkdir(skill_dir2, 0755);
	char md_path[512];
	snprintf(md_path, sizeof(md_path), "%s/SKILL.md", skill_dir2);
	FILE *f = fopen(md_path, "w");
	ASSERT_NE(f, nullptr);
	fprintf(f, "---\nname: dup-name\ndescription: Second.\n---\nB\n");
	fclose(f);
	skill_discover(&reg, tmpdir);
	EXPECT_EQ(reg.count, 1);
	unlink(md_path);
	rmdir(skill_dir2);
	remove_skill("dup-name");
}

TEST_F(SkillRegistryTest, NullParams) {
	skill_registry_init(nullptr);
	skill_registry_cleanup(nullptr);
	EXPECT_EQ(skill_discover(nullptr, "/tmp"), -EINVAL);
	EXPECT_EQ(skill_discover(&reg, nullptr), -EINVAL);
	EXPECT_EQ(skill_lookup(nullptr, "x"), nullptr);
	EXPECT_EQ(skill_lookup(&reg, nullptr), nullptr);
	EXPECT_EQ(skill_activate(nullptr), -EINVAL);
	skill_deactivate(nullptr);
	skill_deactivate_all(nullptr);
	EXPECT_EQ(skill_build_activated_instructions(nullptr), nullptr);
	EXPECT_EQ(skill_build_catalog(nullptr, nullptr, 0), -EINVAL);
}

class SkillActivateToolTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	struct skill_registry skills;
	void SetUp() override {
		tool_registry_init(&tools);
		skill_registry_init(&skills);
	}
	void TearDown() override {
		skill_registry_cleanup(&skills);
	}
};

TEST_F(SkillActivateToolTest, RegisterTool) {
	int rc = skill_activate_init(&tools, &skills);
	EXPECT_EQ(rc, 0);
	struct tool_entry *e = tool_lookup(&tools, "activate_skill");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "activate_skill");
}

TEST_F(SkillActivateToolTest, NullParams) {
	EXPECT_NE(skill_activate_init(nullptr, &skills), 0);
	EXPECT_NE(skill_activate_init(&tools, nullptr), 0);
}

TEST_F(SkillActivateToolTest, ActivateViaTool) {
	char tmpdir[256];
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/morph_skill_tool_test_%d", getpid());
	mkdir(tmpdir, 0755);
	char skill_dir[512];
	snprintf(skill_dir, sizeof(skill_dir), "%s/review", tmpdir);
	mkdir(skill_dir, 0755);
	char md_path[512];
	snprintf(md_path, sizeof(md_path), "%s/SKILL.md", skill_dir);
	FILE *f = fopen(md_path, "w");
	ASSERT_NE(f, nullptr);
	fprintf(f, "---\nname: review\ndescription: Review code.\n---\n# Review\nCheck bugs.\n");
	fclose(f);

	skill_discover(&skills, tmpdir);
	skill_activate_init(&tools, &skills);

	char *result = nullptr;
	int rc = tool_exec(&tools, "activate_skill",
			   "{\"name\":\"review\"}", &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result, nullptr);
	EXPECT_NE(strstr(result, "<skill name=\"review\">"), nullptr);
	EXPECT_NE(strstr(result, "Check bugs."), nullptr);
	free(result);

	unlink(md_path);
	rmdir(skill_dir);
	rmdir(tmpdir);
}

TEST_F(SkillActivateToolTest, ActivateNotFound) {
	skill_activate_init(&tools, &skills);
	char *result = nullptr;
	int rc = tool_exec(&tools, "activate_skill",
			   "{\"name\":\"nonexistent\"}", &result);
	EXPECT_NE(rc, 0);
	free(result);
}

TEST_F(SkillActivateToolTest, ActivateMissingName) {
	skill_activate_init(&tools, &skills);
	char *result = nullptr;
	int rc = tool_exec(&tools, "activate_skill", "{}", &result);
	EXPECT_NE(rc, 0);
	free(result);
}
