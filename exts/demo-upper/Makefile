CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
CPPFLAGS += -I../../vendor/nothing/include
TARGET   = upper.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): upper.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC -o $@ upper.c

clean:
	rm -f $(TARGET)
