#include "name.h"
#include <string.h>

int morph_name_char_valid(char c, const char *punctuation)
{
	return (c >= 'A' && c <= 'Z') ||
		(c >= 'a' && c <= 'z') ||
		(c >= '0' && c <= '9') ||
		(c != '\0' && punctuation && strchr(punctuation, c) != NULL);
}

int morph_name_valid(const char *name, const char *punctuation)
{
	if (!name || !*name)
		return 0;
	for (const char *p = name; *p; p++) {
		if (!morph_name_char_valid(*p, punctuation))
			return 0;
	}
	return 1;
}

int morph_env_name_valid(const char *name)
{
	if (!morph_name_valid(name, "_"))
		return 0;
	return !(name[0] >= '0' && name[0] <= '9');
}
