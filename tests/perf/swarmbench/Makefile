# swarmbench (Boing Swarm's step 1), with bebbo's amiga-gcc: make
# (B: this tree's m68k build, its import library and generated headers).
# swarmbench_shaders.h holds shaders/ as C strings ("make header" after
# editing them); "make shaders" compiles them with the offline compiler into
# build/v3dp, for a shaders/ drawer beside the program.

REPO     ?= ../../..
B        ?= $(HOME)/ogles3-build/m68k
CC       ?= $(HOME)/amiga-gcc-install/bin/m68k-amigaos-gcc
CFLAGS   ?= -std=gnu99 -O2 -mcpu=68020 -m68881 -mcrt=clib2 -fno-builtin -Wall
OUTDIR   ?= .
BUILD    ?= build/v3dp
V3DGLSLC ?= $(HOME)/ogles3-build/v3dglslc/host/v3dglslc

SHADERS  := shaders/boid.vert shaders/boid.frag shaders/free.comp shaders/count.comp \
            shaders/scan.comp shaders/scatter.comp shaders/flock.comp
# the draw's pair, then each compute program
PAIRS    := boid.vert boid.frag free.comp count.comp scan.comp scatter.comp flock.comp

all: $(OUTDIR)/swarmbench

$(OUTDIR)/swarmbench: swarmbench.c swarmbench_shaders.h
	$(CC) $(CFLAGS) -I$(REPO)/include -I$(B)/gen/include swarmbench.c -L$(B) -logles3 -lm881 -o $@

header: $(SHADERS)
	python3 -I $(REPO)/release/examples/boing2026-ogles3/tools/embed_shaders.py swarmbench_shaders.h $(SHADERS)

shaders: $(SHADERS)
	rm -rf $(BUILD) && mkdir -p $(BUILD)
	cd shaders && $(V3DGLSLC) -o $(abspath $(BUILD)) $(PAIRS)
	python3 -I "$(REPO)/v3dp/v3dp_hashname.py" $(BUILD)/*
	python3 -I "$(REPO)/v3dp/v3ds_record.py" $(BUILD) shaders/boid.vert shaders/boid.frag

clean:
	rm -rf swarmbench build

.PHONY: all header shaders clean
