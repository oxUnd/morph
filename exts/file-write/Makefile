CMAKE     ?= cmake
ROOT      = ../..
BUILD_DIR ?= $(ROOT)/build

.PHONY: all test clean

# Use the core's link targets so the test follows tool-context dependencies.
all:
	$(CMAKE) -S $(ROOT) -B $(BUILD_DIR) -DBUILD_TESTS=ON
	$(CMAKE) --build $(BUILD_DIR) --target morph-file-write-tests

test: all
	ctest --test-dir $(BUILD_DIR) --output-on-failure -R '^file_write_extension$$'

clean:
	rm -f $(BUILD_DIR)/bin/morph-file-write-tests
