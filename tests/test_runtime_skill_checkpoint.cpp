#include "runtime_test_support.hpp"

extern "C" {
#include "runtime/checkpoint.h"
#include "runtime/runtime_internal.h"
#include "persistence/checkpoint_store.h"
#include "skill/skill.h"
#include "nb/cleanup.h"
#include "cJSON.h"
}

class RuntimeSkillCheckpointTest : public RuntimeFacadeTest
{
      protected:
	void OpenSkills()
	{
		for (const char *name : {"before", "after"}) {
			const std::string path = directory + "/.agents/skills/" + name;
			std::filesystem::create_directories(path);
			std::ofstream(path + "/SKILL.md")
				<< "---\nname: " << name
				<< "\ndescription: Check session skill restoration\n---\n"
				<< "Instructions for " << name << ".\n";
		}
		WriteConfig("[memory]\nenabled=false\n[skill]\ndir=\"" + directory +
			    "/.agents/skills\"\n");
		struct runtime_options options{};
		options.config_path = config_path.c_str();
		options.default_session = "skill-checkpoint";
		options.allocate_skill_registry = 1;
		Open(&options);
	}

	void ExpectActive(const char *name, bool expected)
	{
		struct skill_entry entry{};
		ASSERT_EQ(runtime_skill_find_borrow(instance, name, &entry), 0);
		EXPECT_EQ(entry.activated != 0, expected) << name;
	}
};

TEST_F(RuntimeSkillCheckpointTest, RewindReloadsSkillsAndSelectionAndColdRestoreStayConsistent)
{
	OpenSkills();
	ASSERT_EQ(runtime_skill_set_active(instance, "before", 1, nullptr), 0);
	const int64_t session_id = instance->context.current_session.id;
	int64_t checkpoint_id = 0;
	ASSERT_EQ(checkpoint_store_create(&instance->context.database, session_id,
					  directory.c_str(), "skill-turn",
					  "before activation changes", &checkpoint_id),
		  0);
	ASSERT_EQ(runtime_skill_set_active(instance, "before", 0, nullptr), 0);
	ASSERT_EQ(runtime_skill_set_active(instance, "after", 1, nullptr), 0);
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_checkpoint_restore(instance, checkpoint_id, RUNTIME_REWIND_CODE, &report),
		  0);
	ExpectActive("before", false);
	ExpectActive("after", true);
	cJSON_DeleteOwner(&report);
	ASSERT_EQ(instance->context.active_skill_session_id, session_id);
	ASSERT_EQ(runtime_checkpoint_restore(instance, checkpoint_id, RUNTIME_REWIND_CONVERSATION,
					     &report),
		  0);
	ExpectActive("before", true);
	ExpectActive("after", false);
	ASSERT_EQ(runtime_session_create_and_select(instance, "empty-skills", nullptr), 0);
	ExpectActive("before", false);
	ExpectActive("after", false);
	ASSERT_EQ(runtime_session_select_existing(instance, session_id, nullptr), 0);
	ExpectActive("before", true);
	ExpectActive("after", false);
	runtime_close(instance);
	instance = nullptr;
	OpenSkills();
	ASSERT_EQ(instance->context.current_session.id, session_id);
	ExpectActive("before", true);
	ExpectActive("after", false);
}
