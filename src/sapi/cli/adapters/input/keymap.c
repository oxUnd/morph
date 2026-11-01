#include "sapi/cli/adapters/input/keymap.h"
#ifdef HAVE_READLINE
#include <stdio.h>
#include <readline/readline.h>
#endif
#include "sapi/cli/application_internal.h"

#ifdef HAVE_READLINE
void cli_keymap_free(Keymap map)
{
	if (map) {
		rl_discard_keymap(map);
		free(map);
	}
}

/* Readline's rl_copy_keymap shares submaps, including static library tables.
 * Clone each path so bindings and their cleanup belong entirely to this run. */
static Keymap cli_keymap_copy_nested(Keymap source, Keymap *ancestors, size_t depth)
{
	Keymap map;

	if (depth >= 64)
		return NULL;
	for (size_t i = 0; i < depth; i++) {
		if (source == ancestors[i])
			return NULL;
	}
	ancestors[depth] = source;
	map = rl_make_bare_keymap();
	if (!map)
		return NULL;
	for (int i = 0; i < KEYMAP_SIZE; i++) {
		if (source[i].type == ISKMAP) {
			Keymap child;

			_Static_assert(sizeof(child) == sizeof(source[i].function),
				       "incompatible keymap ABI");
			memcpy(&child, &source[i].function, sizeof(child));
			child = cli_keymap_copy_nested(child, ancestors, depth + 1);
			if (!child) {
				cli_keymap_free(map);
				return NULL;
			}
			map[i].type = ISKMAP;
			memcpy(&map[i].function, &child, sizeof(child));
		} else if (source[i].type == ISMACR) {
			char *text;

			memcpy(&text, &source[i].function, sizeof(text));
			text = text ? strdup(text) : NULL;
			if (!text && source[i].function) {
				cli_keymap_free(map);
				return NULL;
			}
			map[i].type = ISMACR;
			memcpy(&map[i].function, &text, sizeof(text));
		} else {
			map[i] = source[i];
		}
	}
	return map;
}

Keymap cli_keymap_copy(Keymap source)
{
	Keymap ancestors[64];

	return source ? cli_keymap_copy_nested(source, ancestors, 0) : NULL;
}

#endif
