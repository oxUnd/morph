#ifndef MORPH_CLI_UI_INPUT_KEYMAP_H
#define MORPH_CLI_UI_INPUT_KEYMAP_H

#ifdef HAVE_READLINE
#include <stdio.h>
#include <readline/keymaps.h>
Keymap cli_keymap_copy(Keymap source);
void cli_keymap_free(Keymap map);
#endif

#endif
