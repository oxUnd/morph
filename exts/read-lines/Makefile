CMAKE ?= cmake
BUILD_DIR ?= build

.PHONY: all test clean

all:
	$(CMAKE) -S . -B $(BUILD_DIR)
	$(CMAKE) --build $(BUILD_DIR)
	cp $(BUILD_DIR)/read_lines.so read_lines.so

test: all
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR) read_lines.so
