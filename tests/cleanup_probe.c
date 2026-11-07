#include "cleanup_probe.h"
#include "util/cleanup.h"
#include "util/error.h"

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
		return -EINVAL;
	struct cleanup_probe first MORPH_CLEANUP(cleanup_probe_record) = {order, count, 1};
	struct cleanup_probe second MORPH_CLEANUP(cleanup_probe_record) = {order, count, 2};

	if (early)
		return -ECANCELED;
	return 0;
}
