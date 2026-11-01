#ifndef MORPH_CLI_APPLICATION_INTERNAL_H
#define MORPH_CLI_APPLICATION_INTERNAL_H

struct cli_application;
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/style.h"
#include "util/log.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/arena.h"
#include "util/utf8.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <poll.h>

#define CMD_HEADER(ctx, fmt, ...)                                                                  \
	printf(ctx, "\n" ANSI_BOLD ANSI_CYAN "● " fmt ANSI_RESET "\n", ##__VA_ARGS__)
#define CMD_ERROR(ctx, fmt, ...)                                                                   \
	printf(ctx,                                                                                \
	       ANSI_BOLD ANSI_RED CLI_FEED_FAILED " Error  " ANSI_RESET fmt "\n", ##__VA_ARGS__)
#define CMD_OK(ctx, fmt, ...)                                                                      \
	printf(ctx, ANSI_BOLD ANSI_GREEN CLI_FEED_DONE " " ANSI_RESET fmt "\n", ##__VA_ARGS__)

#define printf cli_output_printf
#define fputs cli_output_fputs
#define putchar cli_output_putchar

#endif
