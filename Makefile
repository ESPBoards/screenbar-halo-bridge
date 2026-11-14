# SPDX-License-Identifier: GPL-3.0-only
# Host tests for lib/. No ESP-IDF needed, so they run anywhere.

CC     ?= cc
CFLAGS ?= -O2
# Kept even when CFLAGS is given on the make command line, so a warning still fails the build.
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS ?= -lm
BUILD  := build-host

LIB_SRC  := $(wildcard lib/*/src/*.c)
LIB_HDR  := $(wildcard lib/*/include/*.h)
LIB_INC  := $(addprefix -I,$(wildcard lib/*/include))
TEST_HDR := $(wildcard test/*.h)
TESTS    := $(patsubst test/%.c,$(BUILD)/%,$(wildcard test/test_*.c))

test: check-copies $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

# The C3's ESPHome component keeps copies of the lib/ files it needs: ESPHome
# copies only the component folder, and symlinks break on Windows.
# `make sync` refreshes them; `make test` fails if they drifted.
ESPHOME_DIR := esphome/components/halo_bridge
SHARED      := lib/lamp/include/halo_lamp.h lib/lamp/include/halo_payload.h lib/lamp/src/halo_payload.c \
               lib/wire/include/halo_wire.h lib/wire/src/halo_wire.c

sync:
	cp $(SHARED) $(ESPHOME_DIR)/

check-copies:
	@for f in $(SHARED); do \
		copy=$(ESPHOME_DIR)/$$(basename $$f); \
		cmp -s $$f $$copy || { echo "$$copy differs from $$f: run make sync"; exit 1; }; \
	done

$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(TEST_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(LIB_INC) $< $(LIB_SRC) $(LDLIBS) -o $@

$(BUILD):
	mkdir -p $@

# Dev tool, not a test: replays the raw captures in a tools/s3-bench serial log.
replay: $(BUILD)/replay

$(BUILD)/replay: tools/replay.c $(LIB_SRC) $(LIB_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(LIB_INC) $< $(LIB_SRC) $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD)

.PHONY: test replay clean sync check-copies
