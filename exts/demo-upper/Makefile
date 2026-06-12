CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
TARGET   = upper.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): upper.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ upper.c

clean:
	rm -f $(TARGET)
