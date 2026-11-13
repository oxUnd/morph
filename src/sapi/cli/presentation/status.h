#ifndef MORPH_CLI_PRESENTATION_STATUS_H
#define MORPH_CLI_PRESENTATION_STATUS_H

#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"

#define CMD_HEADER(ctx, fmt, ...)                                                                  \
	CLI_OUTPUT_PRINTF(ctx, "\n" ANSI_BOLD ANSI_CYAN CLI_FEED_MESSAGE " " fmt ANSI_RESET         \
			  "\n", ##__VA_ARGS__)
#define CMD_ERROR(ctx, fmt, ...)                                                                   \
	CLI_OUTPUT_PRINTF(ctx, CLI_FEED_FAILED_STYLE CLI_FEED_FAILED " Error  " ANSI_RESET fmt      \
			  "\n", ##__VA_ARGS__)
#define CMD_OK(ctx, fmt, ...)                                                                      \
	CLI_OUTPUT_PRINTF(ctx, ANSI_BOLD ANSI_GREEN CLI_FEED_DONE " " ANSI_RESET fmt "\n",         \
			  ##__VA_ARGS__)

#endif
