CC ?= cc
PYTHON ?= python3
SANITIZE_CC ?= $(CC)
.DEFAULT_GOAL := all

KILIX_GAME_KIT_DIR ?= third_party/kilix-game-kit
KILIX_GAME_KIT_ROOT := $(abspath $(KILIX_GAME_KIT_DIR))
include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk

KILIX_TOP_DOWN_DIR ?= third_party/kilix-top-down-engine
include $(KILIX_TOP_DOWN_DIR)/mk/kilix-top-down.mk

KILIX_ASSETS_DIR ?= third_party/kilix-assets
include $(KILIX_ASSETS_DIR)/mk/kilix-assets.mk

KILIX_UI_DIR ?= third_party/kilix-ui
include $(KILIX_UI_DIR)/mk/kilix-ui.mk

override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	$(KILIX_GAME_KIT_CPPFLAGS) $(KILIX_TD_CPPFLAGS) \
	$(KILIX_ASSETS_CPPFLAGS) $(KILIX_UI_CPPFLAGS)
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
SANITIZE_CFLAGS := -O1 -g -std=c11 -pthread $(WARNINGS) \
	-fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -pthread $(WARNINGS) -MMD -MP
LDLIBS := $(KILIX_ASSETS_LDLIBS) $(KILIX_GAME_KIT_LDLIBS)

BIN := kilix-land-desktop
SRC := src/main.c src/desk.c src/rooms.c src/json_reader.c src/items.c \
	src/world_state.c src/state_store.c src/launcher.c src/laptop.c \
	src/graphics.c src/render.c src/audio.c
OBJ := $(patsubst src/%.c,build/%.o,$(SRC))
DEPENDENCIES := $(OBJ:.o=.d)
GRAPHICS_MANIFEST := assets/graphics/manifest.json
GAMES_ROOT ?= ../../games
PARITY_TOOL := tools/sync_source_parity.py
AUDIO_TOOL := tools/generate_audio.py

all: $(BIN)

$(BIN): $(OBJ) $(GRAPHICS_MANIFEST) $(KILIX_UI_LIB) $(KILIX_TD_LIBS) \
	$(KILIX_ASSETS_LIB) $(KILIX_GAME_KIT_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(KILIX_UI_LIB) $(KILIX_TD_LIBS) \
		$(KILIX_ASSETS_LIB) $(KILIX_GAME_KIT_LIB) $(LDLIBS)

build/%.o: src/%.c src/kilix_land_desktop.h src/source_parity.h \
	src/json_reader.h src/items.h src/laptop.h src/world_state.h \
	src/state_store.h \
	$(KILIX_ASSETS_ROOT)/include/kilix_assets.h \
	$(KILIX_UI_ROOT)/include/kilix_ui.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p $@

parity-sync:
	$(PYTHON) $(PARITY_TOOL) --games-root "$(abspath $(GAMES_ROOT))" --write

# Verifies every source game you actually have. The source games are separate
# repositories, so a checkout without them beside it (any clone that is not a
# maintainer's) skips the games it cannot see and checks the rest. Each game's
# parity switches back on by itself once that game is checked out here.
parity-check:
	$(PYTHON) $(PARITY_TOOL) --games-root "$(abspath $(GAMES_ROOT))" \
		--skip-absent

# Fails if any source game is missing: use when parity must be fully proven.
parity-check-strict:
	$(PYTHON) $(PARITY_TOOL) --games-root "$(abspath $(GAMES_ROOT))"

items-art:
	$(PYTHON) tools/sync_item_art.py

actions-art:
	$(PYTHON) tools/sync_action_art.py

audio-plan:
	$(PYTHON) tools/validate_audio.py --plan

audio-requests:
	$(PYTHON) $(AUDIO_TOOL) requests

audio-record:
	$(PYTHON) $(AUDIO_TOOL) record-minimax \
		--generated-on "$(MINIMAX_GENERATED_ON)"

audio:
	$(PYTHON) $(AUDIO_TOOL) build

MASK_DIR := third_party/kilix-mask
MASK_LIB := $(MASK_DIR)/build/libkilix-mask.so

$(MASK_LIB):
	@test -f $(MASK_DIR)/Makefile || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(MAKE) -C $(MASK_DIR)

# Converting a room out to kilix-mask and back must reproduce world.json
# and every behind mask exactly.  Checked against the real assets, not a
# synthetic room: only those prove the conversion agrees with what the
# engine already loads.
.PHONY: region-editor-check
region-editor-check: $(MASK_LIB)
	$(PYTHON) tools/region_editor.py --selftest

test: parity-check $(BIN)
	$(PYTHON) tools/validate_world.py assets/world/world.json
	$(PYTHON) tools/validate_items.py assets/world/items.json
	$(PYTHON) tools/validate_visual.py
	$(PYTHON) tools/validate_audio.py --plan
	$(PYTHON) tools/land_config.py --check
	$(MAKE) --no-print-directory region-editor-check
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py'
	./$(BIN) --selftest
	./$(BIN) --audio-test
	./$(BIN) --graphics-test
	./$(BIN) --json-test
	./$(BIN) --items-test
	./$(BIN) --world-test
	./$(BIN) --doors-test
	./$(BIN) --profile-test
	./$(BIN) --laptop-test
	./$(BIN) --wizard-render-test build/wizard-preview
	./$(BIN) --room-render-test build/room-preview
	./$(BIN) --outfit-render-test build/outfit-preview
	./$(BIN) --walk-render-test build/walk-preview
	./$(BIN) --items-render-test build/items-preview

test-deps:
	$(MAKE) -C $(KILIX_GAME_KIT_ROOT) test
	$(MAKE) -C $(KILIX_TOP_DOWN_ROOT) \
		SOFT_RASTER_DIR="$(SOFT_RASTER_DIR)" test

sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) test CC="$(SANITIZE_CC)" CFLAGS="$(SANITIZE_CFLAGS)"
	$(MAKE) clean
	$(MAKE) all

release-check: parity-check-strict test sanitize

clean:
	$(RM) -r build $(BIN)

.PHONY: all actions-art audio audio-plan audio-record audio-requests clean \
	parity-check-strict \
	items-art parity-check parity-sync release-check test test-deps sanitize

-include $(DEPENDENCIES)
