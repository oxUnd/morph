#ifndef CLI_LIST_UI_H
#define CLI_LIST_UI_H

#include <stddef.h>

int cli_list_columns(void);
size_t cli_list_compact_text(char *dst, size_t dst_cap, const char *src);
void cli_list_group(const char *name, int count, int is_last);
void cli_list_item(const char *ancestor, int is_last, const char *marker,
		   const char *name, const char *description, int name_width,
		   int columns);
void cli_list_row(const char *id, const char *name, const char *metadata,
		  int is_current, int is_last, int columns);
void cli_list_value_field(const char *label, const char *value, int is_last,
			  int label_width, int columns);
void cli_list_text_field(const char *label, const char *value, int is_last,
			 int columns);
void cli_list_json_field(const char *label, const char *json, int is_last,
			 int columns);

#endif
