# Juggler RT for OpenGL ES 3.1, with bebbo's amiga-gcc (clib2):  make
# From the SDK's examples/juggler-ogles3/ drawer; the headers and import
# libraries are in ../../include and ../../lib.
#
# The shaders' sources are in shaders/ (GLSL ES 1.00, 3.00 and 3.10) and
# the original scenes in scenes/; juggler-ogles3_shaders.h holds them as C
# strings (make it again with "make header" after editing them: python3).
# The program needs the shaders' compiled forms (.v3dp/.v3dw) in a shaders/
# drawer beside it, or the shader compiler: "make shaders" makes them with
# the offline compiler of the ogles3 source (OGLES3, V3DGLSLC) into
# build/v3dp. tools/pccheck.sh runs the renderer on a PC (Mesa).

SDK      ?= ../..
CC       ?= m68k-amigaos-gcc
# -fbbb=...: bebbo's optimiser without "e" (it breaks float-to-int rounding).
CFLAGS   ?= -O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall
LIBDIR   ?= $(SDK)/lib
OUTDIR   ?= .
BUILD    ?= build/v3dp
OGLES3   ?= /mnt/c/GitHub/ogles3
V3DGLSLC ?= $(HOME)/ogles3-build/v3dglslc/host/v3dglslc

SHADERS  := shaders/animate.comp shaders/shadow.comp shaders/trace.comp shaders/ham.comp shaders/screen.vert shaders/show.frag \
            shaders/es2.vert shaders/es2trace.frag shaders/es2show.frag \
            shaders/text.vert shaders/text.frag
SCENES   := scenes/robot.dat scenes/ele.dat scenes/dragon.dat
# each compute program, then the draws' pairs
PAIRS    := animate.comp shadow.comp trace.comp ham.comp screen.vert show.frag es2.vert es2trace.frag es2.vert es2show.frag text.vert text.frag

all: $(OUTDIR)/juggler-ogles3

$(OUTDIR)/juggler-ogles3: juggler-ogles3.c juggler_scene.h juggler_gl.h juggler-ogles3_shaders.h
	$(CC) $(CFLAGS) -I$(SDK)/include juggler-ogles3.c -L$(LIBDIR) -logles3 -lm881 -o $@

header: $(SHADERS) $(SCENES) tools/embed_shaders.py
	python3 -I tools/embed_shaders.py juggler-ogles3_shaders.h $(SHADERS) $(SCENES)

shaders: $(SHADERS)
	rm -rf $(BUILD) && mkdir -p $(BUILD)
	cd shaders && $(V3DGLSLC) -o $(abspath $(BUILD)) $(PAIRS)
	python3 -I "$(OGLES3)/v3dp/v3dp_hashname.py" $(BUILD)/*
	python3 -I "$(OGLES3)/v3dp/v3ds_record.py" $(BUILD) $(SHADERS)

clean:
	rm -rf juggler-ogles3 build

.PHONY: all header shaders clean
