#include "read_lines.h"
#include "nb/cleanup.h"

#include <stdio.h>

int main(int argc, char **argv)
{
	char *result NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (argc != 2)
		return 2;
	int rc = ext_run(argv[1], &result);
	if (printf("%d\n%s\n", rc, result ? result : "null") < 0)
		return 3;
	return 0;
}
