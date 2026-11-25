#include "persistence/orm/session_sync_audit.h"
#include <nb/cleanup.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	cJSON *report NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	char *text NB_CLEANUP(nb_charp_cleanup) = NULL;

	if (argc != 5 || strcmp(argv[3], "--platform")) {
		fprintf(stderr, "Usage: %s CORE UI --platform android|ios\n", argv[0]);
		return 1;
	}

	int rc = session_sync_audit_copy(argv[1], argv[2], argv[4], &report);

	if (rc == 0) {
		text = cJSON_Print(report);
		if (!text)
			rc = 1;
	}

	if (rc != 0) {
		puts("{\"error\":\"Cannot audit input: check files, schemas, integrity and "
		     "journals\"}");
		return 1;
	}

	puts(text);
	return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(report, "has_findings")) ? 2 : 0;
}
