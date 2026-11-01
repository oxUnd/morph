#ifndef CLI_INTERNAL_H
#define CLI_INTERNAL_H

struct cli_context;
#include "sapi/cli/cli.h"
#include "sapi/cli/ui/presentation/feed_style.h"
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
	printf(ctx, ANSI_BOLD ANSI_RED CLI_FEED_FAILED " Error  " ANSI_RESET fmt "\n", ##__VA_ARGS__)
#define CMD_OK(ctx, fmt, ...)                                                                      \
	printf(ctx, ANSI_BOLD ANSI_GREEN CLI_FEED_DONE " " ANSI_RESET fmt "\n", ##__VA_ARGS__)

#define printf cli_printf
#define fputs cli_fputs
#define putchar cli_putchar

#endif
