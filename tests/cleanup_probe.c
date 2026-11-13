#include "cleanup_probe.h"
#include "nb/cleanup.h"
#include "errors.h"

struct cleanup_probe {
	int *order;
	int *count;
	int identity;
};

static void cleanup_probe_record(struct cleanup_probe *probe)
{
	probe->order[*probe->count] = probe->identity;
	(*probe->count)++;
}

int cleanup_probe_run(int early, int *order, int *count)
{
	if (!order || !count)
		return NB_ERR_INVAL;
	struct cleanup_probe first NB_CLEANUP(cleanup_probe_record) = {order, count, 1};
	struct cleanup_probe second NB_CLEANUP(cleanup_probe_record) = {order, count, 2};

	if (early)
		return NB_ERR_CANCELLED;
	return 0;
}
