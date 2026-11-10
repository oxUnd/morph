#ifndef SKILL_ACTIVATE_H
#define SKILL_ACTIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"
#include <stdint.h>
#include "skill/skill.h"

struct db;
int skill_activate_session(struct skill_registry *skills, struct db *db, int64_t session,
			   const char *name, int active);
int skill_restore_session(struct skill_registry *skills, struct db *db, int64_t session);
int skill_activate_rebind(struct tool_registry *reg, struct skill_registry *skills);
int skill_activate_init(struct tool_registry *reg, struct skill_registry *skills);

#ifdef __cplusplus
}
#endif

#endif
