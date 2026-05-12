#include "cli.h"
#include "util/log.h"
#include "util/file.h"
#include "http/client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
	const char *config_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
			config_path = argv[++i];
		else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
			config_path = argv[++i];
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			printf("Usage: multi-agent [-c config_path]\n");
			return 0;
		}
	}
	char *log_dir = file_expand_path("~/.multi-agent/log");
	file_ensure_dir(log_dir);
	char log_path[512];
	snprintf(log_path, sizeof(log_path), "%s/agent.log", log_dir);
	free(log_dir);
	log_init(log_path, LOG_INFO);
	http_init();
	struct cli_context ctx;
	int rc = cli_init(&ctx, config_path);
	if (rc < 0) {
		log_err("failed to initialize: %d", rc);
		return 1;
	}
	cli_run(&ctx);
	cli_shutdown(&ctx);
	http_cleanup();
	log_shutdown();
	return 0;
}