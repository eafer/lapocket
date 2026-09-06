CC = gcc
SHELL = /bin/bash
TARGET = lapocket

ifdef EMCC
  CC = $(EMCC)
  TARGET = index.js
  SDL3_CFLAGS = -DHAVE_SDL -DHAVE_PNG
  SDL3_LIBS = -sUSE_SDL=3 -sUSE_LIBPNG
  EMCC_FLAGS = --embed-file firmware.bin --pre-js pre.js
else
  PKGCONF_ERR = $(shell pkgconf --about >&/dev/null; echo $$?)
  ifeq ($(PKGCONF_ERR),0)
    SDL3_ERR = $(shell pkgconf --exists sdl3 >&/dev/null; echo $$?)
    ifeq ($(SDL3_ERR),0)
      SDL3_CFLAGS = $(shell pkgconf --cflags sdl3) -DHAVE_SDL
      SDL3_LIBS = $(shell pkgconf --libs sdl3)
      PNG_ERR = $(shell pkgconf --exists libpng >&/dev/null; echo $$?)
      ifeq ($(PNG_ERR),0)
        PNG_CFLAGS = $(shell pkgconf --cflags libpng) -DHAVE_PNG
        PNG_LIBS = $(shell pkgconf --libs libpng)
      else
        $(info Failed to find libpng, overlays will not work...)
      endif
    else
      $(info Failed to find sdl3, building headless...)
    endif
  else
    $(info Failed to find pkgconf, building headless...)
  endif
endif

CFLAGS = -Wall -Wextra -Wno-unused-parameter -O3 $(SDL3_CFLAGS) $(PNG_CFLAGS) $(EMCC_FLAGS)
# TODO: enable unused parameter warning

$(TARGET): lapocket.c
	$(CC) $(CFLAGS) -o $(TARGET) lapocket.c $(SDL3_LIBS) $(PNG_LIBS)

clean:
	rm -rf *.o index.js index.wasm lapocket
