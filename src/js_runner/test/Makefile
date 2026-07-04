ROOT := ../../..
BUILD ?= $(ROOT)/build
RUNNER ?= $(BUILD)/morph-js-runner
OUT ?= /tmp/morph-js-runner-ut
SCRIPT := ut.js
CAPS := fs_read,fs_write,env,shell,process,image,wasm
READ_ALLOW := $(OUT),$(ROOT),$(BUILD)
WRITE_ALLOW := $(OUT)
CMD_ALLOW := printf morph-exec
NETWORK_ALLOW ?= https://example.com
RUN_NETWORK ?= 0
URL ?= https://example.com

.PHONY: test check network clean runner

test: runner
	mkdir -p "$(OUT)"
	printf '{"jsonrpc":"2.0","id":1,"params":{"out":"$(OUT)","runNetwork":false}}\n' | \
		env MORPH_DYNAMIC_CAPS="$(CAPS)" \
		MORPH_DYNAMIC_ALLOWED_READ="$(READ_ALLOW)" \
		MORPH_DYNAMIC_ALLOWED_WRITE="$(WRITE_ALLOW)" \
		MORPH_DYNAMIC_ALLOWED_COMMANDS="$(CMD_ALLOW)" \
		MORPH_DYNAMIC_ALLOWED_NETWORK="$(NETWORK_ALLOW)" \
		MORPH_JS_RUNNER_UT=ok \
		"$(RUNNER)" "$(SCRIPT)"

network: runner
	mkdir -p "$(OUT)"
	printf '{"jsonrpc":"2.0","id":1,"params":{"out":"$(OUT)","runNetwork":true,"url":"$(URL)"}}\n' | \
		env MORPH_DYNAMIC_CAPS="$(CAPS),network" \
		MORPH_DYNAMIC_ALLOWED_READ="$(READ_ALLOW)" \
		MORPH_DYNAMIC_ALLOWED_WRITE="$(WRITE_ALLOW)" \
		MORPH_DYNAMIC_ALLOWED_COMMANDS="$(CMD_ALLOW)" \
		MORPH_DYNAMIC_ALLOWED_NETWORK="$(NETWORK_ALLOW)" \
		MORPH_JS_RUNNER_UT=ok \
		"$(RUNNER)" "$(SCRIPT)"

check: runner
	"$(RUNNER)" --check "$(SCRIPT)"

runner:
	cmake --build "$(BUILD)" --target morph-js-runner

clean:
	rm -rf "$(OUT)"
