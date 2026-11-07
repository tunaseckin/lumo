CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

# On macOS, lumo gets the gfx.* graphics module (system frameworks only).
# Elsewhere it builds from lumo.c alone and gfx.window reports that graphics aren't available.
ifeq ($(shell uname),Darwin)
GFX = -DLUMO_GFX gfx_macos.m -framework Cocoa -framework QuartzCore -framework ImageIO
endif

lumo: lumo.c gfx_macos.m lumo_src.h
	$(CC) $(CFLAGS) -DLUMO_HAVE_SRC -o lumo lumo.c $(GFX) -lm

# lumo carries its own source so `lumo build --fast` can compile programs to machine code
lumo_src.h: lumo.c gfx_macos.m
	xxd -i lumo.c > lumo_src.h
	xxd -i gfx_macos.m >> lumo_src.h

test: lumo
	./tests/run.sh

install: lumo
	install -d $(PREFIX)/bin
	install lumo $(PREFIX)/bin/lumo

clean:
	rm -f lumo lumo_src.h

.PHONY: test install clean
