#ifndef MORPH_PERMISSION_PATH_H
#define MORPH_PERMISSION_PATH_H

#include <stddef.h>

int permission_path_resolve_absolute(const char *path, char *resolved, size_t size);
int permission_path_resolve_existing_directory(const char *path, int create, char *resolved,
					       size_t size);

#endif
