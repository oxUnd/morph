#ifndef MORPH_CLI_COMMANDS_SKILLS_MANAGE_H
#define MORPH_CLI_COMMANDS_SKILLS_MANAGE_H
struct nb_log;
int cli_skills_manage_run(struct nb_log *logger, int argc, char **argv, const char *config_path);
#endif
