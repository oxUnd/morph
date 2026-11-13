#include "sapi/cli/models/agents.h"
#include "nb/err.h"

#include "nb/cleanup.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int cli_agents_model_init(struct cli_agents_model *model)
{
	if (!model)
		return NB_ERR_INVAL;
	memset(model, 0, sizeof(*model));
	return (nb_vec_init(&model->receipts, sizeof(struct cli_agents_receipt)),
		nb_vec_reserve(&model->receipts, 16) ? 0 : NB_ERR_NOMEM);
}

void cli_agents_model_receipts_clear(struct cli_agents_model *model)
{
	struct cli_agents_receipt *receipt;

	if (!model)
		return;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&model->receipts) &&
	     (receipt = (struct cli_agents_receipt *)nb_vec_at(&model->receipts, _i_));
	     _i_++)
		free(receipt->id);
	nb_vec_clear(&model->receipts);
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
	nb_vec_cleanup(&model->receipts);
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
	char *owned NB_CLEANUP(nb_charp_cleanup) = NULL;
	struct cli_agents_receipt *receipt;

	if (!model)
		return NB_ERR_INVAL;
	owned = strdup(id ? id : "agent");
	if (!owned)
		return NB_ERR_NOMEM;
	receipt = nb_vec_push(&model->receipts);
	if (!receipt)
		return NB_ERR_NOMEM;
	receipt->id = nb_charp_steal(&owned);
	receipt->status = status;
	return 0;
}
