#ifndef MORPH_CLI_SETUP_ROLE_H
#define MORPH_CLI_SETUP_ROLE_H

enum cli_setup_role_id {
	SETUP_GENERAL, SETUP_DEVELOPER, SETUP_PHOTOGRAPHER, SETUP_PHOTO_EDITOR,
	SETUP_WRITER, SETUP_DESIGNER, SETUP_VIDEO_CREATOR, SETUP_RESEARCHER,
	SETUP_ROLE_COUNT
};

struct cli_setup_role {
	const char *id;
	const char *label;
	const char *description;
	unsigned required_models;
};

#ifdef __cplusplus
extern "C" {
#endif

struct cli_setup_role cli_setup_role_get(int index);
int cli_setup_role_find(const char *id);
int cli_setup_role_requires(int role, int model);
int cli_setup_role_render(int role, char **out);

#ifdef __cplusplus
}
#endif
#endif
