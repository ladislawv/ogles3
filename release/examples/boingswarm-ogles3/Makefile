# Boing Swarm for OpenGL ES 3.1, with bebbo's amiga-gcc (clib2):  make
# From the SDK's examples/boingswarm-ogles3/ drawer; the headers and import
# libraries are in ../../include and ../../lib.
#
# The shaders' sources are in shaders/ (GLSL ES 3.00 and 3.10);
# boingswarm-ogles3_shaders.h holds them as C strings (make it again with
# "make header" after editing them: python3). The program needs their
# compiled forms (.v3dp/.v3dw) in a shaders/ drawer beside it, or the shader
# compiler: the release ships them, with every shader's record (.v3ds), which
# keep an installed compiler from starting. "make shaders" makes them with
# the offline compiler of the ogles3 source (OGLES3, V3DGLSLC) into
# build/v3dp.

SDK      ?= ../..
CC       ?= m68k-amigaos-gcc
# -fbbb=...: bebbo's optimiser without "e" (it breaks float-to-int rounding).
CFLAGS   ?= -O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall
LIBDIR   ?= $(SDK)/lib
OUTDIR   ?= .
BUILD    ?= build/v3dp
OGLES3   ?= /mnt/c/GitHub/ogles3
V3DGLSLC ?= $(HOME)/ogles3-build/v3dglslc/host/v3dglslc

DRAWS    := shaders/boid.vert shaders/boid.frag shaders/floor.vert shaders/floor.frag \
            shaders/screen.vert shaders/trail.frag \
            shaders/blur.frag shaders/comp.frag shaders/es2boid.vert shaders/es2boid.frag \
            shaders/es2floor.vert shaders/es2floor.frag shaders/text.vert shaders/text.frag
SHADERS  := $(DRAWS) shaders/count.comp shaders/scan.comp shaders/scatter.comp shaders/wave.comp \
            shaders/sim.comp
# the draws' pairs, then each compute program
PAIRS    := boid.vert boid.frag floor.vert floor.frag \
            screen.vert trail.frag screen.vert blur.frag screen.vert comp.frag \
            es2boid.vert es2boid.frag es2floor.vert es2floor.frag text.vert text.frag \
            count.comp scan.comp scatter.comp wave.comp sim.comp

all: $(OUTDIR)/boingswarm-ogles3

$(OUTDIR)/boingswarm-ogles3: boingswarm-ogles3.c boingswarm-ogles3_shaders.h
	$(CC) $(CFLAGS) -I$(SDK)/include boingswarm-ogles3.c -L$(LIBDIR) -logles3 -lm881 -o $@

header: $(SHADERS) tools/embed_shaders.py
	python3 -I tools/embed_shaders.py boingswarm-ogles3_shaders.h $(SHADERS)

shaders: $(SHADERS)
	rm -rf $(BUILD) && mkdir -p $(BUILD)
	cd shaders && $(V3DGLSLC) -o $(abspath $(BUILD)) $(PAIRS)
	python3 -I "$(OGLES3)/v3dp/v3dp_hashname.py" $(BUILD)/*
	python3 -I "$(OGLES3)/v3dp/v3ds_record.py" $(BUILD) $(SHADERS)

clean:
	rm -rf boingswarm-ogles3 build

.PHONY: all header shaders clean
