CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
CPPFLAGS += -I../../vendor/nothing/include
TARGET   = agent_ui_tags.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): agent_ui_tags.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC -o $@ agent_ui_tags.c

clean:
	rm -f $(TARGET)
