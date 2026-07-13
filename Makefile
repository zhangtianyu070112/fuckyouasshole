# =============================================================================
#  航空驾驶舱模拟控制系统 — Flight Cockpit Simulation Control System
#  Build System (MSYS2 / MinGW-w64 + GCC)
# =============================================================================

# Compiler & flags
CC       := gcc
BIN      := cockpit.exe

# pkg-config for SDL2 libraries (fails gracefully if not installed)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs   sdl2 SDL2_image SDL2_ttf 2>/dev/null)
ifeq ($(SDL_LIBS),)
  SDL_LIBS := -lmingw32 -lSDL2main -lSDL2 -lSDL2_image -lSDL2_ttf
endif

# Common flags
WARN_FLAGS   := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes
OPT_FLAGS    := -O2
DBG_FLAGS    := -g -O0 -DDEBUG

# Include paths: src/ for "data/xxx.h" style, instruments/ for instrument.h,
# build/<module>/ for module headers (config.h, app.h, event.h, thread.h)
MOD_NAMES    := config event main thread app ai
MOD_INC      := $(foreach mod,$(MOD_NAMES),-Ibuild/$(mod))
INC_FLAGS    := -Isrc -Isrc/ai -Iinstruments $(MOD_INC) $(SDL_CFLAGS)
LDFLAGS      := -lws2_32 -lm -lopengl32 -lglu32 $(SDL_LIBS)

# If SDL2_gfx is available, include it
SDL_GFX_CFLAGS := $(shell pkg-config --cflags SDL2_gfx 2>/dev/null)
SDL_GFX_LIBS   := $(shell pkg-config --libs   SDL2_gfx 2>/dev/null)
ifeq ($(SDL_GFX_LIBS),)
  SDL_GFX_LIBS := -lSDL2_gfx
endif
ifneq ($(SDL_GFX_CFLAGS),)
  INC_FLAGS += $(SDL_GFX_CFLAGS)
  LDFLAGS   += $(SDL_GFX_LIBS)
  CFLAGS_GFX := -DHAS_SDL2_GFX
endif

# libcurl (HTTPS for 高德 REST APIs)
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs   libcurl 2>/dev/null)
ifeq ($(CURL_LIBS),)
  CURL_LIBS := -lcurl
endif
INC_FLAGS += $(CURL_CFLAGS)
LDFLAGS   += $(CURL_LIBS)

# cJSON (JSON parsing for weather API)
CJSON_CFLAGS := $(shell pkg-config --cflags cjson 2>/dev/null)
CJSON_LIBS   := $(shell pkg-config --libs   cjson 2>/dev/null)
ifeq ($(CJSON_LIBS),)
  CJSON_LIBS := -lcjson
endif
INC_FLAGS += $(CJSON_CFLAGS)
LDFLAGS   += $(CJSON_LIBS)

# Work around GCC 16.1.0 MinGW ICE in AVX-512 intrinsics headers.
# The bug is triggered when SDL2 → intrin.h → immintrin.h → avx512*intrin.h
# is pulled in. Disabling AVX-512 ISA extensions skips the broken headers.
CFLAGS_AVX_FIX := -mno-avx512f -mno-avx512vl -mno-avx512bw -mno-avx512dq

CFLAGS := $(WARN_FLAGS) $(OPT_FLAGS) $(INC_FLAGS) $(CFLAGS_GFX) $(CFLAGS_AVX_FIX)

# -----------------------------------------------------------------------------
# Source files
# -----------------------------------------------------------------------------

# Existing subdirectory modules — .o next to .c in src/<dir>/
SRC_DIRS := src/utils src/net src/data src/ds src/map src/audio src/cabin_old
SRCS     := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS     := $(SRCS:.c=.o)
DEPS     := $(SRCS:.c=.d)

# Root-level modules moved into src/<module>/ — .o in build/<module>/
MOD_SRCS  := $(foreach mod,$(MOD_NAMES),$(wildcard src/$(mod)/*.c))
MOD_OBJS  := $(patsubst src/%.c,build/%.o,$(MOD_SRCS))
MOD_DEPS  := $(MOD_OBJS:.o=.d)

# Instrument source directories — compiled to build/<instrument>/
INST_NAMES := EICAS FMC ND PFD
INST_SRCS  := $(foreach inst,$(INST_NAMES),$(wildcard instruments/$(inst)/*.c))
INST_OBJS  := $(patsubst instruments/%.c,build/%.o,$(INST_SRCS))
INST_DEPS  := $(INST_OBJS:.o=.d)

# Combined lists
OBJS_ALL := $(OBJS) $(MOD_OBJS) $(INST_OBJS)
DEPS_ALL := $(DEPS) $(MOD_DEPS) $(INST_DEPS)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
.PHONY: all clean run debug help

all: $(BIN)

$(BIN): $(OBJS_ALL)
	@echo "  LINK    $@"
	@$(CC) $(OBJS_ALL) -o $@ $(LDFLAGS)
	@echo "  ✓ Build complete: $(BIN)"

# Pattern rule for existing subdirectory sources — .o next to .c
%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Pattern rule for root-level module sources — .o in build/<module>/
build/%.o: src/%.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Pattern rule for instrument sources — .o in build/<instrument>/
build/%.o: instruments/%.c
	@echo "  CC      $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Include auto-generated dependency files
-include $(DEPS_ALL)

debug: CFLAGS := $(WARN_FLAGS) $(DBG_FLAGS) $(INC_FLAGS) $(CFLAGS_GFX)
debug: clean all
	@echo "  ✓ Debug build complete (no optimization, symbols enabled)"

run: all
	@echo "  Running $(BIN)..."
	@./$(BIN)

clean:
	@echo "  Cleaning..."
	@rm -f $(OBJS) $(DEPS) $(BIN)
	@rm -f $(MOD_OBJS) $(MOD_DEPS)
	@rm -f $(INST_OBJS) $(INST_DEPS)
	@echo "  ✓ Clean complete"

help:
	@echo "Flight Cockpit Simulation System — Build Help"
	@echo "=============================================="
	@echo "  make          — Release build"
	@echo "  make debug    — Debug build with -g -O0"
	@echo "  make run      — Build & run"
	@echo "  make clean    — Remove all build artifacts"
	@echo "=============================================="
	@echo "Required: MSYS2 with mingw-w64-gcc, SDL2, SDL2_image, SDL2_ttf"
	@echo "Optional: SDL2_gfx"
