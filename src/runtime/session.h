#ifndef MORPH_RUNTIME_SESSION_H
#define MORPH_RUNTIME_SESSION_H

#include "../session.h"

struct runtime_engine;
struct react_context;

int runtime_session_get_or_create(struct runtime_engine *engine,
				  const char *name,
				  const char *model,
				  struct session *out,
				  int *created);
int runtime_session_create(struct runtime_engine *engine,
			   const char *name,
			   const char *model,
			   struct session *out);
int runtime_session_switch(struct runtime_engine *engine,
			   const char *name,
			   const char *model,
			   struct session *out,
			   int *created);
int runtime_session_rename(struct runtime_engine *engine,
			   int64_t id,
			   const char *new_name);
int runtime_session_delete(struct runtime_engine *engine, int64_t id);
void runtime_session_load_history(struct runtime_engine *engine,
				  int64_t session_id);
void runtime_session_clear_history(struct react_context *react);

#endif
