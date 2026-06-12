CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
TARGET   = pii_check.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): pii_check.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ pii_check.c

clean:
	rm -f $(TARGET)
