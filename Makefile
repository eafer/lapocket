CC = gcc
SHELL = /bin/bash
TARGET = lapocket

ifdef EMCC
  CC = $(EMCC)
  TARGET = index.html
  SDL3_CFLAGS = -DHAVE_SDL
  SDL3_LIBS = -sUSE_SDL=3
  EMCC_FLAGS = --embed-file firmware.bin --pre-js pre.js
else
  PKGCONF_ERR = $(shell pkgconf --about >&/dev/null; echo $$?)
  ifeq ($(PKGCONF_ERR),0)
    SDL3_ERR = $(shell pkgconf --exists sdl3 >&/dev/null; echo $$?)
    ifeq ($(SDL3_ERR),0)
      SDL3_CFLAGS = $(shell pkgconf --cflags sdl3) -DHAVE_SDL
      SDL3_LIBS = $(shell pkgconf --libs sdl3)
    else
      $(info Failed to find sdl3, building headless...)
    endif
  else
    $(info Failed to find pkgconf, building headless...)
  endif
endif

CFLAGS = -Wall -Wextra -Wno-unused-parameter -O3 $(SDL3_CFLAGS) $(EMCC_FLAGS)
# TODO: enable unused parameter warning

$(TARGET): lapocket.c
	$(CC) $(CFLAGS) -o $(TARGET) lapocket.c $(SDL3_LIBS)

clean:
	rm -rf *.o index.html index.js index.wasm lapocket
