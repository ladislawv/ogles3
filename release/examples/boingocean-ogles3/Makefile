# Boing Ocean for OpenGL ES 3, with bebbo's amiga-gcc (clib2):  make
# From the SDK's examples/boingocean-ogles3/ drawer; the headers and import
# libraries are in ../../include and ../../lib, the ES 2.0 way's shaders in
# ../boingocean_shaders.h (Boing Ocean's own, byte for byte: the release's
# Examples/shaders/ has their binaries).
#
# The ES 3 way's shaders are in shaders/ (GLSL ES 3.00);
# boingocean-ogles3_shaders.h holds them as C strings (make it again with
# "make header" after editing them: python3). The program needs their
# compiled forms (.v3dp/.v3dw) in a shaders/ drawer beside it, or the shader
# compiler: the release ships them, with the shaders' records (.v3ds). "make
# shaders" makes all three with the offline compiler of the ogles3 source
# (OGLES3, V3DGLSLC) into build/v3dp.

SDK      ?= ../..
CC       ?= m68k-amigaos-gcc
# -fbbb=...: bebbo's optimiser without "e" (it breaks float-to-int rounding).
CFLAGS   ?= -O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall
LIBDIR   ?= $(SDK)/lib
OUTDIR   ?= .
BUILD    ?= build/v3dp
OGLES3   ?= /mnt/c/GitHub/ogles3
V3DGLSLC ?= $(HOME)/ogles3-build/v3dglslc/host/v3dglslc

SHADERS  := shaders/screen3.vert shaders/height3.frag shaders/sky3.frag \
            shaders/ocean3.vert shaders/ocean3.frag shaders/oceanfar3.frag shaders/ripple3.frag \
            shaders/caust3.frag shaders/copy3.frag shaders/bright3.frag shaders/blur3.frag shaders/final3.frag
# vertex + fragment shader of each program, as link_program() pairs them
PAIRS    := screen3.vert height3.frag screen3.vert sky3.frag \
            ocean3.vert ocean3.frag ocean3.vert oceanfar3.frag \
            screen3.vert ripple3.frag screen3.vert caust3.frag \
            screen3.vert copy3.frag screen3.vert bright3.frag \
            screen3.vert blur3.frag screen3.vert final3.frag

all: $(OUTDIR)/boingocean-ogles3

$(OUTDIR)/boingocean-ogles3: boingocean-ogles3.c boingocean-ogles3_shaders.h
	$(CC) $(CFLAGS) -I$(SDK)/include -I.. boingocean-ogles3.c -L$(LIBDIR) -logles3 -lm881 -o $@

# The far water's shader: ocean3.frag with FAR defined (after its #version).
shaders/oceanfar3.frag: shaders/ocean3.frag
	sed '1a #define FAR 1' $< > $@

header: $(SHADERS) tools/embed_shaders.py
	python3 -I tools/embed_shaders.py boingocean-ogles3_shaders.h $(SHADERS)

shaders: $(SHADERS)
	rm -rf $(BUILD) && mkdir -p $(BUILD)
	cd shaders && $(V3DGLSLC) -o $(abspath $(BUILD)) $(PAIRS)
	python3 -I $(OGLES3)/v3dp/v3dp_hashname.py $(BUILD)/*
	python3 -I $(OGLES3)/v3dp/v3ds_record.py $(BUILD) $(SHADERS)

clean:
	rm -rf boingocean-ogles3 build

.PHONY: all header shaders clean
