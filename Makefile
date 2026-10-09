# PicoDeck DOOM (doomgeneric) Native App Build

CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -std=gnu99 \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -O3 -g -Wall -Wextra -Wno-unused-parameter \
          -I. \
          -Isdk/native \
          -Isrc/doomgeneric \
          -DDOOMGENERIC_RESX=320 \
          -DDOOMGENERIC_RESY=200 \
          -DFEATURE_SOUND
# Dev-only flags (add to CFLAGS when needed):
#   -DOPL_CAPTURE  — dump OPL register writes to SD (SD writes on the Core 1
#                    audio path; starves the mixer — never ship enabled)
#   -DMUS_DEBUG    — per-voice/per-render MUS logging (same starvation issue)
LDFLAGS = -T sdk/native/linker.ld \
          -Wl,--entry=picodeck_main \
          -Wl,-pie \
          -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments \
          -nostartfiles -nodefaultlibs -lc -lm -lgcc

# All .c files in doomgeneric EXCEPT the other platform files and sound drivers we don't have
EXCLUDE_SRCS = src/doomgeneric/doomgeneric_%.c \
               src/doomgeneric/dummy.c \
               src/doomgeneric/icon.c \
               src/doomgeneric/i_allegro%.c \
               src/doomgeneric/i_sdl%.c \
               src/doomgeneric/i_cdmus.c \
               src/doomgeneric/i_joystick.c

DOOM_SRCS = $(filter-out $(EXCLUDE_SRCS), $(wildcard src/doomgeneric/*.c))
COMMON_SRCS = dg_picodeck.c stubs.c i_picodeck_sound.c opl.c mus_player.c opl_capture.c $(DOOM_SRCS)
SRCS    = $(COMMON_SRCS) stubs_newlib.c

TARGET  = main.elf

.PHONY: all clean web-stage

all: $(TARGET)

$(TARGET): $(SRCS) sdk/native/linker.ld sdk/native/os.h sdk/native/app_abi.h
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	arm-none-eabi-strip $@
	arm-none-eabi-size $@

# The browser build (PicoDeck/web-sim runs it on picodeck.net/try): the same
# sources as a WebAssembly side module, with Emscripten's C library in place
# of stubs_newlib.c and exit() routed back to picodeck_main (web_exit.c).
# Needs Emscripten (EMSDK; the version PicoDeck/web-sim pins).
EMSDK  ?= $(HOME)/emsdk
EMCC    = $(EMSDK)/upstream/emscripten/emcc
WASM_CFLAGS = -O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
              -I. -Isdk/native -Isrc/doomgeneric \
              -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND \
              -Dexit=dg_web_exit
WASM_LDFLAGS = -sSIDE_MODULE=2 -sASYNCIFY -sEXPORTED_FUNCTIONS=_picodeck_main
WASM_SRCS = $(COMMON_SRCS) web_exit.c

main.wasm: $(WASM_SRCS) sdk/native/os.h sdk/native/app_abi.h
	$(EMCC) $(WASM_CFLAGS) $(WASM_SRCS) $(WASM_LDFLAGS) -o $@

# What web-sim's test build bundles (make test DOOM=<this checkout>): the
# release zip's files plus main.wasm.
web-stage: main.elf main.wasm
	rm -rf build-web/doom
	mkdir -p build-web/doom
	cp app.json main.elf main.wasm doom1.wad icon.png build-web/doom/

clean:
	rm -f $(TARGET) main.wasm
	rm -rf build-web
