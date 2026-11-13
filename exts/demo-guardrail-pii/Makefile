CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
CPPFLAGS += -I../../vendor/nothing/include
TARGET   = pii_check.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): pii_check.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC -o $@ pii_check.c

clean:
	rm -f $(TARGET)
