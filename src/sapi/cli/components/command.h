#ifndef MORPH_CLI_COMPONENTS_COMMAND_H
#define MORPH_CLI_COMPONENTS_COMMAND_H

struct nt_builder;
struct nt_view;

/* Results attach to the returned column after the submitted command heading. */
struct nt_view *cli_command_add(struct nt_builder *builder, struct nt_view *parent,
				const char *text, int styled);

#endif
