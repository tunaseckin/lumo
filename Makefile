CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

# lumo gets the gfx.* graphics module from the system's own libraries:
# Cocoa on macOS (gfx_macos.m), Win32 on Windows and X11 on Linux (gfx.c, X11 loaded at runtime).
ifeq ($(shell uname),Darwin)
GFX = -DLUMO_GFX gfx_macos.m -framework Cocoa -framework QuartzCore -framework ImageIO
else ifeq ($(OS),Windows_NT)
# lumo.res: a manifest so file names with ç, ş, ğ work, and the screen's real resolution is used
GFX = -DLUMO_GFX gfx.c -lgdi32 -lwinmm lumo.res -static
RES = lumo.res
else
GFX = -DLUMO_GFX gfx.c -ldl
endif

lumo: lumo.c gfx_macos.m gfx.c gfx_font.h lumo_src.h $(RES)
	$(CC) $(CFLAGS) -DLUMO_HAVE_SRC -o lumo lumo.c $(GFX) -lm

# lumo carries its own source so `lumo build --fast` can compile programs to machine code
lumo_src.h: lumo.c gfx_macos.m gfx.c gfx_font.h
	xxd -i lumo.c > lumo_src.h
	xxd -i gfx_macos.m >> lumo_src.h
	xxd -i gfx.c >> lumo_src.h
	xxd -i gfx_font.h >> lumo_src.h

lumo.res: lumo.rc lumo.manifest
	windres lumo.rc -O coff -o lumo.res

test: lumo
	./tests/run.sh

install: lumo
	install -d $(PREFIX)/bin
	install lumo $(PREFIX)/bin/lumo

clean:
	rm -f lumo lumo.exe lumo_src.h lumo.res

.PHONY: test install clean
