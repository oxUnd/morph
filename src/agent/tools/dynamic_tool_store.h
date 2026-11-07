#ifndef MORPH_DYNAMIC_TOOL_STORE_H
#define MORPH_DYNAMIC_TOOL_STORE_H

#include "dynamic_tool_internal.h"

int dynamic_tool_store_copy_tool_snapshot(const char *source_meta, const char *source_file,
					  const char *destination_meta,
					  const char *destination_file);
int dynamic_tool_store_write_tool_files(struct dynamic_tool *dt, const char *source);
int dynamic_tool_store_tool_file_paths(const char *dir, char *meta_path, size_t meta_size,
				       char *source_path, size_t source_size);
int dynamic_tool_store_history_dir_path(const char *tool_dir, char *path, size_t path_size);
int dynamic_tool_store_checkpoint_dir_path(const char *tool_dir, const char *checkpoint_id,
					   char *path, size_t path_size);
void dynamic_tool_store_hash_text(const char *data, size_t len, char out[32]);
int dynamic_tool_store_checkpoint_id_value(const char *id);
int dynamic_tool_store_latest_checkpoint_id(const char *tool_dir, char *id, size_t id_size);
int dynamic_tool_store_write_checkpoint_meta(const char *checkpoint_dir,
					     const struct tool_checkpoint *cp);
int dynamic_tool_store_read_checkpoint_meta(const char *checkpoint_dir, struct tool_checkpoint *cp);
int dynamic_tool_store_remove_checkpoint_ids_from(const char *tool_dir, int min_id);
int dynamic_tool_store_prune_checkpoint_history(const char *tool_dir);
int dynamic_tool_store_create_tool_checkpoint(const char *tool_dir,
					      const struct dynamic_tool *old_dt, const char *name,
					      struct tool_checkpoint *cp, char *checkpoint_dir,
					      size_t checkpoint_dir_size);
int dynamic_tool_store_restore_tool_checkpoint(const char *tool_dir, const char *checkpoint_dir,
					       const struct tool_checkpoint *cp);
int dynamic_tool_store_remove_tree(const char *path);
char *dynamic_tool_store_read_optional_file(const char *path);
int dynamic_tool_store_append_whole_file_diff(morph_buf_t *buf, const char *label,
					      const char *old_text, const char *new_text);

#endif
