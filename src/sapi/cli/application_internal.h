#ifndef MORPH_CLI_APPLICATION_INTERNAL_H
#define MORPH_CLI_APPLICATION_INTERNAL_H

/*
 * Compatibility include for implementation modules. It owns output
 * conveniences only; domain SDK headers belong to the modules that use them.
 */
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/presentation/status.h"
#include "sapi/cli/presentation/style.h"
#include "util/log.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
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

#endif
