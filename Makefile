# Native build (macOS, Apple Silicon). Order of events for a fresh checkout:
#   scripts/prepare.sh "<your disc>.iso"   extract the disc, analyse default.xbe, translate it to build/lifted.c
#   make                                    compile everything into build/stw
#   ./build/stw                             the launcher opens; pick resolution, controls, and (first time) your disc image
# lifted.c comes from tools/lift.py (slow to compile: ~90 s at -O1), so it is kept as its own object and only rebuilt when it changes.
CC      = clang
CFLAGS  = -w -fobjc-arc -ffp-contract=off -I runtime -I $(BUILD)
BUILD   = build
# Runtime sources (the "console"). Everything that depends on the game is generated into build/ and kept out of these.
RT_SRCS = runtime/main.c runtime/kernel.c runtime/kobj.c runtime/kfile.c runtime/lib.c runtime/nv2a.c runtime/xapi.c runtime/audio.c runtime/display.c runtime/display_metal.m runtime/input.m runtime/gpu_metal.m runtime/vsh.c runtime/psh.c runtime/xiso.c runtime/paths.c runtime/pad.m runtime/launcher_pad.m runtime/gamelib.c runtime/prepare.m
RT_HDRS = runtime/kernel.h runtime/rt.h runtime/lib.h runtime/gametables.h runtime/pad.h runtime/input.h runtime/paths.h
RT_LIBS = -lpthread -framework Cocoa -framework Metal -framework QuartzCore -framework GameController -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework CoreHaptics -framework CoreGraphics
RTFLAGS = $(CFLAGS) -DSTW_RUNTIME -O1 -g

all: $(BUILD)/stw

# The game version this build supports (a hash of default.xbe: the discs the launcher accepts, and what the app prepares for).
$(BUILD)/xbe_id.h: runtime/data/supported_xbe.sha256
	@mkdir -p $(BUILD)
	@printf '#define STW_XBE_SHA256 "%s"\n' "$$(cat runtime/data/supported_xbe.sha256)" > $@

$(BUILD)/libstubs.c: tools/gen_libstubs.py $(BUILD)/lifted.c.json runtime/data/libnames.txt
	.venv/bin/python tools/gen_libstubs.py game/default.xbe $(BUILD)/lifted.c.json $@

$(BUILD)/ktable.c: tools/gen_kernel.py runtime/data/xboxkrnl.def runtime/kernel.c runtime/kobj.c runtime/kfile.c
	.venv/bin/python tools/gen_kernel.py game/default.xbe $@

$(BUILD)/lifted.o: $(BUILD)/lifted.c runtime/rt.h
	$(CC) $(CFLAGS) -O1 -c $< -o $@

# Developer build: the translated code is linked into the program.
$(BUILD)/gametables.o: runtime/gametables.c $(RT_HDRS)
	$(CC) $(CFLAGS) -O1 -c $< -o $@

$(BUILD)/stw: $(RT_SRCS) $(RT_HDRS) $(BUILD)/ktable.c $(BUILD)/libstubs.c $(BUILD)/lifted.o $(BUILD)/gametables.o $(BUILD)/xbe_id.h
	$(CC) $(RTFLAGS) -DSTW_STATIC -o $@ $(RT_SRCS) $(BUILD)/ktable.c $(BUILD)/libstubs.c $(BUILD)/lifted.o $(BUILD)/gametables.o $(RT_LIBS)

# App build: the runtime alone (no game-derived code); it loads <data dir>/lifted/.../lifted.dylib, built on the user's machine.
$(BUILD)/stw-app: $(RT_SRCS) $(RT_HDRS) $(BUILD)/xbe_id.h
	$(CC) $(RTFLAGS) -Wl,-export_dynamic -o $@ $(RT_SRCS) $(RT_LIBS)

# The translated code as a library (what the app builds on first run; this target is for testing the same path from a checkout).
$(BUILD)/lifted.dylib: $(BUILD)/lifted.c $(BUILD)/ktable.c $(BUILD)/libstubs.c runtime/gametables.c $(RT_HDRS)
	$(CC) $(CFLAGS) -O1 -shared -undefined dynamic_lookup -o $@ $(BUILD)/lifted.c $(BUILD)/ktable.c $(BUILD)/libstubs.c runtime/gametables.c

app: $(BUILD)/stw-app
	scripts/make_app.sh

.PHONY: help clean prepare app
help:
	@echo "scripts/prepare.sh <disc.iso>   generate the sources from your own copy of the game (once)"
	@echo "make                            build build/stw"
	@echo "make clean                      remove build/ (keeps game/ and analysis/)"
prepare:
	@[ -n "$(ISO)" ] || { echo "usage: make prepare ISO=/path/to/disc.iso"; exit 1; }
	scripts/prepare.sh "$(ISO)"
clean:
	rm -rf $(BUILD)
