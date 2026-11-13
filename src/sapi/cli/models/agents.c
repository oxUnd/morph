#include "sapi/cli/models/agents.h"

#include "util/cleanup.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int cli_agents_model_init(struct cli_agents_model *model)
{
	if (!model)
		return -EINVAL;
	memset(model, 0, sizeof(*model));
	return morph_array_init(&model->receipts, MORPH_ARRAY_INIT_CAP,
				sizeof(struct cli_agents_receipt));
}

void cli_agents_model_receipts_clear(struct cli_agents_model *model)
{
	struct cli_agents_receipt *receipt;

	if (!model)
		return;
	morph_array_foreach (receipt, &model->receipts, struct cli_agents_receipt)
		free(receipt->id);
	morph_array_clear(&model->receipts);
}

static void cli_agents_model_snapshot_clear(struct cli_agents_model *model)
{
	if (model->items)
		agent_control_free_list(model->items, model->count);
	model->items = NULL;
	model->count = 0;
}

void cli_agents_model_cleanup(struct cli_agents_model *model)
{
	if (!model)
		return;
	cli_agents_model_snapshot_clear(model);
	cli_agents_model_receipts_clear(model);
	morph_array_cleanup(&model->receipts);
}

void cli_agents_model_set(struct cli_agents_model *model, struct agent_info *items, int count,
			  int64_t session_id)
{
	if (!model)
		return;
	cli_agents_model_snapshot_clear(model);
	model->items = items;
	model->count = count;
	model->session_id = session_id;
}

int cli_agents_model_active(const struct cli_agents_model *model)
{
	if (!model)
		return 0;
	for (int i = 0; i < model->count; i++) {
		if (model->items[i].status == AGENT_RUNNING ||
		    model->items[i].status == AGENT_PENDING)
			return 1;
	}
	return 0;
}

int cli_agents_model_receipt_add(struct cli_agents_model *model, const char *id,
				 enum agent_status status)
{
	char *owned MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	struct cli_agents_receipt *receipt;

	if (!model)
		return -EINVAL;
	owned = strdup(id ? id : "agent");
	if (!owned)
		return -ENOMEM;
	receipt = morph_array_push_borrow(&model->receipts);
	if (!receipt)
		return -ENOMEM;
	receipt->id = morph_charp_steal(&owned);
	receipt->status = status;
	return 0;
}
