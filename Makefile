CC ?= cc
PKGS = x11 x11-xcb xcb xcomposite xdamage xfixes xext xrender egl glesv2

BUILD ?= release
ifeq ($(filter $(BUILD),debug release),)
$(error BUILD must be debug or release)
endif

BUILD_DIR := build/$(BUILD)

CPPFLAGS += -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags $(PKGS))
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -g
ifeq ($(BUILD),debug)
CFLAGS += -O1 -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
else
CFLAGS += -O2
endif
LDLIBS += $(shell pkg-config --libs $(PKGS))

SRCS = src/main.c src/x11.c src/egl.c src/render.c src/stats.c src/background.c src/damage.c src/x11_errors.c
OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

all: $(BUILD_DIR)/slopcom $(BUILD_DIR)/fixture slopcom

$(BUILD_DIR)/slopcom: $(OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

slopcom: $(BUILD_DIR)/slopcom
	ln -sf $(BUILD_DIR)/slopcom $@

$(BUILD_DIR)/fixture: tools/fixture.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILD_DIR)/window_lifecycle: tests/window_lifecycle.c tests/x11_stubs.c tests/x11_stubs.h $(BUILD_DIR)/x11.o $(BUILD_DIR)/damage.o $(BUILD_DIR)/x11_errors.o $(BUILD_DIR)/stats.o $(BUILD_DIR)/background.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ $< tests/x11_stubs.c $(BUILD_DIR)/x11.o $(BUILD_DIR)/damage.o $(BUILD_DIR)/x11_errors.o $(BUILD_DIR)/stats.o $(BUILD_DIR)/background.o $(LDLIBS)

$(BUILD_DIR)/rendering: tests/rendering.c tests/x11_stubs.c tests/x11_stubs.h $(BUILD_DIR)/x11.o $(BUILD_DIR)/damage.o $(BUILD_DIR)/x11_errors.o $(BUILD_DIR)/render.o $(BUILD_DIR)/stats.o $(BUILD_DIR)/background.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ $< tests/x11_stubs.c $(BUILD_DIR)/x11.o $(BUILD_DIR)/damage.o $(BUILD_DIR)/x11_errors.o $(BUILD_DIR)/render.o $(BUILD_DIR)/stats.o $(BUILD_DIR)/background.o $(LDLIBS)

$(BUILD_DIR)/background: tests/background.c tests/x11_stubs.c tests/x11_stubs.h $(BUILD_DIR)/x11.o $(BUILD_DIR)/damage.o $(BUILD_DIR)/x11_errors.o $(BUILD_DIR)/render.o $(BUILD_DIR)/stats.o $(BUILD_DIR)/background.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ $< tests/x11_stubs.c $(BUILD_DIR)/x11.o $(BUILD_DIR)/damage.o $(BUILD_DIR)/x11_errors.o $(BUILD_DIR)/render.o $(BUILD_DIR)/stats.o $(BUILD_DIR)/background.o $(LDLIBS)

