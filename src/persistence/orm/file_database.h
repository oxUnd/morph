#ifndef MORPH_ORM_FILE_DATABASE_H
#define MORPH_ORM_FILE_DATABASE_H

#include "persistence/orm/database.h"

int orm_database_quick_check(const char *path);
int orm_database_is_sqlite(const char *path);
int orm_database_snapshot(const char *source, const char *destination, int *page_size);
int orm_database_merge_display(const char *snapshot, const char *display_snapshot);

#endif
