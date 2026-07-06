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
INC_FLAGS    := -Isrc $(SDL_CFLAGS)
LDFLAGS      := -lws2_32 -lm $(SDL_LIBS)

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

CFLAGS := $(WARN_FLAGS) $(OPT_FLAGS) $(INC_FLAGS) $(CFLAGS_GFX)

# -----------------------------------------------------------------------------
# Source files (auto-discovered)
# -----------------------------------------------------------------------------
SRC_DIRS := src src/utils src/config src/net src/instruments src/data src/ds src/map src/audio src/cabin
SRCS     := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS     := $(SRCS:.c=.o)
DEPS     := $(SRCS:.c=.d)

# -----------------------------------------------------------------------------
# Targets
# -----------------------------------------------------------------------------
.PHONY: all clean run debug help

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LINK    $@"
	@$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "  ✓ Build complete: $(BIN)"

# Pattern rule for .o + .d generation
%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Include auto-generated dependency files
-include $(DEPS)

debug: CFLAGS := $(WARN_FLAGS) $(DBG_FLAGS) $(INC_FLAGS) $(CFLAGS_GFX)
debug: clean all
	@echo "  ✓ Debug build complete (no optimization, symbols enabled)"

run: all
	@echo "  Running $(BIN)..."
	@./$(BIN)

clean:
	@echo "  Cleaning..."
	@rm -f $(OBJS) $(DEPS) $(BIN)
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
