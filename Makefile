CXX ?= g++
PKG_CONFIG ?= pkg-config

BUILD_DIR := build

# Two independent DSP plugins share this build/install system:
#   fmheadendlimiter -> libgstfmheadendlimiter.so (lookahead limiter)
#   fmheadendclipper -> libgstfmheadendclipper.so (zero-latency final clipper)
LIMITER_PLUGIN_NAME := fmheadendlimiter
CLIPPER_PLUGIN_NAME := fmheadendclipper

LIMITER_PLUGIN := $(BUILD_DIR)/libgst$(LIMITER_PLUGIN_NAME).so
CLIPPER_PLUGIN := $(BUILD_DIR)/libgst$(CLIPPER_PLUGIN_NAME).so

LIMITER_TEST_BIN := $(BUILD_DIR)/test_core
CLIPPER_TEST_BIN := $(BUILD_DIR)/test_clipper_core

GST_PKGS := gstreamer-1.0 gstreamer-base-1.0 gstreamer-audio-1.0
GST_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(GST_PKGS) 2>/dev/null)
GST_LIBS := $(shell $(PKG_CONFIG) --libs $(GST_PKGS) 2>/dev/null)
GST_PLUGIN_DIR ?= $(shell $(PKG_CONFIG) --variable=pluginsdir gstreamer-1.0 2>/dev/null)

CPPFLAGS += -Isrc
CXXFLAGS += -O3 -std=c++17 -fPIC -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS +=

.PHONY: all plugin test clean install uninstall verify check-deps \
        limiter-plugin clipper-plugin limiter-test clipper-test

all: plugin test

check-deps:
	@$(PKG_CONFIG) --exists $(GST_PKGS) || { \
		echo "Missing GStreamer development packages."; \
		echo "Debian/Raspberry Pi OS: sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev"; \
		exit 1; \
	}

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LIMITER_TEST_BIN): tests/test_core.cpp src/fm_limiter_core.hpp src/fm_preemphasis.hpp src/fm_true_peak.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/test_core.cpp

$(CLIPPER_TEST_BIN): tests/test_clipper_core.cpp src/fm_clipper_core.hpp src/fm_preemphasis.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/test_clipper_core.cpp

limiter-test: $(LIMITER_TEST_BIN)
	./$(LIMITER_TEST_BIN)

clipper-test: $(CLIPPER_TEST_BIN)
	./$(CLIPPER_TEST_BIN)

# Runs all three standalone (GStreamer-independent) DSP test executables.
test: limiter-test clipper-test extension-test

$(BUILD_DIR)/test_extensions: tests/test_extensions.cpp src/fm_limiter_core.hpp src/fm_clipper_core.hpp src/fm_preemphasis.hpp src/fm_true_peak.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/test_extensions.cpp

.PHONY: extension-test benchmark
extension-test: $(BUILD_DIR)/test_extensions
	./$(BUILD_DIR)/test_extensions

benchmark: | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFM_EXTENDED -o $(BUILD_DIR)/benchmark tests/benchmark.cpp
	./$(BUILD_DIR)/benchmark

.PHONY: integration-test analysis-tools
$(BUILD_DIR)/test_properties: tests/test_properties.cpp | $(BUILD_DIR) check-deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GST_CFLAGS) -o $@ $< $(GST_LIBS)

integration-test: plugin $(BUILD_DIR)/test_properties
	GST_PLUGIN_PATH="$(CURDIR)/$(BUILD_DIR)" GST_REGISTRY="$(CURDIR)/$(BUILD_DIR)/test-registry.bin" ./$(BUILD_DIR)/test_properties
	GST_PLUGIN_PATH="$(CURDIR)/$(BUILD_DIR)" GST_REGISTRY="$(CURDIR)/$(BUILD_DIR)/test-registry.bin" bash scripts/smoke_test.sh
	GST_PLUGIN_PATH="$(CURDIR)/$(BUILD_DIR)" GST_REGISTRY="$(CURDIR)/$(BUILD_DIR)/test-registry.bin" bash scripts/smoke_test_clipper.sh

analysis-tools: | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $(BUILD_DIR)/render_audio tests/render_audio.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFM_TP_NEON -o $(BUILD_DIR)/render_audio_neon tests/render_audio.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-vectorize -ffp-contract=off -o $(BUILD_DIR)/render_audio_scalar tests/render_audio.cpp

$(LIMITER_PLUGIN): src/gstfmheadendlimiter.cpp src/fm_limiter_core.hpp src/fm_preemphasis.hpp src/fm_true_peak.hpp | $(BUILD_DIR) check-deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GST_CFLAGS) -shared -o $@ src/gstfmheadendlimiter.cpp $(GST_LIBS)

$(CLIPPER_PLUGIN): src/gstfmheadendclipper.cpp src/fm_clipper_core.hpp src/fm_preemphasis.hpp | $(BUILD_DIR) check-deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GST_CFLAGS) -shared -o $@ src/gstfmheadendclipper.cpp $(GST_LIBS)

limiter-plugin: $(LIMITER_PLUGIN)
clipper-plugin: $(CLIPPER_PLUGIN)

# Builds both plugin shared libraries.
plugin: limiter-plugin clipper-plugin

install: plugin
	@test -n "$(GST_PLUGIN_DIR)" || { echo "Could not determine GStreamer plugin directory"; exit 1; }
	install -d "$(DESTDIR)$(GST_PLUGIN_DIR)"
	install -m 0755 "$(LIMITER_PLUGIN)" "$(DESTDIR)$(GST_PLUGIN_DIR)/libgst$(LIMITER_PLUGIN_NAME).so"
	install -m 0755 "$(CLIPPER_PLUGIN)" "$(DESTDIR)$(GST_PLUGIN_DIR)/libgst$(CLIPPER_PLUGIN_NAME).so"
	@echo "Installed to $(DESTDIR)$(GST_PLUGIN_DIR)/libgst$(LIMITER_PLUGIN_NAME).so"
	@echo "Installed to $(DESTDIR)$(GST_PLUGIN_DIR)/libgst$(CLIPPER_PLUGIN_NAME).so"

uninstall:
	@test -n "$(GST_PLUGIN_DIR)" || { echo "Could not determine GStreamer plugin directory"; exit 1; }
	rm -f "$(DESTDIR)$(GST_PLUGIN_DIR)/libgst$(LIMITER_PLUGIN_NAME).so"
	rm -f "$(DESTDIR)$(GST_PLUGIN_DIR)/libgst$(CLIPPER_PLUGIN_NAME).so"

verify:
	gst-inspect-1.0 fmheadendlimiter
	gst-inspect-1.0 fmheadendclipper
	bash scripts/smoke_test.sh
	bash scripts/smoke_test_clipper.sh

clean:
	rm -rf $(BUILD_DIR)