$(BUILD_DIR)/shutdown: tests/shutdown.c $(BUILD_DIR)/damage.o $(BUILD_DIR)/stats.o src/main.c $(wildcard src/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ $< $(BUILD_DIR)/damage.o $(BUILD_DIR)/stats.o $(LDLIBS)

$(BUILD_DIR)/egl_config: tests/egl_config.c $(BUILD_DIR)/damage.o $(BUILD_DIR)/stats.o src/egl.c $(wildcard src/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ $< $(BUILD_DIR)/damage.o $(BUILD_DIR)/stats.o $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

-include $(DEPS)

test: $(BUILD_DIR)/window_lifecycle $(BUILD_DIR)/shutdown $(BUILD_DIR)/rendering $(BUILD_DIR)/background $(BUILD_DIR)/egl_config
	$(BUILD_DIR)/window_lifecycle remap
	$(BUILD_DIR)/window_lifecycle bordered-move
	$(BUILD_DIR)/window_lifecycle resize
	$(BUILD_DIR)/window_lifecycle import-failure
	$(BUILD_DIR)/window_lifecycle vanished-import
	$(BUILD_DIR)/window_lifecycle reused-xid
	$(BUILD_DIR)/window_lifecycle retired-damage
	$(BUILD_DIR)/window_lifecycle partial-failure
	$(BUILD_DIR)/window_lifecycle counter-deltas
	$(BUILD_DIR)/window_lifecycle stale-damage-visible
	$(BUILD_DIR)/window_lifecycle stale-damage-occluded
	$(BUILD_DIR)/window_lifecycle mixed-hidden-visible-damage
	$(BUILD_DIR)/window_lifecycle damage-error-matching
	$(BUILD_DIR)/window_lifecycle damage-create-failure
	$(BUILD_DIR)/window_lifecycle counter-accuracy
	$(BUILD_DIR)/window_lifecycle trap-errors
	$(BUILD_DIR)/window_lifecycle client-replacement
	$(BUILD_DIR)/window_lifecycle batch-ordering
	$(BUILD_DIR)/window_lifecycle stable-second-reconcile
	$(BUILD_DIR)/window_lifecycle geometry-visuals
	$(BUILD_DIR)/window_lifecycle subscription-preservation
	$(BUILD_DIR)/window_lifecycle vanished-child
	$(BUILD_DIR)/window_lifecycle filtered-empty-children
	$(BUILD_DIR)/window_lifecycle failure-after-collection
	$(BUILD_DIR)/window_lifecycle connection-loss
	$(BUILD_DIR)/window_lifecycle xcb-protocol-errors
	$(BUILD_DIR)/window_lifecycle xcb-reply-lifecycle
	$(BUILD_DIR)/window_lifecycle fullscreen-policy-invalidation
	$(BUILD_DIR)/window_lifecycle fullscreen-acceptance
	$(BUILD_DIR)/window_lifecycle fullscreen-rejections
	$(BUILD_DIR)/window_lifecycle fullscreen-popups
	$(BUILD_DIR)/window_lifecycle fullscreen-bypassed-reconcile
	$(BUILD_DIR)/window_lifecycle fullscreen-two-phase-restoration
	$(BUILD_DIR)/window_lifecycle fullscreen-intervening-event
	$(BUILD_DIR)/window_lifecycle fullscreen-failures
	$(BUILD_DIR)/shutdown
	$(BUILD_DIR)/egl_config
	$(BUILD_DIR)/rendering coalesced-damage
	$(BUILD_DIR)/rendering fullscreen-entry-recovery
	$(BUILD_DIR)/rendering history
	$(BUILD_DIR)/rendering opacity-threshold
	$(BUILD_DIR)/rendering opacity-change
	$(BUILD_DIR)/rendering empty-shape
	$(BUILD_DIR)/rendering shape-uncover
	$(BUILD_DIR)/rendering restack
	$(BUILD_DIR)/rendering offscreen-damage-skip
	$(BUILD_DIR)/rendering empty-damage-no-swap
	$(BUILD_DIR)/rendering expose-repair
	$(BUILD_DIR)/rendering hidden-to-visible-repaint
	$(BUILD_DIR)/rendering branch-counters
	$(BUILD_DIR)/rendering shape-clip-restoration
	$(BUILD_DIR)/rendering opaque-blending-bypass
	$(BUILD_DIR)/rendering bounded-presentation-damage
	$(BUILD_DIR)/rendering static-geometry-reuse
	$(BUILD_DIR)/background startup-primary
	$(BUILD_DIR)/background fallback-candidate
	$(BUILD_DIR)/background malformed-properties
	$(BUILD_DIR)/background invalid-geometry
	$(BUILD_DIR)/background import-failure
	$(BUILD_DIR)/background same-xid-refresh
	$(BUILD_DIR)/background replacement-candidate
	$(BUILD_DIR)/background coalesced-events
	$(BUILD_DIR)/background tiling-quads
	$(BUILD_DIR)/background repeated-release
	$(BUILD_DIR)/background zero-external-free
	$(BUILD_DIR)/background import-fallback
	$(BUILD_DIR)/background tiny-tile
	python3 -m unittest discover -s tests -p 'test_build_selection.py'
	python3 -m unittest discover -s tests -p 'test_benchmark_memory.py'

check: all test
	python3 benchmarks/run.py verify --quick

smoke: all
	python3 benchmarks/run.py bench --smoke

bench-quick: all
	python3 benchmarks/run.py bench --quick

run: $(BUILD_DIR)/slopcom
	./slopcom

clean:
	rm -rf build slopcom tools/fixture tests/window_lifecycle tests/shutdown tests/rendering tests/background src/*.o src/*.d

.PHONY: all run clean test check smoke bench-quick slopcom
