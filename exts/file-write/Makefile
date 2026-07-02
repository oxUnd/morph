CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
ROOT     = ../..
TARGET   = test_file_write
NO_GNU_ZERO_VARIADIC := $(shell $(CC) \
	-Wno-gnu-zero-variadic-macro-arguments -E -x c /dev/null \
	>/dev/null 2>&1 && echo -Wno-gnu-zero-variadic-macro-arguments)
CFLAGS  += $(NO_GNU_ZERO_VARIADIC)

INCLUDES = -I. -I$(ROOT)/src -I$(ROOT)/vendor
SRCS = \
	test_file_write.c \
	file_write.c \
	$(ROOT)/src/agent/tool.c \
	$(ROOT)/src/agent/tool_context.c \
	$(ROOT)/src/util/arena.c \
	$(ROOT)/src/util/array.c \
	$(ROOT)/src/util/buf.c \
	$(ROOT)/src/util/error.c \
	$(ROOT)/src/util/file.c \
	$(ROOT)/src/util/log.c \
	$(ROOT)/src/util/str.c \
	$(ROOT)/src/util/strmap.c \
	$(ROOT)/vendor/cJSON.c

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SRCS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
