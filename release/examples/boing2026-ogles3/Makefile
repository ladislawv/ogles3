# Boing 2026 for OpenGL ES 3, with bebbo's amiga-gcc (clib2):  make
# From the SDK's examples/boing2026-ogles3/ drawer; the headers and import
# libraries are in ../../include and ../../lib.
#
# The shaders' sources are in shaders/ (the *3.* ones GLSL ES 3.00; the others
# Boing 2026's own, for the ES 2.0 way, key E); boing2026-ogles3_shaders.h
# holds them as C strings (make it again with "make header" after editing
# them: python3). The program needs their compiled forms (.v3dp/.v3dw) in a
# shaders/ drawer beside it, or the shader compiler: the release ships them,
# with the shaders' records (.v3ds), which keep an installed compiler from
# starting. "make shaders" makes all three with the offline compiler of the
# ogles3 source (OGLES3, V3DGLSLC) into build/v3dp.

SDK      ?= ../..
CC       ?= m68k-amigaos-gcc
# -fbbb=...: bebbo's optimiser without "e" (it breaks float-to-int rounding).
CFLAGS   ?= -O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall
LIBDIR   ?= $(SDK)/lib
OUTDIR   ?= .
BUILD    ?= build/v3dp
OGLES3   ?= /mnt/c/GitHub/ogles3
V3DGLSLC ?= $(HOME)/ogles3-build/v3dglslc/host/v3dglslc

ES2      := shaders/ball.vert shaders/ball.frag shaders/room.vert shaders/room.frag \
            shaders/screen.vert shaders/blur.frag shaders/composite.frag \
            shaders/shadow.vert shaders/shadow.frag shaders/flare.vert shaders/flare.frag
ES3      := shaders/ball3.vert shaders/ball3.frag shaders/room3.vert shaders/room3.frag shaders/floor3.frag \
            shaders/screen3.vert shaders/down3.frag shaders/blur3.frag shaders/composite3.frag \
            shaders/mapview3.frag shaders/shadow3.vert shaders/shadow3.frag \
            shaders/flare3.vert shaders/flare3.frag shaders/spark3.vert shaders/spark3.frag \
            shaders/water3.frag shaders/caust3.vert shaders/caust3.frag \
            shaders/refl3.vert shaders/refl3.frag shaders/pool3.frag shaders/poolroom3.frag
SHADERS  := $(ES2) $(ES3)
# vertex + fragment shader of each program, as link_program() pairs them
PAIRS    := ball.vert ball.frag room.vert room.frag screen.vert blur.frag \
            screen.vert composite.frag shadow.vert shadow.frag flare.vert flare.frag \
            ball3.vert ball3.frag room3.vert room3.frag room3.vert floor3.frag screen3.vert down3.frag \
            screen3.vert blur3.frag screen3.vert composite3.frag screen3.vert mapview3.frag \
            shadow3.vert shadow3.frag flare3.vert flare3.frag spark3.vert spark3.frag \
            screen3.vert water3.frag caust3.vert caust3.frag refl3.vert refl3.frag \
            room3.vert pool3.frag room3.vert poolroom3.frag

all: $(OUTDIR)/boing2026-ogles3

$(OUTDIR)/boing2026-ogles3: boing2026-ogles3.c boing2026-ogles3_shaders.h
	$(CC) $(CFLAGS) -I$(SDK)/include boing2026-ogles3.c -L$(LIBDIR) -logles3 -lm881 -o $@

# The pool's forms of the floor's and the walls' shaders: the same source
# with POOL defined, so the dry floor's programs carry none of the water.
shaders/pool3.frag: shaders/floor3.frag
	sed '1a #define POOL' $< > $@
shaders/poolroom3.frag: shaders/room3.frag
	sed '1a #define POOL' $< > $@

header: $(SHADERS) tools/embed_shaders.py
	python3 -I tools/embed_shaders.py boing2026-ogles3_shaders.h $(SHADERS)

shaders: $(SHADERS)
	rm -rf $(BUILD) && mkdir -p $(BUILD)
	cd shaders && $(V3DGLSLC) -o $(abspath $(BUILD)) $(PAIRS)
	python3 -I $(OGLES3)/v3dp/v3dp_hashname.py $(BUILD)/*
	python3 -I $(OGLES3)/v3dp/v3ds_record.py $(BUILD) $(SHADERS)

clean:
	rm -rf boing2026-ogles3 build

.PHONY: all header shaders clean
