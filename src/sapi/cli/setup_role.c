#include "setup_role.h"
#include "setup_ui.h"
#include "prompt/template.h"
#include "util/data.h"
#include "util/error.h"
#include <stdio.h>
#include <string.h>

struct cli_setup_role cli_setup_role_get(int index)
{
	const unsigned text = 1u << SETUP_TEXT;
	const unsigned vision = text | (1u << SETUP_VISION);
	const unsigned image = vision | (1u << SETUP_IMAGE);
	const struct cli_setup_role roles[] = {
		{"general", "General assistant", "Everyday questions and mixed tasks. Requires text.", text},
		{"developer", "Software developer",
		 "Build, debug, test and review software. Requires text.", text},
		{"photographer", "Photographer",
		 "Plan shoots and critique photos. Requires text and vision.", vision},
		{"photo_editor", "Photo editor",
		 "Retouch and transform photos. Requires text, vision and images.", image},
		{"writer", "Writer", "Develop ideas, draft and revise writing. Requires text.", text},
		{"designer", "Designer",
		 "Create and evaluate visual designs. Requires text, vision and images.", image},
		{"video_creator", "Video creator", "Requires all four models: text, vision, images and video.",
		 image | (1u << SETUP_VIDEO)},
		{"researcher", "Researcher", "Gather evidence and develop analyses. Requires text.", text},
	};
	return roles[index >= 0 && index < SETUP_ROLE_COUNT ? index : SETUP_GENERAL];
}

int cli_setup_role_find(const char *id)
{
	for (int i = 0; id && i < SETUP_ROLE_COUNT; i++)
		if (strcmp(cli_setup_role_get(i).id, id) == 0)
			return i;
	return SETUP_GENERAL;
}

int cli_setup_role_requires(int role, int model)
{
	return model >= 0 && model < SETUP_MODEL_COUNT &&
		(cli_setup_role_get(role).required_models & (1u << (unsigned)model)) != 0;
}

int cli_setup_role_render(int role, char **out)
{
	char root[PATH_MAX];
	char entry[128];
	struct cli_setup_role selected = cli_setup_role_get(role);
	struct morph_prompt_template_variable variables[] = {
		{"role.name", MORPH_PROMPT_TEMPLATE_STRING, selected.label, 0},
	};
	int rc = morph_data_find(root, sizeof(root), "prompts");
	if (rc != 0)
		return rc;
	snprintf(entry, sizeof(entry), "scenarios/%s.txt", selected.id);
	return morph_prompt_template_render_file(root, entry, variables,
		sizeof(variables) / sizeof(variables[0]), out, NULL);
}
