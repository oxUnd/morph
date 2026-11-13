#ifndef MORPH_CLI_MODELS_AGENTS_H
#define MORPH_CLI_MODELS_AGENTS_H

#include "agent/agent_control.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include <stdint.h>

struct cli_agents_receipt {
	char *id;
	enum agent_status status;
};

/* Display projection of the runtime's agent snapshots. Owned by the
 * application; services replace it, presentation renders it. */
struct cli_agents_model {
	struct agent_info *items;
	int count;
	int64_t session_id;
	nb_vec receipts;
};

int cli_agents_model_init(struct cli_agents_model *model);
void cli_agents_model_cleanup(struct cli_agents_model *model);
void cli_agents_model_set(struct cli_agents_model *model, struct agent_info *items, int count,
			  int64_t session_id);
int cli_agents_model_active(const struct cli_agents_model *model);
int cli_agents_model_receipt_add(struct cli_agents_model *model, const char *id,
				 enum agent_status status);
void cli_agents_model_receipts_clear(struct cli_agents_model *model);

#endif
