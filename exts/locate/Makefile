CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
TARGET   = locate
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
	# macOS: compile as Obj-C, link CoreLocation
	CFLAGS  += -x objective-c
	LIBS     = -framework Foundation -framework CoreLocation
	# .app bundle paths
	APP_BUNDLE = Locate.app
	APP_BINARY = $(APP_BUNDLE)/Contents/MacOS/$(TARGET)
	APP_PLIST  = $(APP_BUNDLE)/Contents/Info.plist
	ENTITLE    = entitlements.plist

all: $(TARGET)
	@# Create .app bundle structure
	@mkdir -p $(APP_BUNDLE)/Contents/MacOS
	@cp $(TARGET) $(APP_BINARY)
	@cp Info.plist $(APP_PLIST)
	@# Sign the .app bundle with location entitlement
	@codesign --force --sign - --entitlements $(ENTITLE) $(APP_BUNDLE) 2>/dev/null || true
	@# Symlink so entry="locate" resolves to the binary inside the bundle
	@ln -sf $(APP_BINARY) $(TARGET)
	@echo "Built $(APP_BUNDLE)"

$(TARGET): locate.c
	$(CC) $(CFLAGS) -o $@ locate.c $(LIBS)

else
	# Linux: compile plain C, link sd-bus for GeoClue2 D-Bus
	LIBS = -lsystemd

$(TARGET): locate.c
	$(CC) $(CFLAGS) -o $@ locate.c $(LIBS)

endif

.PHONY: all clean install

clean:
	rm -f $(TARGET)
	rm -rf $(APP_BUNDLE)

install: $(TARGET)
	cp manifest.toml ~/.morph/exts/locate/
	cp $(TARGET) ~/.morph/exts/locate/
	cp -r $(APP_BUNDLE) ~/.morph/exts/locate/ 2>/dev/null || true
